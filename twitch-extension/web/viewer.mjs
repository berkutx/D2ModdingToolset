import {FrameAssembler, FrameTimeline, portraitBoxes} from './protocol.mjs';
import {parseGameText} from './game-text.mjs';
const root = document.querySelector('#portraits'), card = document.querySelector('#card');
const assembler = new FrameAssembler();
const timeline = new FrameTimeline();
let current = null, delay = 0, selected = null, anchor = null, closeTimer = null;
let videoRatio = null;
function content(tag, text, parent = card) { const el = document.createElement(tag); el.textContent = text; parent.append(el); return el; }
function appendRuns(parent, runs, start = 0, end = Infinity) {
  let offset = 0;
  for (const run of runs) {
    const from = Math.max(0, start - offset), to = Math.min(run.text.length, end - offset);
    if (to > from) {
      const node = content('span', run.text.slice(from, to), parent);
      // Lift native parchment colors for the translucent dark card.
      if (run.color) node.style.color = `color-mix(in srgb, ${run.color} 50%, #ffffff)`;
    }
    offset += run.text.length;
  }
}
function formattedRows(unit, key) { return parseGameText(unit.formatted?.[key] ?? unit[key] ?? ''); }
function renderStats(rows, parent) {
  const list = content('dl', '', parent); list.className = 'stat-list';
  for (const runs of rows) {
    const text = runs.map(run => run.text).join('');
    if (!text.trim()) continue;
    const colon = text.indexOf(':');
    if (colon < 0) { const line = content('div', '', list); line.className = 'stat-note'; appendRuns(line, runs); continue; }
    const term = content('dt', '', list), value = content('dd', '', list);
    appendRuns(term, runs, 0, colon + 1);
    const valueStart = colon + 1 + (text.slice(colon + 1).match(/^\s*/)?.[0].length ?? 0);
    appendRuns(value, runs, valueStart);
  }
}
function renderLines(rows, parent, boldLabels = false) {
  for (const runs of rows) {
    const text = runs.map(run => run.text).join('');
    if (!text.trim()) continue;
    const line = content('div', '', parent); line.className = 'text-line';
    if (!boldLabels) { appendRuns(line, runs); continue; }
    let offset = 0;
    for (const match of text.matchAll(/(?:^|\s)([\p{L}][\p{L} -]*:)/gu)) {
      const start = match.index + match[0].length - match[1].length;
      appendRuns(line, runs, offset, start);
      appendRuns(content('strong', '', line), runs, start, start + match[1].length);
      offset = start + match[1].length;
    }
    appendRuns(line, runs, offset);
  }
}
function cancelClose() { clearTimeout(closeTimer); closeTimer = null; }
function closeCard() { cancelClose(); card.hidden = true; selected = null; anchor = null; }
function scheduleClose() { cancelClose(); closeTimer = setTimeout(closeCard, 180); }
function positionCard() {
  if (!anchor) return;
  const b = anchor.getBoundingClientRect(), gap = 12;
  const x = b.right + gap + card.offsetWidth <= innerWidth ? b.right + gap : b.left - gap - card.offsetWidth;
  card.style.left = `${Math.max(8, Math.min(innerWidth - card.offsetWidth - 8, x))}px`;
  card.style.top = `${Math.max(8, Math.min(innerHeight - card.offsetHeight - 8, b.top))}px`;
}
function openCard(unitIndex, button) {
  const unit = current?.snapshot?.units?.[unitIndex]; if (!unit) return closeCard();
  const expanded = selected === unitIndex
    ? new Set([...card.querySelectorAll('details[open]')].map(detail => detail.dataset.section)) : new Set();
  const focused = selected === unitIndex && card.contains(document.activeElement)
    ? (document.activeElement.classList.contains('card-close') ? 'close'
      : document.activeElement.closest('details')?.dataset.section) : null;
  cancelClose();
  selected = unitIndex; anchor = button; card.replaceChildren();
  const header = content('header', ''); header.className = 'card-header';
  content('h2', unit.name, header);
  const close = content('button', '×', header); close.type = 'button'; close.className = 'card-close';
  close.setAttribute('aria-label', 'Закрыть карточку'); close.addEventListener('click', closeCard);
  if (unit.leader) {
    const leader = content('div', ''); leader.className = 'leader-line';
    renderLines(formattedRows(unit, 'leader'), leader, true);
  }
  const columns = content('div', ''); columns.className = 'stat-columns';
  for (const key of ['stats', 'attack']) {
    const column = content('section', '', columns);
    const title = key === 'stats' ? 'Параметры' : 'Атака';
    column.setAttribute('aria-label', title); content('h3', title, column);
    renderStats(formattedRows(unit, key), column);
  }
  if (unit.upgrade) {
    const upgrade = content('div', ''); upgrade.className = 'upgrade-info';
    renderLines(formattedRows(unit, 'upgrade'), upgrade);
  }
  if (unit.effects.length) {
    const effects = content('section', ''); effects.className = 'effects'; effects.setAttribute('aria-label', 'Действующие эффекты');
    for (let i = 0; i < unit.effects.length; i++) {
      const effect = content('div', '', effects); effect.className = 'effect';
      renderLines(parseGameText(unit.formatted?.effects?.[i] ?? unit.effects[i]), effect, true);
    }
  }
  function disclosure(key, label) {
    const detail = content('details', ''); detail.dataset.section = key; detail.open = expanded.has(key);
    content('summary', label, detail); detail.addEventListener('toggle', positionCard); return detail;
  }
  if (unit.stats_extra || unit.formatted?.stats_extra) {
    const extra = disclosure('extra', 'Дополнительные параметры');
    renderStats(formattedRows(unit, 'stats_extra'), extra);
  }
  if (unit.description) {
    const description = disclosure('description', 'Описание');
    renderLines(formattedRows(unit, 'description'), description);
  }
  card.hidden = false; positionCard();
  if (focused) (focused === 'close' ? close
    : [...card.querySelectorAll('details')].find(detail => detail.dataset.section === focused)?.querySelector('summary'))
    ?.focus({preventScroll:true});
}
function layout() {
  let width = innerWidth, height = innerHeight;
  if (videoRatio && width / height > videoRatio) width = height * videoRatio;
  else if (videoRatio) height = width / videoRatio;
  Object.assign(root.style, {left:`${(innerWidth-width)/2}px`,top:`${(innerHeight-height)/2}px`,width:`${width}px`,height:`${height}px`,right:'auto',bottom:'auto'});
  positionCard();
}
function render(frame) {
  const oldBattle = current?.battle_id;
  current = frame;
  if (!frame.active) { root.replaceChildren(); closeCard(); return; }
  if (oldBattle !== frame.battle_id) { root.replaceChildren(); closeCard(); }
  const existing = new Map([...root.children].map(b => [Number(b.dataset.unit), b]));
  for (const box of portraitBoxes(frame)) {
    let button = existing.get(box.unit); existing.delete(box.unit);
    if (!button) {
      button = document.createElement('button'); button.className = 'unit-hit'; button.dataset.unit = box.unit;
      button.addEventListener('mouseenter', () => openCard(box.unit, button));
      button.addEventListener('focus', () => openCard(box.unit, button));
      button.addEventListener('click', () => openCard(box.unit, button));
      button.addEventListener('mouseleave', event => { if (!card.contains(event.relatedTarget)) scheduleClose(); });
      button.addEventListener('blur', event => { if (!card.contains(event.relatedTarget)) scheduleClose(); });
      root.append(button);
    }
    button.setAttribute('aria-label', frame.snapshot.units[box.unit].name);
    Object.assign(button.style, {left:`${box.left*100}%`,top:`${box.top*100}%`,width:`${(box.right-box.left)*100}%`,height:`${(box.bottom-box.top)*100}%`});
  }
  for (const button of existing.values()) { if (button === anchor) closeCard(); button.remove(); }
  if (selected !== null && anchor?.isConnected) {
    const scroll = card.scrollTop, wasClosing = closeTimer !== null;
    openCard(selected, anchor); card.scrollTop = scroll;
    if (wasClosing) scheduleClose();
  }
  layout();
}
card.addEventListener('mouseenter', cancelClose);
card.addEventListener('mouseleave', scheduleClose);
addEventListener('resize', layout);
function receive(frame, received = performance.now(), completedReceived = received) {
  timeline.enqueue(frame, received, completedReceived);
}
setInterval(() => {
  const next = timeline.take(performance.now(), delay);
  if (next) render(next);
  else if (next === null) { root.replaceChildren(); closeCard(); current = null; }
}, 100);
const local = ['127.0.0.1','localhost'].includes(location.hostname) && new URLSearchParams(location.search).has('local');
if (local) {
  setInterval(async () => { try { const payload = await (await fetch('/snapshot', {cache:'no-store'})).json(); receive(payload.frame); } catch {} }, 1000);
} else if (window.Twitch?.ext) {
  Twitch.ext.onContext(context => {
    if (Number.isFinite(context.hlsLatencyBroadcaster)) delay = Math.max(0, Math.min(60000, context.hlsLatencyBroadcaster * 1000));
    const resolution = context.videoResolution;
    if (typeof resolution === 'string' && /^\d+x\d+$/.test(resolution)) { const [w,h]=resolution.split('x').map(Number); if(h>0) videoRatio=w/h; }
    else if (resolution?.height > 0) videoRatio = resolution.width / resolution.height;
    layout();
  });
  Twitch.ext.listen('broadcast', async (_target, type, message) => {
    if (type !== 'application/json') return;
    try { const complete = await assembler.receive(message); if (complete) receive(complete.frame, complete.received, complete.completedReceived); } catch {}
  });
}
