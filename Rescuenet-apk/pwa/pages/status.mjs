const SOS_LABELS = {
  QUEUED_LOCAL: 'Menunggu dikirim', SENDING: 'Sedang mengirim ke Field Node',
  FIELD_ACCEPTED: 'Diterima Field Node', UNKNOWN: 'Status belum pasti', FAILED: 'Gagal dikirim',
};

function setSosStage(elements, stage, detail, state = '') {
  elements[`sos-stage-${stage}-detail`].textContent = detail;
  elements[`sos-stage-${stage}`].className = `sos-stage${state ? ` ${state}` : ''}`;
}

export function renderSosStatus({ state, elements, retrySos }) {
  const current = state.events.at(-1) || null;
  if (!current) {
    elements['report-status'].textContent = 'Belum ada SOS aktif';
    elements['report-detail'].textContent = 'Belum ada laporan SOS dari perangkat ini.';
    elements['home-report-status'].textContent = 'Belum ada SOS aktif';
    elements['home-report-detail'].textContent = 'Anda belum membuat laporan SOS.';
    elements['retry-button'].classList.add('hidden');
    elements['sos-history-panel'].hidden = true;
    elements['sos-history'].replaceChildren();
    setSosStage(elements, 'local', 'Belum dibuat');
    setSosStage(elements, 'node', 'Belum dikirim');
    setSosStage(elements, 'server', 'Belum dapat dipantau dari PWA');
    return;
  }
  const label = SOS_LABELS[current.status] || 'Status belum diketahui';
  const detail = current.status === 'FIELD_ACCEPTED'
    ? `Field Node ${current.nodeId ?? '?'} mengakui antrean ${new Date(current.deliveredAt).toLocaleTimeString('id-ID')}. Gateway/server belum mengonfirmasi.`
    : current.error || `ID laporan ${current.id} disimpan lokal. Retry akan memakai ID yang sama.`;
  elements['report-status'].textContent = label;
  elements['report-detail'].textContent = detail;
  elements['home-report-status'].textContent = label;
  elements['home-report-detail'].textContent = detail;
  elements['retry-button'].classList.toggle('hidden', !['QUEUED_LOCAL', 'FAILED', 'UNKNOWN'].includes(current.status));
  elements['sos-history-panel'].hidden = false;
  setSosStage(elements, 'local', `Dibuat ${new Date(current.createdAt).toLocaleTimeString('id-ID')}`, 'is-complete');
  const nodeStage = {
    QUEUED_LOCAL: ['Menunggu pengiriman', ''],
    SENDING: ['Sedang dikirim', 'is-current'],
    FIELD_ACCEPTED: [`Diterima ${new Date(current.deliveredAt).toLocaleTimeString('id-ID')}`, 'is-complete'],
    UNKNOWN: ['Belum pasti; coba lagi dengan ID yang sama', 'is-attention'],
    FAILED: ['Ditolak; coba kirim ulang', 'is-attention'],
  }[current.status] || ['Belum diketahui', 'is-attention'];
  setSosStage(elements, 'node', ...nodeStage);
  setSosStage(elements, 'server', 'Belum dapat dipantau dari PWA');
  elements['sos-history'].replaceChildren();
  for (const event of [...state.events].reverse()) {
    const row = document.createElement('div');
    row.className = 'sos-history-row';
    const copy = document.createElement('span');
    copy.textContent = `${new Date(event.createdAt).toLocaleTimeString('id-ID')} · ${SOS_LABELS[event.status] || event.status} · ${event.id.slice(0, 8)}`;
    row.append(copy);
    if (['QUEUED_LOCAL', 'FAILED', 'UNKNOWN'].includes(event.status)) {
      const retry = document.createElement('button');
      retry.type = 'button'; retry.className = 'text-button'; retry.textContent = 'Coba lagi';
      retry.addEventListener('click', () => retrySos(event.id));
      row.append(retry);
    }
    elements['sos-history'].append(row);
  }
}

export function renderLocationStatus({ pending, accepted, canRetry, elements }) {
  const label = pending?.attempts >= 5 ? 'Gagal dikirim' : pending ? 'Menunggu dikirim' : accepted ? 'Diterima Field Node' : 'Belum dikirim';
  elements['location-delivery-status'].textContent = label;
  elements['location-delivery-detail'].textContent = pending
    ? `${pending.lastError ? `${pending.lastError} ` : ''}Percobaan ${pending.attempts}/5; server belum dikonfirmasi.`
    : accepted ? `Diterima Node ${accepted.nodeId ?? '?'} pukul ${new Date(accepted.acceptedAt).toLocaleTimeString('id-ID')}. Server belum dikonfirmasi.`
      : 'GPS lokal tidak berarti paket sudah dikirim.';
  elements['retry-location-button'].classList.toggle('hidden', !canRetry);
}

export function bindStatusPage({ elements, retrySos, retryLocation, newSos }) {
  elements['retry-button'].addEventListener('click', retrySos);
  elements['retry-location-button'].addEventListener('click', retryLocation);
  elements['new-sos-button'].addEventListener('click', newSos);
}

