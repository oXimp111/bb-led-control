"""Page link test in a REAL browser: does GO still land on the first press after the page's Wi-Fi link died in a cue pause?
Needs the TEST control build (pio run -e control_wstest -t upload): AP at 192.168.10.1 (no clash with a 192.168.4.x home
network) and the serial command "wszombie" (the box silently stops talking to every open page = half-open socket).
The PC Wi-Fi adapter --wlan is joined to LED-Control; Chrome reaches 192.168.10.1 through it. The control board's USB
port (--ctl) is only used to send "wszombie" and to log its own lines.

    python tools/link_test.py 70:4B:CA:21:4F:EC --ctl COM5 --wlan Wi-Fi --page http://192.168.10.1/ --label new
    python tools/link_test.py ... --page http://localhost:8765/old.html --label old     (committed page, served from this PC)
    python tools/link_test.py ... --soak 60 --faults none,zombie,wifi,late    (repeat for 60 minutes, faults rotate;
        late = the link dies 0.5 s before the press, so the GO goes into the dead socket)

Each round: GO, optional fault, pause, GO. A GO counts as OK when every node confirmed it within 2 s of the press
(for "late" the pass mark is the same; the time shows how long the page needed to notice and resend). Results go to test_logs/linktest_<label>_<time>/ (test.log, page.log = the page's own log).
"""
import argparse, json, os, subprocess, threading, time
import serial
from playwright.sync_api import sync_playwright

ap = argparse.ArgumentParser()
ap.add_argument("node"); ap.add_argument("--ctl", required=True); ap.add_argument("--wlan", required=True)
ap.add_argument("--page", required=True); ap.add_argument("--label", required=True)
ap.add_argument("--pause", type=float, default=60, help="seconds between the fault and the GO")
ap.add_argument("--soak", type=float, default=0, help="minutes; 0 = one round per fault type")
ap.add_argument("--faults", default="none,zombie,wifi")
ap.add_argument("--fault-at", type=float, default=0, help="seconds into the pause at which the fault happens (default: at its start)")
a = ap.parse_args()
out = os.path.join("test_logs", f"linktest_{a.label}_{time.strftime('%Y%m%d_%H%M%S')}"); os.makedirs(out)
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

def wlan_state():
    o = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
    name, res = None, {}
    for line in o.splitlines():
        k, _, v = line.partition(":"); k, v = k.strip(), v.strip()
        if k == "Name": name = v
        if name == a.wlan and k in ("State", "SSID"): res[k] = v
    return res
def join():
    for i in range(10):
        if wlan_state().get("SSID") == "LED-Control" and wlan_state().get("State") == "connected": return True
        subprocess.run(["netsh", "wlan", "disconnect", f"interface={a.wlan}"], capture_output=True); time.sleep(5)
        subprocess.run(["netsh", "wlan", "connect", "name=LED-Control", "ssid=LED-Control", f"interface={a.wlan}"], capture_output=True); time.sleep(6)
    return False

SETUP = """(mac) => {
  S.nodes[mac] = { name: 'test', wall: 'W1' }; save();
  const one = (id, ...clips) => ({ frames: [id], clips });
  const clips = [];
  for (let i = 0; i < 40; i++) clips.push({ t: i * 2000, fx: 'F', a: i % 2 ? 100 : 0, b: i % 2 ? 0 : 100, dur: 2000 });
  const sh = normalizeShow({ name: 'link test', stepped: true, cues: Array.from({ length: 39 }, (_, i) => ({ t: (i + 1) * 2000, name: 'c' + (i + 2) })),
    tracks: [one('W1-1', ...clips)] });
  S.shows = S.shows.filter(x => x.name !== 'link test'); S.shows.push(sh); save(); refreshShowSel();
  byId('showSel').value = sh.id; setMode('run'); cue = null; localStorage.removeItem('lf-cue'); cueSync();
  window.goLog = [];
  const orig = request;
  request = async obj => {
    const t = performance.now(), r = await orig(obj);
    if (obj.cmd === 'go') goLog.push({ t, dt: performance.now() - t, ok: r.ok, err: r.err || '', t_ms: obj.t_ms });
    return r;
  };
  const toastOrig = toast;
  window.toasts = []; toast = (m, ...x) => { toasts.push(m); return toastOrig(m, ...x); };
  return sh.cues.length + 1;
}"""
GO = """async () => {
  const n0 = goLog.length, t = performance.now();
  await cueGo();
  return { ms: performance.now() - t, gos: goLog.slice(n0), k: cue.k, toasts: toasts.splice(0) };
}"""

