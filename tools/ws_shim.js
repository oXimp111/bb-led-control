/* Injected by tools/embed_web.py into the copy of control.html that the control firmware serves
   (web/control.html itself is not modified). Adds a "Wi-Fi (this device)" transport that speaks the same
   JSON-line protocol as USB, over a WebSocket to the ESP32 that served the page, and selects it automatically.

   Dead links: a laptop whose Wi-Fi dozes or blips during a long cue pause can keep a socket that LOOKS open while the
   control box has already dropped it. The browser does not notice for minutes, so a GO would vanish (a page reload
   used to be the only cure). The box sends {"evt":"alive"} every second, and the page treats 3 s of silence
   (1.5 s while it waits for an answer, 1.2 s when GO is pressed) as a dead link: it opens a new socket at once,
   fails the requests still waiting on the old one so they are re-sent, and request() waits for the link (up to 4 s)
   instead of writing into nothing.                                                                                 */
(function () {
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;   // opened from disk: leave the simulator
  const sel = document.getElementById('transportSel');
  if (!sel) return;
  const IDLE_MS = 3000, BUSY_MS = 1500, FRESH_MS = 1200, CONNECT_MS = 3000, RETRY_MS = 1000;
  const badge = document.createElement('span');
  badge.id = 'linkState'; badge.style.cssText = 'margin-left:8px;font-size:12px;white-space:nowrap';
  sel.insertAdjacentElement('afterend', badge);
  const WsTransport = {
    name: 'ws', ws: null, want: false, up: false, onEvt: null, retry: null, lastRx: 0, waiters: [], downSince: 0, lost: 0,
    open(onEvt) {
      this.onEvt = onEvt; this.want = true;
      if (!this.dog) {
        this.dog = setInterval(() => this.check(false), 500);
        const wake = () => this.check(true);
        document.addEventListener('visibilitychange', () => { if (!document.hidden) wake(); });
        addEventListener('focus', wake); addEventListener('online', wake); addEventListener('pageshow', wake);
      }
      this.show();
      return this.connect();      // false: it keeps retrying by itself (the page came from the box, so the box is the only sensible target)
    },
    connect() {               // one attempt; true when open
      clearTimeout(this.retry); this.retry = null;
      if (this.connecting) return this.connecting;
      return this.connecting = new Promise(res => {
        const ws = this.ws = new WebSocket('ws://' + location.hostname + ':81/');
        let settled = false;
        const done = ok => { if (settled) return; settled = true; clearTimeout(tm); this.connecting = null; res(ok); };
        const tm = setTimeout(() => { done(false); if (this.ws === ws) this.drop('no answer'); }, CONNECT_MS);
        ws.onopen = () => {
          if (this.ws !== ws) return done(false);
          const was = this.downSince; this.up = true; this.lastRx = performance.now(); this.downSince = 0; this.show();
          if (was) log('dev', `link back after ${Math.round(performance.now() - was)} ms`);
          request({ cmd: 'hello' });
          this.waiters.splice(0).forEach(f => f(true));
          done(true);
        };
        ws.onmessage = ev => {
          if (this.ws !== ws) return;
          this.lastRx = performance.now();
          for (const line of String(ev.data).split('\n')) {
            if (!line || line === '{"evt":"alive"}') continue;
            if (line[0] === '{') { try { this.onEvt(JSON.parse(line)); continue; } catch (e) { } }
            log('dev', line);
          }
        };
        ws.onclose = () => { done(false); if (this.ws === ws) this.drop('closed'); };
      });
    },
    drop(why) {               // the current socket is dead: forget it, fail what waits on it, reconnect
      const ws = this.ws; this.ws = null;
      if (ws) { ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null; try { ws.close(); } catch (e) { } }
      const wasUp = this.up; this.up = false;
      if (!this.want) return;
      if (wasUp) {
        this.downSince = performance.now(); this.lost++;
        log('dev', `link lost (${why}), reconnecting`);
        toast('Control box link lost, reconnecting...');
        for (const id of Object.keys(pending)) { clearTimeout(pending[id].timer); pending[id].res({ ok: false, err: 'link_lost' }); delete pending[id]; }
      }
      this.show();
      if (!this.retry && !this.connecting) this.retry = setTimeout(() => this.connect(), wasUp ? 0 : RETRY_MS);
    },
    check(now) {              // watchdog; now = the page just woke up or GO was pressed: be strict
      if (!this.want) return;
      if (this.up) {
        const quiet = performance.now() - this.lastRx;
        if (quiet > (now ? FRESH_MS : Object.keys(pending).length ? BUSY_MS : IDLE_MS)) this.drop(`nothing heard for ${Math.round(quiet)} ms`);
      } else if (now && !this.connecting) this.connect();
    },
    ready(maxMs) {            // resolves true once the link is up and fresh (false after maxMs)
      this.check(true);
      if (this.up) return Promise.resolve(true);
      return new Promise(res => {
        const f = ok => { clearTimeout(t); res(ok); };
        const t = setTimeout(() => { this.waiters = this.waiters.filter(x => x !== f); res(false); }, maxMs);
        this.waiters.push(f);
      });
    },
    show() {
      badge.textContent = !this.want ? '' : this.up ? '● linked' : '● reconnecting…';
      badge.style.color = this.up ? 'var(--ok, #2a2)' : 'var(--bad, #d33)';
    },
    write(obj) { if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(obj)); },
    close() {
      this.want = false; this.up = false; clearTimeout(this.retry); this.retry = null;
      const ws = this.ws; this.ws = null;
      if (ws) { ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null; try { ws.close(); } catch (e) { } }
      this.waiters.splice(0).forEach(f => f(false)); this.show();
    },
  };
  window.WsTransport = WsTransport;
  const opt = document.createElement('option');
  opt.value = 'ws'; opt.textContent = 'Wi-Fi (this device)';
  sel.insertBefore(opt, sel.firstChild);
  const pick = {
    ws: () => WsTransport, serial: () => SerialTransport, sim: () => SimTransport,
  };
  async function choose(v) {
    transport.close();
    transport = (pick[v] || pick.sim)();
    if (transport === WsTransport) {   // sends hello itself every time the link comes up
      live.nodes = {};
      if (!(await transport.open(onEvent))) toast('Wi-Fi link to the control box failed, retrying... Is this laptop on "LED-Control"?', 6000);
    } else if (!(await transport.open(onEvent))) { sel.value = 'sim'; transport = SimTransport; await transport.open(onEvent); }
    else { live.nodes = {}; request({ cmd: 'hello' }); }
    renderNodes(); renderControl();
  }
  sel.onchange = e => choose(e.target.value);
  sel.value = 'ws';
  choose('ws');
})();
