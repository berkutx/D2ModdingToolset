export const MAX_MESSAGE_BYTES = 5000;
export const MAX_FRAME_BYTES = 1024 * 1024;
export const MAX_PARTS = 12;
export const MIN_SEND_INTERVAL_MS = 1100;
export const MAX_ASSEMBLY_MS = 35000;
export const MAX_FORMATTED_TEXT_CHARS = 65536;
export const MAX_FORMATTED_EFFECT_CHARS = 16384;
const encoder = new TextEncoder();

export function validateFrame(frame) {
  if (!frame || frame.schema !== 'c4dll.twitch-frame' || frame.version !== 1 ||
      typeof frame.active !== 'boolean' || typeof frame.battle_id !== 'string' ||
      frame.battle_id.length > 120 || !Number.isFinite(frame.ts)) throw new Error('Некорректный кадр игры');
  if (!frame.active) return frame;
  const data = frame.snapshot, view = frame.viewport;
  if (!view || !['left', 'top', 'width', 'height'].every(k => Number.isFinite(view[k])) ||
      view.width <= 0 || view.height <= 0 || view.width > 16384 || view.height > 16384 ||
      data?.schema !== 'c4dll.battle-roster' || data.schema_version !== 1 ||
      !Array.isArray(data.slots) || data.slots.length !== 12 ||
      !Array.isArray(data.units) || data.units.length > 12) throw new Error('Неизвестный формат боя');
  const fields = ['name', 'description', 'stats', 'stats_extra', 'leader', 'attack', 'upgrade'];
  for (const unit of data.units) {
    if (!unit || !fields.every(k => unit[k] == null || typeof unit[k] === 'string') ||
        typeof unit.name !== 'string' || !Array.isArray(unit.effects) ||
        unit.effects.length > 128 || !unit.effects.every(e => typeof e === 'string')) throw new Error('Некорректная карточка');
    // Optional native markup is additive: old plugins and partially populated
    // cards keep their plaintext fields. Pass accepted strings through unchanged.
    if (unit.formatted != null) {
      const marked = unit.formatted;
      if (typeof marked !== 'object' || Array.isArray(marked) ||
          !fields.every(k => marked[k] == null || (typeof marked[k] === 'string' && marked[k].length <= MAX_FORMATTED_TEXT_CHARS)) ||
          (marked.effects != null && (!Array.isArray(marked.effects) || marked.effects.length > 128 ||
            !marked.effects.every(value => typeof value === 'string' && value.length <= MAX_FORMATTED_EFFECT_CHARS)))) {
        throw new Error('Некорректное форматирование карточки');
      }
    }
  }
  for (const slot of data.slots) {
    const b = slot?.bounds;
    if (!b || !['left', 'top', 'right', 'bottom'].every(k => Number.isFinite(b[k])) ||
        b.right <= b.left || b.bottom <= b.top || typeof slot.occupied !== 'boolean' ||
        (slot.occupied && (!Number.isInteger(slot.unit_index) || slot.unit_index < 0 ||
          slot.unit_index >= data.units.length))) throw new Error('Некорректная рамка');
  }
  return frame;
}

export function portraitBoxes(frame) {
  validateFrame(frame);
  if (!frame.active) return [];
  const grouped = new Map();
  for (const slot of frame.snapshot.slots) {
    if (!slot.occupied) continue;
    const previous = grouped.get(slot.unit_index);
    const b = slot.bounds;
    grouped.set(slot.unit_index, previous ? {
      left: Math.min(previous.left, b.left), top: Math.min(previous.top, b.top),
      right: Math.max(previous.right, b.right), bottom: Math.max(previous.bottom, b.bottom)
    } : {...b});
  }
  const v = frame.viewport;
  return [...grouped].map(([unit, b]) => ({unit,
    left: Math.max(0, (b.left - v.left) / v.width), top: Math.max(0, (b.top - v.top) / v.height),
    right: Math.min(1, (b.right - v.left) / v.width), bottom: Math.min(1, (b.bottom - v.top) / v.height)
  })).filter(b => b.right > b.left && b.bottom > b.top);
}

