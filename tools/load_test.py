"""Cue-list GO under site-like load with a laptop that answers late. Question: can a GO fail in a way that only a page
reload fixes (installer report 2026-10-16, fw 3, MacBook)?
Needs: control board on the TEST build (pio run -e control_wstest: AP 192.168.10.1, serial wsdup/wsstats), one paired
node, the PC adapter --wlan joined to LED-Control, and the proxy with a slow-laptop knob:
    python tools/ap_proxy.py --wlan Wi-Fi --host 192.168.10.1 --rcvbuf 2048 --stall-file <file>
    python tools/load_test.py <node MAC> --ctl COM5 --stall-file <same file> --dup 8
--dup 8 = every node status sent 8 times (site: 4 nodes, each heard twice through the relay).
Each round: GO, wait, then the laptop stops reading for <stall> s at <when>, then GO. A failed GO is pressed again
(1 s, then 5 s later, then every 5 s up to 30 s) before the page is reloaded. Logs: test_logs/loadtest_<label>_<time>/.
"""
import argparse, os, threading, time
import serial
from playwright.sync_api import sync_playwright

ap = argparse.ArgumentParser()
ap.add_argument("node"); ap.add_argument("--ctl", required=True); ap.add_argument("--stall-file", required=True)
ap.add_argument("--page", default="http://localhost:8080/"); ap.add_argument("--label", default="fw3")
ap.add_argument("--dup", type=int, default=8); ap.add_argument("--pause", type=float, default=15)
ap.add_argument("--stalls", default="0,0.5,1,2,3,4,6")
ap.add_argument("--whens", default="pause,press,upload")
ap.add_argument("--repeat", type=int, default=1)
a = ap.parse_args()
out = os.path.join("test_logs", f"loadtest_{a.label}_{time.strftime('%Y%m%d_%H%M%S')}"); os.makedirs(out)
logf = open(os.path.join(out, "test.log"), "w", encoding="utf-8")
T0 = time.time()
def say(*x):
    s = f"[{(time.time() - T0) / 60:6.2f} min {time.strftime('%H:%M:%S')}] " + " ".join(str(v) for v in x)
    print(s, flush=True); logf.write(s + "\n"); logf.flush()

ctl = serial.Serial(); ctl.port = a.ctl; ctl.baudrate = 115200; ctl.timeout = 0.2; ctl.dtr = False; ctl.rts = False; ctl.open()
def ctl_reader():
    buf = b""
    while True:
        try: buf += ctl.read(4096)
        except Exception: return
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("ascii", "replace").strip()
            if t and not t.startswith("{"): say("ctl:", t)
threading.Thread(target=ctl_reader, daemon=True).start()
def stall(sec):
    if sec: say(f"  laptop stops reading for {sec} s")
    with open(a.stall_file, "w") as f: f.write(str(sec) if sec else "")

# 8 frames on W1, 40 sections, every frame busy in every section (several steps each = realistic upload size)
SETUP = """(mac) => {
  S.nodes = { [mac]: { name: 'test', wall: 'W1' } }; save();
  const tracks = [];
  for (let f = 1; f <= 8; f++) {
    const clips = [];
    for (let i = 0; i < 40; i++) {
      const t = i * 4000;
      clips.push({ t, fx: 'F', a: 0, b: 60 + f, dur: 900 });
      clips.push({ t: t + 1000, fx: 'B', a: 10, b: 90, per: 400 + 50 * f, dur: 1500 });
      clips.push({ t: t + 2600, fx: 'F', a: 60 + f, b: (i * 7 + f) % 50, dur: 1300 });
    }
    tracks.push({ frames: ['W1-' + f], clips });
  }
  const sh = normalizeShow({ name: 'load test', stepped: true, cues: Array.from({ length: 39 }, (_, i) => ({ t: (i + 1) * 4000, name: 'c' + (i + 2) })), tracks });
  S.shows = S.shows.filter(x => x.name !== 'load test'); S.shows.push(sh); save(); refreshShowSel();
  byId('showSel').value = sh.id; setMode('run'); cue = null; localStorage.removeItem('lf-cue'); cueSync();
  window.goLog = [];
  if (!window.origRequest) window.origRequest = request;
  request = async obj => {
    const t = performance.now(), r = await origRequest(obj);
    if (obj.cmd === 'go' || obj.cmd === 'prog') goLog.push({ cmd: obj.cmd, t, dt: performance.now() - t, ok: r.ok, err: r.err || '' });
    return r;
  };
  if (!window.toastOrig) window.toastOrig = toast;
  window.toasts = []; toast = (m, ...x) => { toasts.push(m); return toastOrig(m, ...x); };
  return sh.cues.length + 1;
}"""
GO = """async () => {
  const n0 = goLog.length, t = performance.now(), k0 = cue.k;
  await cueGo();
  const g = goLog.slice(n0);
  return { ms: performance.now() - t, k0, k: cue.k, gos: g.filter(x => x.cmd === 'go'), progs: g.filter(x => x.cmd === 'prog'),
           toasts: toasts.splice(0), up: window.WsTransport ? WsTransport.up : null, loaded: cue.loaded.slice() };
}"""
STATE = "() => ({ k: cue.k, loaded: cue.loaded.slice(), busy: cue.busy, preparing: cue.preparing, up: WsTransport.up, pending: Object.keys(pending).length, status: (byId('cueStatus')||{}).textContent })"

