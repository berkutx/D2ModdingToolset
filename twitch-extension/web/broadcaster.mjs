import {BroadcastQueue, BroadcastPump, validateFrame} from './protocol.mjs';
const bridgeOrigin = 'http://127.0.0.1:8765';
const status = document.querySelector('#status'), stats = document.querySelector('#stats');
const connect = document.querySelector('#connect'), stop = document.querySelector('#stop');
const queue = new BroadcastQueue();
let popup = null, nonce = null, authorized = false, lastReceived = 0, messages = 0, running = false, stoppedFrame = null;
const pump = new BroadcastPump(queue, message => {
  Twitch.ext.send('broadcast', 'application/json', message);
  messages++;
  stats.textContent = `Отправлено сообщений: ${messages}. Осталось частей снимка: ${Math.max(0, queue.parts.length - 1)}.`;
}, {onError: error => { status.textContent = `Передача не удалась: ${error.message}`; }});
function inactive() { return {schema:'c4dll.twitch-frame',version:1,battle_id:'disconnected',ts:Date.now(),active:false,snapshot:null}; }
function stopSending() {
  running = false; stop.disabled = true; connect.disabled = !authorized;
  queue.offer(inactive()); stoppedFrame = Date.now();
  if (popup && !popup.closed) popup.close(); popup = null; nonce = null;
  status.textContent = 'Передача остановлена. Рамки у зрителей будут скрыты.';
  if (authorized) void pump.wake();
}
connect.addEventListener('click', () => {
  nonce = crypto.randomUUID();
  const url = new URL('/relay.html', bridgeOrigin);
  url.searchParams.set('origin', location.origin); url.searchParams.set('nonce', nonce);
  popup = window.open(url, 'd2-battle-bridge', 'popup,width=480,height=310');
  if (!popup) { status.textContent = 'Разрешите открытие окна соединения для этой панели.'; return; }
  running = true; stoppedFrame = null; stop.disabled = false; connect.disabled = true;
  lastReceived = performance.now(); status.textContent = 'Подключаем локальный мост…';
});
stop.addEventListener('click', stopSending);
addEventListener('message', event => {
  if (!running || event.source !== popup || event.origin !== bridgeOrigin ||
      event.data?.source !== 'd2-battle-bridge' || event.data.nonce !== nonce) return;
  try {
    const frame = validateFrame(event.data.frame);
    queue.offer(frame); lastReceived = performance.now();
    status.textContent = frame.active ? `Бой подключён: ${frame.snapshot.units.length} юнитов. Передаём зрителям.` :
      (typeof event.data.status?.message === 'string' ? event.data.status.message : 'Ожидаем данные поддерживаемого боя.');
  } catch (error) { status.textContent = error.message; queue.offer(inactive()); }
  // This task originates from the relay's network event, so hidden-page timer
  // throttling cannot make setInterval the only way to reach Twitch.ext.send.
  if (authorized) void pump.wake();
});
if (window.Twitch?.ext) {
  Twitch.ext.onAuthorized(() => { authorized = true; connect.disabled = running; if (!running) status.textContent = 'Twitch подключён. Теперь подключите игру.'; });
} else status.textContent = 'Откройте панель управления расширением в Twitch.';
setInterval(() => {
  if (!authorized) return;
  if (running && (popup?.closed || performance.now() - lastReceived > 6000)) {
    queue.offer(inactive()); status.textContent = 'Нет связи с игрой. Проверьте, что игра запущена и Twitch Stat включён, затем подключитесь заново.';
    if (popup?.closed) { running = false; connect.disabled = false; stop.disabled = true; }
  }
  // Repeat a clear briefly after a deliberate stop so viewers who missed the first message recover.
  if (stoppedFrame && Date.now() - stoppedFrame < 5000) queue.offer(inactive());
  void pump.wake();
}, 1000);
addEventListener('pagehide', () => { if (authorized) { try { queue.offer(inactive()); void pump.wake(); } catch {} } });
