'use strict';

/*
 * Shared by the pages that show live state from GET /api/status: Status,
 * System, and General Settings (Sendspin pairing). Load it after index.js.
 *
 * /api/status sends no CORS header on purpose, so these pages only work when
 * the device itself serves them, not in the ?backend= development mode.
 */

const POLL_MS = 2000;

// ---- formatting ----

function has(v) {
  return v !== undefined && v !== null && v !== '';
}

function duration(s) {
  if (!has(s)) return null;
  s = Math.max(0, Math.floor(s));
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (d > 0) return `${d} d ${h} h`;
  if (h > 0) return `${h} h ${String(m).padStart(2, '0')} min`;
  if (m > 0) return `${m} min ${String(sec).padStart(2, '0')} s`;
  return `${sec} s`;
}

function rate(hz) {
  if (!has(hz)) return null;
  return `${(hz / 1000).toLocaleString('en', { maximumFractionDigits: 1 })} kHz`;
}

function format(o) {
  // "44.1 kHz · 16-bit · stereo"
  if (!has(o.sample_rate)) return null;
  const parts = [rate(o.sample_rate)];
  if (has(o.bits)) parts.push(`${o.bits}-bit`);
  if (has(o.channels)) parts.push(o.channels === 2 ? 'stereo' : o.channels === 1 ? 'mono' : `${o.channels} ch`);
  return parts.join(' · ');
}

function codecFormat(o) {
  const f = format(o);
  if (!has(o.codec)) return f;
  const codec = o.codec.toUpperCase();
  return f ? `${codec}, ${f}` : codec;
}

function micros(us) {
  if (!has(us)) return null;
  const sign = us > 0 ? '+' : us < 0 ? '−' : '';
  const a = Math.abs(us);
  if (a >= 1000) return `${sign}${(a / 1000).toFixed(a >= 10000 ? 0 : 1)} ms`;
  return `${sign}${Math.round(a)} µs`;
}

function kib(bytes) {
  if (!has(bytes)) return null;
  return `${Math.round(bytes / 1024).toLocaleString('en')} KB`;
}

function volume(o) {
  if (!has(o.volume)) return null;
  return o.muted ? `${o.volume} % (muted)` : `${o.volume} %`;
}

function ago(s) {
  const d = duration(s);
  return d ? `${d} ago` : null;
}

function mmss(ms) {
  if (!has(ms)) return null;
  const t = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(t / 3600);
  const m = Math.floor((t % 3600) / 60);
  const sec = String(t % 60).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

function signal(rssi) {
  if (!has(rssi)) return null;
  const q = rssi >= -60 ? 'good' : rssi >= -70 ? 'fair' : 'weak';
  return `${rssi} dBm (${q})`;
}

// ---- rendering (textContent only: names come from the network) ----

// rows: [label or label(data), value(data)]; a missing value hides the row
function renderRowsInto(dl, rows, data) {
  const frag = document.createDocumentFragment();
  for (const [label, fn, cls] of rows) {
    let value = null;
    try {
      value = typeof fn === 'function' ? fn(data) : fn;
    } catch (e) {
      value = null;
    }
    if (!has(value)) continue;
    const dt = document.createElement('dt');
    dt.textContent = typeof label === 'function' ? label(data) : label;
    const dd = document.createElement('dd');
    dd.textContent = value;
    if (cls) dd.className = cls;
    frag.appendChild(dt);
    frag.appendChild(dd);
  }
  dl.replaceChildren(frag);
}

// a card with id card-<name> and a list with id rows-<name>; no data hides it
function renderCard(name, rows, data) {
  const card = document.getElementById(`card-${name}`);
  if (!data) {
    card.hidden = true;
    return;
  }
  card.hidden = false;
  renderRowsInto(document.getElementById(`rows-${name}`), rows, data);
}

function setBadge(id, text, kind) {
  const b = document.getElementById(id);
  b.textContent = text;
  b.className = `badge ${kind}`;
}

// ---- requests ----

// fetch() that gives up after timeoutMs (covers the body too: aborting makes
// a stalled json() reject). A missing login is a 401 without the browser's
// prompt (see noLoginPrompt in index.js). url: on the device itself.
async function fetchJson(url, options = {}, timeoutMs = 5000) {
  const controller = new AbortController();
  const abort = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, Object.assign({ cache: 'no-store', signal: controller.signal }, options,
      { headers: noLoginPrompt(options.headers) }));
    let body = null;
    try {
      body = await response.json();
    } catch (e) {
      body = null;
    }
    return { status: response.status, ok: response.ok, body };
  } finally {
    clearTimeout(abort);
  }
}

// the System tab's and General Settings' buttons (POST /api/action/<name>):
// true when the device took it. Throws when the device can't be reached or
// refused; the error's status is the HTTP status (401: LOGIN_NEEDED).
async function statusAction(name) {
  const r = await fetchJson(`/api/action/${name}`, {
    method: 'POST',
    headers: { 'X-Status-Action': '1' },
  });
  if (!r.ok) {
    const error = new Error(r.status === 401 ? LOGIN_NEEDED : `HTTP ${r.status}`);
    error.status = r.status;
    throw error;
  }
  return !!(r.body && r.body.ok === true);
}

