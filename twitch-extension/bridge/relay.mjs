const params = new URLSearchParams(location.search);
const target = params.get('origin'), nonce = params.get('nonce');
const status = document.querySelector('#status');
const game = document.querySelector('#game');
const allowed = ['https://pvffxvvhlpi5o8qe3ybjjpwb5hh7n7.ext-twitch.tv', location.origin];
const validNonce = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(nonce || '');
let stopped = false, events;
function disconnected() {
  return {frame: {schema: 'c4dll.twitch-frame', version: 1, battle_id: 'bridge:disconnected',
    ts: Date.now(), active: false, snapshot: null},
    status: {code: 'unavailable', message: 'Нет связи с игрой. Запустите игру и включите Twitch Stat в меню плагинов.'}};
}
function send(payload) { window.opener?.postMessage({source: 'd2-battle-bridge', nonce, ...payload}, target); }
function receive(payload) {
  if (stopped) return;
  if (!window.opener || window.opener.closed) {
    stopped = true; events?.close();
    status.textContent = 'Панель Twitch закрыта. Откройте соединение заново из панели управления расширением.';
    return;
  }
  status.textContent = typeof payload.status?.message === 'string' ? payload.status.message : 'Ожидаем данные игры…';
  const pid = payload.status?.pid ?? payload.frame?.pid;
  const hasPid = Number.isInteger(pid) && pid > 0 && pid <= 0xffffffff;
  game.hidden = !hasPid;
  game.textContent = hasPid ? `Подключённая игра: PID ${pid}` : '';
  send(payload);
}
document.querySelector('#close').addEventListener('click', () => {
  stopped = true; events?.close(); send(disconnected()); window.close();
});
addEventListener('pagehide', () => { stopped = true; events?.close(); if (allowed.includes(target) && validNonce) send(disconnected()); });
if (!allowed.includes(target) || !validNonce) status.textContent = 'Откройте соединение из панели управления расширением Twitch.';
else {
  events = new EventSource('/events');
  events.onmessage = event => {
    try { receive(JSON.parse(event.data)); }
    catch { receive(disconnected()); }
  };
  events.onerror = () => receive(disconnected());
}