def press(p, why):
    r = p.evaluate(GO)
    gos = r["gos"]
    ok = bool(gos) and gos[-1]["ok"]          # the node confirmed (a resend inside the same press counts); time is reported
    progs = r["progs"]
    pinfo = f" uploads {sum(x['ok'] for x in progs)}/{len(progs)} ok" if progs else ""
    say(f"GO ({why}) -> section {r['k'] + 1}: {r['ms']:.0f} ms {'OK' if ok else 'FAILED'}  go:[" +
        "; ".join(f"{'ok' if g['ok'] else g['err']} {g['dt']:.0f}ms" for g in gos) + "]" + pinfo +
        (f"  toast: {r['toasts']}" if r["toasts"] else "") + ("" if ok else f"  state: {p.evaluate(STATE)}"))
    return ok, r["ms"]

results = []
with sync_playwright() as pw:
    b = pw.chromium.launch(channel="chrome")
    p = b.new_page()
    plog = open(os.path.join(out, "page.log"), "w", encoding="utf-8")
    p.on("pageerror", lambda e: say("PAGE ERROR", e))
    p.expose_function("pyLog", lambda s: (plog.write(f"{time.strftime('%H:%M:%S')} {s}\n"), plog.flush()))
    HOOK = """() => { const o = log; log = (k, s) => { if (!/"evt":"status"/.test(s)) pyLog(k + ' ' + s); return o(k, s); }; }"""
    def load():
        p.goto(a.page); p.wait_for_function("transport.name === 'ws' && WsTransport.up", timeout=30000); p.wait_for_timeout(1500)
        p.evaluate(HOOK); n = p.evaluate(SETUP, a.node); p.wait_for_timeout(6000); return n
    stall(0)
    ctl.write(f"wsdup {a.dup}\n".encode()); time.sleep(0.3)
    n = load()
    say(f"show with {n} sections; status lines x{a.dup}; state {p.evaluate(STATE)}")
    press(p, "first"); p.wait_for_timeout(4000)
    plan = [(float(s), w) for _ in range(a.repeat) for w in a.whens.split(",") for s in a.stalls.split(",")]
    for sec, when in plan:
        if p.evaluate("cue.k") >= n - 3: say("restart show"); n = load(); press(p, "restart"); p.wait_for_timeout(4000)
        say(f"--- round: stall {sec} s at {when}")
        if when == "upload":      # right after a GO, while the next section is being uploaded
            press(p, "before upload stall"); stall(sec); p.wait_for_timeout(max(a.pause, sec + 6) * 1000)
        elif when == "press":     # the laptop is not reading when the GO is pressed
            p.wait_for_timeout(a.pause * 1000); stall(sec); p.wait_for_timeout(300)
        else:                     # somewhere in the pause, over before the press
            p.wait_for_timeout(3000); stall(sec); p.wait_for_timeout(max(a.pause, sec + 4) * 1000)
        ok, ms = press(p, f"stall {sec}s at {when}")
        tries, waited, reload = 1, 0, False
        while not ok and waited < 30:
            gap = 1 if tries == 1 else 5
            p.wait_for_timeout(gap * 1000); waited += gap; tries += 1
            ok, _ = press(p, f"again after {waited} s")
        if not ok:
            reload = True; say("!!! still failing after 30 s of presses: reloading the page"); n = load(); press(p, "after reload")
        results.append((when, sec, tries, reload))
        p.wait_for_timeout(3000)
    b.close()
ctl.write(b"wsstats\n"); time.sleep(0.8)
say("==== SUMMARY", a.label, f"dup {a.dup} ====")
for when, sec, tries, reload in results:
    say(f"  stall {sec:>4} s at {when:7s}: " + ("first press OK" if tries == 1 else f"OK after {tries} presses" if not reload else "NEEDED RELOAD"))
say(f"  first press OK {sum(r[2] == 1 for r in results)}/{len(results)}, recovered by pressing again {sum(r[2] > 1 and not r[3] for r in results)}, needed reload {sum(r[3] for r in results)}")
