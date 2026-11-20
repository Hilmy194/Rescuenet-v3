#pragma once
#include "MobileModel.h"

namespace rescue_mobile {
struct CompletedSos {
  char requestId[41]{};
  uint8_t digest[32]{};
  bool nameTruncated = false;
};
class Journal {
public:
  virtual ~Journal() = default;
  virtual bool begin(uint8_t node) = 0;
  // 0=absent, 1=valid, -1=storage/corruption failure.
  virtual int load(size_t slot, Message &message) = 0;
  virtual int loadCompleted(size_t slot, CompletedSos &entry) = 0;
  virtual bool save(size_t slot, const Message &message) = 0;
  virtual bool saveCompleted(size_t slot, const CompletedSos &entry) = 0;
  virtual bool clear(size_t slot) = 0;
  virtual int loadCursor() = 0; // 0..SOS_CAPACITY-1, or -1 on corruption
  virtual bool saveCursor(uint8_t nextSlot) = 0;
};
class MobileQueue {
  struct Slot {
    Message message{};
    uint64_t order = 0;
    uint32_t lastAttempt = 0, retryAfter = 0;
    bool used = false, attemptedOnce = false, acceptedByMesh = false;
  };
  struct History {
    char requestId[41]{};
    uint8_t digest[32]{};
    uint32_t acceptedMs = 0;
    bool used = false, pending = false, nameTruncated = false;
  };
  Slot sos[SOS_CAPACITY]{};
  Slot locations[LOCATION_CAPACITY]{};
  History history[LOCATION_HISTORY]{};
  CompletedSos completed[COMPLETED_SOS_CAPACITY]{};
  Journal &journal;
  uint64_t order = 0, session = 0;
  uint32_t sequence = 0;
  uint8_t node = 0;
  size_t completedCursor = 0;
  uint8_t sosCursor = 0;
  bool healthy = false;