function toBase64(bytes) {
  let text = '';
  for (let i = 0; i < bytes.length; i += 8192) text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}

export async function encodeFrame(frame, id = crypto.randomUUID()) {
  validateFrame(frame);
  if (typeof id !== 'string' || !id.length || id.length > 80) throw new Error('Некорректный идентификатор кадра');
  const bytes = encoder.encode(JSON.stringify(frame));
  if (bytes.length > MAX_FRAME_BYTES) throw new Error('Слишком большой снимок боя');
  const compressed = new Uint8Array(await new Response(
    new Blob([bytes]).stream().pipeThrough(new CompressionStream('gzip'))).arrayBuffer());
  const data = toBase64(compressed), count = Math.ceil(data.length / 4000);
  if (count > MAX_PARTS) throw new Error('Снимок боя превышает лимит передачи Twitch');
  return Array.from({length: count}, (_, index) => {
    const message = JSON.stringify({d2: 1, id, i: index, n: count, data: data.slice(index * 4000, (index + 1) * 4000)});
    if (encoder.encode(message).length > MAX_MESSAGE_BYTES) throw new Error('Превышен размер сообщения Twitch');
    return message;
  });
}

export class FrameAssembler {
  pending = new Map();
  completed = new Map();
  async receive(message, now = performance.now()) {
    for (const [key, value] of this.pending) if (now - value.first > MAX_ASSEMBLY_MS) this.pending.delete(key);
    for (const [key, value] of this.completed) if (now - value > 30000) this.completed.delete(key);
    if (typeof message !== 'string' || encoder.encode(message).length > MAX_MESSAGE_BYTES) return null;
    let part;
    try { part = JSON.parse(message); } catch { return null; }
    if (part?.d2 !== 1 || typeof part.id !== 'string' || !part.id.length || part.id.length > 80 || this.completed.has(part.id) ||
        !Number.isInteger(part.n) || part.n < 1 || part.n > MAX_PARTS || !Number.isInteger(part.i) ||
        part.i < 0 || part.i >= part.n || typeof part.data !== 'string' || part.data.length > 4000 ||
        !/^[A-Za-z0-9+/=]+$/.test(part.data)) return null;
    let pending = this.pending.get(part.id);
    if (!pending) {
      if (this.pending.size >= 4) this.pending.delete(this.pending.keys().next().value);
      pending = {first: now, parts: new Array(part.n)};
      this.pending.set(part.id, pending);
    }
    if (pending.parts.length !== part.n || (pending.parts[part.i] && pending.parts[part.i] !== part.data)) {
      this.pending.delete(part.id); return null;
    }
    pending.parts[part.i] = part.data;
    if (pending.parts.filter(Boolean).length !== part.n) return null;
    this.pending.delete(part.id);
    this.completed.set(part.id, now);
    if (this.completed.size > 64) this.completed.delete(this.completed.keys().next().value);
    let reader;
    try {
      const bytes = Uint8Array.from(atob(pending.parts.join('')), c => c.charCodeAt(0));
      reader = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip')).getReader();
      const chunks = []; let size = 0;
      for (;;) {
        const {value, done} = await reader.read();
        if (done) break;
        size += value.length;
        if (size > MAX_FRAME_BYTES) { await reader.cancel(); throw new Error('Слишком большой кадр'); }
        chunks.push(value);
      }
      const frame = validateFrame(JSON.parse(await new Blob(chunks).text()));
      return {frame, received: pending.first, completedReceived: now};
    } catch { return null; }
    finally { reader?.releaseLock(); }
  }
}

