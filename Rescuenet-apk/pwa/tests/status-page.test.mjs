import test from 'node:test';
import assert from 'node:assert/strict';
import { renderSosStatus } from '../pages/status.mjs';

function element() {
  const classes = new Set();
  return {
    textContent: '', className: '', hidden: false, children: [],
    classList: {
      add: name => classes.add(name),
      toggle: (name, enabled) => enabled ? classes.add(name) : classes.delete(name),
      contains: name => classes.has(name),
    },
    replaceChildren() { this.children = []; },
    append(child) { this.children.push(child); },
    addEventListener() {},
  };
}

function elements() {
  return Object.fromEntries([
    'report-status', 'report-detail', 'home-report-status', 'home-report-detail',
    'retry-button', 'sos-history-panel', 'sos-history',
    'sos-stage-local', 'sos-stage-local-detail', 'sos-stage-node',
    'sos-stage-node-detail', 'sos-stage-server', 'sos-stage-server-detail',
  ].map(id => [id, element()]));
}

test('SOS timeline distinguishes local creation, Field Node ACK, and unconfirmed server', () => {
  const originalDocument = globalThis.document;
  globalThis.document = { createElement: element };
  try {
    const ui = elements();
    const state = { events: [] };
    renderSosStatus({ state, elements: ui, retrySos() {} });
    assert.equal(ui['sos-stage-local-detail'].textContent, 'Belum dibuat');
    assert.equal(ui['sos-history-panel'].hidden, true);

    const event = { id: 'request-12345678', status: 'QUEUED_LOCAL', createdAt: 1_700_000_000_000, error: null };
    state.events.push(event);
    renderSosStatus({ state, elements: ui, retrySos() {} });
    assert.match(ui['sos-stage-local-detail'].textContent, /Dibuat/);
    assert.equal(ui['sos-stage-node-detail'].textContent, 'Menunggu pengiriman');
    assert.equal(ui['sos-stage-server-detail'].textContent, 'Belum dapat dipantau dari PWA');
    assert.equal(ui['retry-button'].classList.contains('hidden'), false);

    event.status = 'FIELD_ACCEPTED';
    event.nodeId = 2;
    event.deliveredAt = 1_700_000_001_000;
    renderSosStatus({ state, elements: ui, retrySos() {} });
    assert.match(ui['sos-stage-node-detail'].textContent, /Diterima/);
    assert.match(ui['report-detail'].textContent, /Gateway\/server belum mengonfirmasi/);
    assert.equal(ui['sos-stage-server-detail'].textContent, 'Belum dapat dipantau dari PWA');
    assert.equal(ui['retry-button'].classList.contains('hidden'), true);
  } finally {
    globalThis.document = originalDocument;
  }
});