// Reload the whole settings page (the tab bar too, when this page runs in
// it) and come back to this tab. A reload brings up the browser's login
// prompt after a 401.
function reloadTop() {
  try {
    if (window.top !== window) {
      const tab = location.pathname.replace(/^.*\//, '').replace(/\.html$/, '');
      if (tab) window.top.location.hash = tab;
    }
    window.top.location.reload();
  } catch (e) {
    location.reload();
  }
}

// /api/status can't be read from a page served by another host
function liveDataBlocked() {
  return !BACKEND_SAME_ORIGIN;
}

// Another tab of the settings page, through the tab bar when the page runs in
// it (index.html), else as a plain link. hash: an element id on that page.
function openPage(page, hash) {
  try {
    if (window.parent !== window && typeof window.parent.openTab === 'function') {
      window.parent.openTab(page, hash);
      return;
    }
  } catch (e) {
    // not the settings page's frame: fall through
  }
  location.href = hash ? `${page}#${hash}` : page;
}

// In rescue mode (web_rescue) /api/status describes the chip and its slots
// instead: it has next_update and no device section.
function isRescueStatus(st) {
  return !!st && typeof st === 'object' && !st.device && typeof st.next_update === 'string';
}

// ---- polling GET /api/status: one request at a time, paused while the page
// is hidden ----
//
// onStatus(json) renders a normal-mode reply. onState({ stale, wrongHost,
// login, rescue }) runs after every request: stale when the last one failed
// or the device is in rescue mode, wrongHost when the device refused this
// page's host name (it answers only to its IP or a LAN name), login when it
// asked for the settings password (401). After a 401 the poller waits for
// soon() or the tab coming back into view: asking every 2 s would bring the
// browser's login prompt back each time.
class StatusPoller {
  constructor(onStatus, onState, interval = POLL_MS) {
    this.onStatus = onStatus;
    this.onState = onState;
    this.interval = interval;
    this.timer = null;
    this.inFlight = false;
    this.running = false;
    this.generation = 0;  // a reply that started before stop() is dropped
    this.inRescue = false;  // the last reply came from rescue mode
    document.addEventListener('visibilitychange', () => {
      if (this.running && !document.hidden && this.timer === null && !this.inFlight) this.poll();
    });
  }

  start() {
    this.running = true;
    this.soon();
  }

  // no more requests or renders until start(); a request on its way is dropped
  stop() {
    this.running = false;
    this.generation++;
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }

  // after a button: refresh now rather than in up to 2 s
  soon() {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    if (this.running && !this.inFlight && !document.hidden) this.poll();
  }

  async poll() {
    this.timer = null;
    if (this.inFlight) return;
    this.inFlight = true;
    const generation = this.generation;
    let state = { stale: true, wrongHost: false, login: false, rescue: this.inRescue };
    let body = null;
    try {
      if (this.inRescue) {
        // This page must not hold rescue mode open (it ends after 10
        // minutes without use), so ask for a page file rather than status.
        // Only normal mode serves the page files; rescue mode 404s
        // them without counting it as use. (Not /favicon.ico: normal mode
        // 404s that too. No query: the file table matches the whole URI.)
        // A 401 also means normal mode: rescue mode's 404 asks no login.
        const r = await fetchJson('/styles.css');
        if (r.ok || r.status === 401) this.inRescue = false;  // back: ask /api/status next
      } else {
        const r = await fetchJson('/api/status');
        state.wrongHost = r.status === 403;
        state.login = r.status === 401;
        if (r.ok && r.body) {
          if (isRescueStatus(r.body)) {
            this.inRescue = true;
          } else {
            body = r.body;
            state.stale = false;
          }
        }
        state.rescue = this.inRescue;
      }
    } catch (error) {
      console.error('Status update failed:', error);
    } finally {
      this.inFlight = false;
    }
    if (!this.running) return;
    if (generation !== this.generation) {
      // stopped and started again meanwhile: ask afresh
      this.soon();
      return;
    }
    if (body) {
      try {
        this.onStatus(body);
      } catch (error) {
        console.error('Status render failed:', error);
      }
    }
    if (this.onState) this.onState(state);
    if (state.login) return;  // see the class comment
    // just left rescue mode: the status now, not in 5 s
    if (!this.inRescue && state.rescue) {
      this.soon();
      return;
    }
    this.schedule();
  }

  schedule() {
    if (this.running && this.timer === null && !this.inFlight && !document.hidden) {
      this.timer = setTimeout(() => this.poll(), this.inRescue ? RESCUE_PROBE_MS : this.interval);
    }
  }
}

const RESCUE_PROBE_MS = 5000;

// The banner and greyed-out cards while the device can't be reached
function showReachability({ stale, wrongHost, login, rescue }) {
  const offline = document.getElementById('offline');
  if (offline) {
    if (wrongHost) {
      offline.textContent = `The device only answers when this page is opened by its IP address or a local name (name.local, name.lan, …), not ${location.hostname}.`;
    } else if (login) {
      const link = document.createElement('a');
      link.href = '/';
      link.target = '_top';
      link.textContent = 'reload the page';
      link.addEventListener('click', e => {
        e.preventDefault();
        reloadTop();
      });
      offline.replaceChildren('Login needed: ', link,
        ', and your browser asks for the settings password (user admin).');
    } else if (rescue) {
      const link = document.createElement('a');
      link.href = '/';
      link.target = '_top';
      link.textContent = 'Open its setup page';
      offline.replaceChildren(
        'The device is in rescue mode (light blinking cyan). ', link,
        ' for WiFi and firmware. It returns to normal by itself after 10 idle minutes.');
    } else {
      offline.textContent = "Can't reach the device. Retrying…";
    }
    offline.style.display = stale ? 'block' : 'none';
  }
  document.querySelectorAll('.card').forEach(c => c.classList.toggle('stale', stale));
}