// Decompression is asynchronous: an earlier, larger frame may finish after a later
// battle-end frame. Order by the first received fragment, not by decode completion.
// Keep delay outside enqueue so a latency change applies to the whole buffer.
export class FrameTimeline {
  pending = []; newestReceived = -Infinity; newestTimestamp = -Infinity;
  current = null; shownCompleted = -Infinity;
  enqueue(frame, received = performance.now(), completedReceived = received) {
    validateFrame(frame);
    if (!Number.isFinite(received) || !Number.isFinite(completedReceived) || completedReceived < received ||
        received < this.newestReceived || frame.ts < this.newestTimestamp) return false;
    this.newestReceived = received; this.newestTimestamp = frame.ts;
    this.pending.push({frame, received, completedReceived});
    if (this.pending.length > 100) this.pending.shift();
    return true;
  }
  // undefined: no visual change; null: stale data must be removed.
  take(now = performance.now(), delay = 0) {
    delay = Number.isFinite(delay) ? Math.max(0, Math.min(60000, delay)) : 0;
    let latest;
    while (this.pending.length && this.pending[0].received + delay <= now) latest = this.pending.shift();
    if (latest) {
      this.current = latest.frame; this.shownCompleted = latest.completedReceived;
      if (latest.frame.active && now - latest.completedReceived > 20000 + delay) {
        this.current = null;
        return null;
      }
      return latest.frame;
    }
    if (this.current?.active && now - this.shownCompleted > 20000 + delay) {
      this.current = null;
      return null;
    }
    return undefined;
  }
}

// Full snapshots are repeated for late viewers. A new snapshot replaces only the waiting frame,
// never the remaining fragments of the currently transmitting frame. Battle end preempts fragments.
export class BroadcastQueue {
  latest = null; parts = []; lastSent = -Infinity; busy = false;
  offer(frame) {
    validateFrame(frame);
    this.latest = frame;
    if (!frame.active) this.parts = [];
  }
  async tick(send, now = () => performance.now()) {
    if (this.busy || now() - this.lastSent < MIN_SEND_INTERVAL_MS) return false;
    this.busy = true;
    try {
      if (!this.parts.length && this.latest) {
        const next = this.latest; this.latest = null;
        const encoded = await encodeFrame(next);
        if (this.latest && !this.latest.active && next.active) return false;
        this.parts = encoded;
      }
      if (!this.parts.length) return false;
      // Gzip and its Promise continuation can be suspended for seconds in a hidden
      // browser. Only a fresh clock sample at the API boundary can enforce spacing.
      const attemptAt = now();
      if (attemptAt - this.lastSent < MIN_SEND_INTERVAL_MS) return false;
      this.lastSent = attemptAt;
      try {
        send(this.parts[0]);
        this.parts.shift();
        return true;
      } finally {
        // A synchronous send may itself block or throw. Count every API attempt,
        // and never let a delayed return or failed call trigger an immediate retry.
        this.lastSent = Math.max(this.lastSent, now());
      }
    } finally { this.busy = false; }
  }
}

// Relay postMessage events are the primary clock. Hidden tabs may delay every browser
// timer; a new network event must therefore attempt sending before relying on a timer.
// Each external wake can arm one follow-up timeout, whose callback never chains itself.
export class BroadcastPump {
  timer = null; flight = null; retryRequested = false; allowFollowup = false;
  constructor(queue, send, {now = () => performance.now(), setTimer = setTimeout,
    clearTimer = clearTimeout, onError = () => {}} = {}) {
    this.queue = queue; this.send = send; this.now = now;
    this.setTimer = setTimer; this.clearTimer = clearTimer; this.onError = onError;
  }
  wake() { this.allowFollowup = true; return this.flush(); }
  flush() {
    if (this.timer !== null) { this.clearTimer(this.timer); this.timer = null; }
    if (this.flight) { this.retryRequested = true; return this.flight; }
    this.flight = this.drain().finally(() => { this.flight = null; });
    return this.flight;
  }
  async drain() {
    do {
      this.retryRequested = false;
      try { await this.queue.tick(this.send, this.now); }
      catch (error) { this.onError(error); }
    } while (this.retryRequested && this.now() - this.queue.lastSent >= MIN_SEND_INTERVAL_MS &&
      (this.queue.parts.length || this.queue.latest));
    const allow = this.allowFollowup; this.allowFollowup = false;
    if (allow && (this.queue.parts.length || this.queue.latest)) {
      const wait = Math.max(1, MIN_SEND_INTERVAL_MS - (this.now() - this.queue.lastSent));
      this.timer = this.setTimer(() => { this.timer = null; return this.flush(); }, wait);
    }
  }
}
