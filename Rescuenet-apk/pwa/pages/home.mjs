export function renderHome({ elements, profile, state }) {
  for (const id of ['welcome-user', 'sos-help', 'sos-button', 'sos-button-label']) {
    if (!elements[id]) throw new Error(`Elemen wajib Beranda tidak ditemukan: #${id}`);
  }
  if (!state || !Object.hasOwn(state, 'sendingSosId') || !Object.hasOwn(state, 'nodeReady')) {
    throw new TypeError('renderHome memerlukan state.sendingSosId dan state.nodeReady.');
  }
  const user = profile();
  elements['welcome-user'].textContent = user
    ? `Halo, ${user.name}. Anda dapat membuat SOS meski Field Node belum terhubung.`
    : 'Siapkan identitas agar Anda dapat membuat SOS.';
  elements['sos-help'].textContent = !user ? 'Lengkapi profil untuk membuat SOS.'
    : state.nodeReady ? 'Kirim SOS ke Field Node. GPS tidak wajib.'
      : 'Simpan SOS di perangkat. Kirim setelah Field Node siap.';
  elements['sos-button'].disabled = state.sendingSosId !== null || !user;
  elements['sos-button-label'].textContent = state.sendingSosId ? 'MENGIRIM…' : state.nodeReady ? 'KIRIM SOS' : 'SIMPAN SOS';
}

export function bindHomePage({ elements, checkConnection, sendSos, showScreen, beginOnboarding }) {
  elements['home-check-button'].addEventListener('click', checkConnection);
  elements['sos-button'].addEventListener('click', sendSos);
  elements['home-map-button'].addEventListener('click', () => showScreen('map'));
  elements['home-status-button'].addEventListener('click', () => showScreen('status'));
  elements['home-wifi-button'].addEventListener('click', () => beginOnboarding(2));
}