def press(p, why):
    r = p.evaluate(GO)
    ok = bool(r["gos"]) and r["gos"][-1]["ok"] and r["ms"] < 2000   # a resend inside the same press counts (t_ms keeps it in time)
    tries = "; ".join(f"{'ok' if g['ok'] else g['err']} {g['dt']:.0f} ms t_ms={g['t_ms']}" for g in r["gos"])
    say(f"GO ({why}) -> section {r['k'] + 1}: {r['ms']:.0f} ms {'OK' if ok else 'FAILED'}  [{tries}]" + (f"  toast: {r['toasts']}" if r["toasts"] else ""))
    return ok, r["ms"]

def fault(kind):
    if kind == "zombie":
        say("FAULT zombie: the box silently stops talking to the page (half-open socket)"); ctl.write(b"wszombie\n")
    elif kind == "wifi":
        say("FAULT wifi: laptop Wi-Fi off for 15 s, then back on LED-Control")
        subprocess.run(["netsh", "wlan", "disconnect", f"interface={a.wlan}"], capture_output=True); time.sleep(15)
        say("  rejoined" if join() else "  !! could not rejoin LED-Control", wlan_state())

if not join(): raise SystemExit("PC adapter cannot join LED-Control")
say("wlan", wlan_state(), "page", a.page)
results = []
with sync_playwright() as pw:
    b = pw.chromium.launch(channel="chrome")
    p = b.new_page()
    plog = open(os.path.join(out, "page.log"), "w", encoding="utf-8")
    p.on("console", lambda m: (plog.write(f"{time.strftime('%H:%M:%S')} console {m.text}\n"), plog.flush()))
    p.on("pageerror", lambda e: say("PAGE ERROR", e))
    p.expose_function("pyLog", lambda s: (plog.write(f"{time.strftime('%H:%M:%S')} {s}\n"), plog.flush()))
    p.goto(a.page)
    try: p.wait_for_function("transport.name === 'ws' && (!window.WsTransport || WsTransport.up)", timeout=20000)
    except Exception: raise SystemExit(f"page is not on the Wi-Fi link (transport {p.evaluate('transport.name')}): fell back to the simulator?")
    p.wait_for_timeout(2000)
    # copy the page's own log (tx/rx/dev lines, minus heartbeats) into page.log
    p.evaluate("""() => { const o = log; log = (k, s) => { if (!/"evt":"status"/.test(s)) pyLog(k + ' ' + s); return o(k, s); }; }""")
    n = p.evaluate(SETUP, a.node); p.wait_for_timeout(4000)
    say(f"show with {n} sections loaded; transport {p.evaluate('transport.name')}; link badge: {p.evaluate('(byId(\"linkState\")||{}).textContent||\"(none, old page)\"')}")
    press(p, "first")
    kinds = a.faults.split(",")
    end = T0 + a.soak * 60
    i = 0
    while True:
        kind = kinds[i % len(kinds)]; i += 1
        if kind == "late":   # worst case: the link dies 0.5 s before the press, before the page can have noticed
            say(f"pause {a.pause:.0f} s"); p.wait_for_timeout(a.pause * 1000); fault("zombie"); p.wait_for_timeout(500)
        else:
            say(f"pause {a.pause:.0f} s" + (f", {kind} fault {a.fault_at:.0f} s in" if kind != "none" else ""))
            t_end = time.time() + a.pause
            p.wait_for_timeout((a.fault_at or 5) * 1000)
            if kind != "none": fault(kind)
            while time.time() < t_end:   # keep the page's event loop running; one line per minute so a long pause shows life
                p.wait_for_timeout(min(60, max(0.1, t_end - time.time())) * 1000)
                if t_end - time.time() > 1: say(f"  ... {t_end - time.time():.0f} s to GO; link: {p.evaluate('(byId(\"linkState\")||{}).textContent')}")
        ok, ms = press(p, f"after {kind}")
        results.append((kind, ok, ms))
        if not ok:   # the operator presses again
            p.wait_for_timeout(1000); ok2, _ = press(p, "second press")
            if not ok2: p.wait_for_timeout(5000); press(p, "third press")
            if not (ok2):
                say("reloading the page (what the installer does)"); p.reload(); p.wait_for_timeout(4000)
                p.evaluate("""() => { const o = log; log = (k, s) => { if (!/"evt":"status"/.test(s)) pyLog(k + ' ' + s); return o(k, s); }; }""")
                p.evaluate(SETUP, a.node); p.wait_for_timeout(4000); press(p, "after reload")
        if p.evaluate("cue.k") >= n - 2: p.evaluate(SETUP, a.node); p.wait_for_timeout(4000); press(p, "restart show")
        if (a.soak == 0 and i >= len(kinds)) or (a.soak and time.time() > end): break
    b.close()

say("==== SUMMARY", a.label, "====")
for kind in kinds:
    rs = [r for r in results if r[0] == kind]
    if rs: say(f"  {kind:7s}: first press OK {sum(r[1] for r in rs)}/{len(rs)}, worst {max(r[2] for r in rs):.0f} ms")
