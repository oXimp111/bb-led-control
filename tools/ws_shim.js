/* Injected by tools/embed_web.py into the copy of control.html that the control firmware serves
   (web/control.html itself is not modified). Adds a "Wi-Fi (this device)" transport that speaks the same
   JSON-line protocol as USB, over a WebSocket to the ESP32 that served the page, and selects it automatically. */
(function () {
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;   // opened from disk: leave the simulator
  const sel = document.getElementById('transportSel');
  if (!sel) return;
  const WsTransport = {
    name: 'ws', ws: null, want: false, onEvt: null, retry: null,
    open(onEvt) {
      this.onEvt = onEvt; this.want = true;
      return new Promise(res => {
        let settled = false;
        const ws = this.ws = new WebSocket('ws://' + location.hostname + ':81/');
        ws.onopen = () => { settled = true; res(true); };
        ws.onerror = () => { if (!settled) { settled = true; toast('Wi-Fi link to the control box failed'); res(false); } };
        ws.onmessage = ev => {
          for (const line of String(ev.data).split('\n')) {
            if (!line) continue;
            if (line[0] === '{') { try { onEvt(JSON.parse(line)); continue; } catch (e) { } }
            log('dev', line);
          }
        };
        ws.onclose = () => {
          if (!this.want || this.ws !== ws) return;
          if (settled) toast('Control box link lost, reconnecting...');
          clearTimeout(this.retry);
          this.retry = setTimeout(async () => {
            if (!this.want) return;
            if (await this.open(this.onEvt)) request({ cmd: 'hello' });
          }, 2000);
        };
      });
    },
    write(obj) { if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(obj)); },
    close() { this.want = false; clearTimeout(this.retry); try { this.ws && this.ws.close(); } catch (e) { } },
  };
  const opt = document.createElement('option');
  opt.value = 'ws'; opt.textContent = 'Wi-Fi (this device)';
  sel.insertBefore(opt, sel.firstChild);
  const pick = {
    ws: () => WsTransport, serial: () => SerialTransport, sim: () => SimTransport,
  };
  async function choose(v) {
    transport.close();
    transport = (pick[v] || pick.sim)();
    if (!(await transport.open(onEvent))) { sel.value = 'sim'; transport = SimTransport; await transport.open(onEvent); }
    else { live.nodes = {}; request({ cmd: 'hello' }); }
    renderNodes(); renderControl();
  }
  sel.onchange = e => choose(e.target.value);
  sel.value = 'ws';
  choose('ws');
})();