  Result duplicate(const uint8_t *known, const Message &m, const char *state) const {
    return memcmp(known, m.digest, 32) == 0 ? Result(200, nullptr, state, true) : Result(409, "request_conflict");
  }
  void expire(uint32_t now) {
    for (auto &entry : history) if (entry.used && !entry.pending && uint32_t(now - entry.acceptedMs) >= HISTORY_AGE_MS) entry = History{};
  }
  int historySlot(uint32_t now) const {
    int selected = -1;
    uint32_t oldest = 0;
    for (size_t i = 0; i < LOCATION_HISTORY; ++i) {
      if (!history[i].used) return int(i);
      uint32_t age = uint32_t(now - history[i].acceptedMs);
      if (!history[i].pending && (selected < 0 || age > oldest)) { selected = int(i); oldest = age; }
    }
    return selected;
  }
public:
  explicit MobileQueue(Journal &store) : journal(store) {}
  bool begin(uint8_t id, uint64_t boot) {
    healthy = false; order = 0;
    for (auto &slot : sos) slot = Slot{};
    for (auto &slot : locations) slot = Slot{};
    for (auto &entry : history) entry = History{};
    for (auto &entry : completed) entry = CompletedSos{};
    node = id; session = boot; sequence = 0; completedCursor = 0; sosCursor = 0;
    if (!id || id > 15 || !boot || !journal.begin(id)) return false;
    int restoredCursor = journal.loadCursor();
    if (restoredCursor < 0 || restoredCursor >= int(SOS_CAPACITY)) return false;
    sosCursor = uint8_t(restoredCursor);
    for (size_t i = 0; i < SOS_CAPACITY; ++i) {
      Message saved;
      int result = journal.load(i, saved);
      if (result < 0 || (result == 1 && (saved.kind != 'S' || saved.source != id))) return false;
      if (result == 1) {
        for (size_t j = 0; j < i; ++j) if (sos[j].used && strcmp(sos[j].message.requestId, saved.requestId) == 0) return false;
        sos[i].message = saved; sos[i].used = true;
      }
    }
    for (size_t i = 0; i < COMPLETED_SOS_CAPACITY; ++i) {
      if (journal.loadCompleted(i, completed[i]) < 0) return false;
      if (completed[i].requestId[0]) completedCursor = (i + 1) % COMPLETED_SOS_CAPACITY;
    }
    // Crash between durable DONE marker and deleting the pending record is safe:
    // reconcile on reboot, never retransmit an already server-ACKed SOS.
    for (auto &slot : sos) if (slot.used) {
      for (const auto &done : completed) if (done.requestId[0] && strcmp(done.requestId, slot.message.requestId) == 0) {
        if (memcmp(done.digest, slot.message.digest, 32) || !journal.clear(size_t(&slot - sos))) return false;
        slot = Slot{}; break;
      }
    }
    // Cursor is committed BEFORE a new SOS slot. Scanning its circular order
    // restores FIFO even when old low-index slots have been reused.
    for (size_t offset = 0; offset < SOS_CAPACITY; ++offset) {
      size_t i = (sosCursor + offset) % SOS_CAPACITY;
      if (sos[i].used) sos[i].order = ++order;
    }
    healthy = true;
    return true;
  }
  bool ready() const { return healthy; }
  size_t pendingSos() const { size_t count = 0; for (const auto &slot : sos) if (slot.used) ++count; return count; }
  size_t pendingLocations() const { size_t count = 0; for (const auto &slot : locations) if (slot.used) ++count; return count; }
  // SOS-first read-only queue view; only a verified STORED ACK may complete it.
  const Message *nextPending() const {
    const Slot *next = nullptr;
    for (const auto &slot : sos) if (slot.used && (!next || slot.order < next->order)) next = &slot;
    if (next) return &next->message;
    for (const auto &slot : locations) if (slot.used && (!next || slot.order < next->order)) next = &slot;
    return next ? &next->message : nullptr;
  }
  // Select a bounded set of distinct frames instead of retrying only the FIFO
  // head forever. This prevents one lost ACK from blocking every later SOS,
  // while acceptedByMesh keeps application retries from filling LoRaMesher's
  // ten-entry TX queue. Scheduling state is intentionally RAM-only: records
  // remain durable until a matching server STORED ACK is committed.
  const Message *nextForTransmit(uint32_t now, size_t meshQueueDepth, size_t maxInFlight) {
    if (!healthy || !maxInFlight || meshQueueDepth >= 8) return nullptr;
    if (meshQueueDepth <= 2) {
      auto releaseDue = [now](Slot &slot) {
        if (slot.used && slot.acceptedByMesh &&
            uint32_t(now - slot.lastAttempt) >= slot.retryAfter)
          slot.acceptedByMesh = false;
      };
      for (auto &slot : sos) releaseDue(slot);
      for (auto &slot : locations) releaseDue(slot);
    }
    size_t accepted = 0;
    for (const auto &slot : sos) if (slot.used && slot.acceptedByMesh) ++accepted;
    for (const auto &slot : locations) if (slot.used && slot.acceptedByMesh) ++accepted;
    if (accepted >= maxInFlight) return nullptr;
    auto eligible = [now](const Slot &slot) {
      return slot.used && !slot.acceptedByMesh &&
        (!slot.attemptedOnce || uint32_t(now - slot.lastAttempt) >= slot.retryAfter);
    };
    Slot *selected = nullptr;
    for (auto &slot : sos)
      if (eligible(slot) && (!selected || slot.order < selected->order)) selected = &slot;
    if (!selected) for (auto &slot : locations)
      if (eligible(slot) && (!selected || slot.order < selected->order)) selected = &slot;
    return selected ? &selected->message : nullptr;
  }
  bool attempted(const uint8_t key[16], bool accepted, uint32_t now) {
    Slot *selected = nullptr;
    for (auto &slot : sos)
      if (slot.used && !memcmp(slot.message.digest, key, 16)) { selected = &slot; break; }
    if (!selected) for (auto &slot : locations)
      if (slot.used && !memcmp(slot.message.digest, key, 16)) { selected = &slot; break; }
    if (!selected) return false;
    selected->lastAttempt = now; selected->attemptedOnce = true;
    if (accepted) {
      selected->acceptedByMesh = true;
      selected->retryAfter = selected->message.kind == 'S' ? 15000U : 60000U;
    } else {
      selected->acceptedByMesh = false;
      selected->retryAfter = selected->retryAfter < 5000U ? 5000U :
        selected->retryAfter >= 40000U ? 40000U : selected->retryAfter * 2U;
    }
    return true;
  }
  char kindForKey(const uint8_t key[16]) const {
    for (const auto &slot : sos)
      if (slot.used && !memcmp(slot.message.digest, key, 16)) return slot.message.kind;
    for (const auto &slot : locations)
      if (slot.used && !memcmp(slot.message.digest, key, 16)) return slot.message.kind;
    return 0;
  }
  bool completeStored(const uint8_t key[16]) {
    if (!healthy) return false;
    for (size_t i = 0; i < SOS_CAPACITY; ++i) if (sos[i].used && !memcmp(sos[i].message.digest, key, 16)) {
      CompletedSos done{};
      strcpy(done.requestId, sos[i].message.requestId);
      memcpy(done.digest, sos[i].message.digest, 32);
      done.nameTruncated = sos[i].message.nameTruncated;
      if (!journal.saveCompleted(completedCursor, done)) { healthy = false; return false; }
      completed[completedCursor] = done;
      completedCursor = (completedCursor + 1) % COMPLETED_SOS_CAPACITY;
      if (!journal.clear(i)) { healthy = false; return false; }
      sos[i] = Slot{}; return true;
    }
    for (auto &slot : locations) if (slot.used && !memcmp(slot.message.digest, key, 16)) {
      for (auto &entry : history) if (entry.used && !strcmp(entry.requestId, slot.message.requestId)) entry.pending = false;
      slot = Slot{}; return true;
    }
    return false;
  }
  Result accept(Message message, uint32_t now) {
    if (!healthy) return Result(503, "storage_unavailable");
    expire(now);
    for (const auto &slot : sos) if (slot.used && strcmp(slot.message.requestId, message.requestId) == 0)
      return duplicate(slot.message.digest, message, "queued_for_lora");
    for (const auto &entry : completed) if (entry.requestId[0] && strcmp(entry.requestId, message.requestId) == 0)
      return duplicate(entry.digest, message, "lora_tx_completed");
    for (const auto &entry : history) if (entry.used && strcmp(entry.requestId, message.requestId) == 0)
      return duplicate(entry.digest, message, entry.pending ? "queued_for_lora" : "superseded");
    if (sequence == UINT32_MAX) return Result(503, "identity_exhausted"); // no silent identity wrap
    message.source = node; message.bootSession = session; message.sequence = ++sequence;
    if (message.kind == 'S') {
      int free = -1;
      for (size_t offset = 0; offset < SOS_CAPACITY; ++offset) {
        size_t i = (sosCursor + offset) % SOS_CAPACITY;
        if (!sos[i].used) { free = int(i); break; }
      }
      if (free < 0) return Result(503, "queue_full");
      uint8_t next = uint8_t((size_t(free) + 1) % SOS_CAPACITY);
      if (!journal.saveCursor(next)) { healthy = false; return Result(503, "storage_unavailable"); }
      sosCursor = next;
      if (!journal.save(size_t(free), message)) { healthy = false; return Result(503, "storage_unavailable"); }
      sos[free].message = message; sos[free].order = ++order; sos[free].used = true;
      return Result{};
    }
    if (message.kind != 'L') return Result(400, "invalid_kind");
    int selected = -1, free = -1;
    for (size_t i = 0; i < LOCATION_CAPACITY; ++i) {
      if (!locations[i].used) { if (free < 0) free = int(i); continue; }
      if (strcmp(locations[i].message.userId, message.userId) == 0) selected = int(i);
    }
    bool coalesced = selected >= 0;
    if (coalesced && message.fixTime <= locations[selected].message.fixTime) return Result(409, "stale_location");
    if (selected < 0) selected = free;
    if (selected < 0) return Result(503, "queue_full");
    int record = historySlot(now);
    if (record < 0) return Result(503, "dedupe_full");
    if (coalesced) for (auto &entry : history) {
      if (entry.used && strcmp(entry.requestId, locations[selected].message.requestId) == 0) entry.pending = false;
    }
    // Coalescing replaces the radio scheduling state too. Otherwise a newer
    // location could inherit an old frame's in-flight reservation/backoff.
    locations[selected] = Slot{};
    locations[selected].message = message; locations[selected].used = true; locations[selected].order = ++order;
    History &entry = history[record]; entry = History{};
    strcpy(entry.requestId, message.requestId); memcpy(entry.digest, message.digest, 32);
    entry.pending = entry.used = true; entry.nameTruncated = message.nameTruncated; entry.acceptedMs = now;
    return Result(202, nullptr, "queued_for_lora", false, coalesced);
  }
};
} // namespace rescue_mobile
