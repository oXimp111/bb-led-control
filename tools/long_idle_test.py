"""Long Wi-Fi soak: behaves like the page during a cue-list show with long pauses, and logs every link problem.
    python tools/long_idle_test.py 70:4B:CA:21:4F:EC --bind 192.168.4.2 --wlan Wi-Fi --minutes 60
- one WebSocket to the control board over the PC adapter joined to LED-Control (like the page; sends nothing while idle)
- GO after pauses cycling through --gaps seconds; each GO timed (page gives up after 5 s)
- a line per minute: status lines received (normal = 2 per second per node), so a stalled control loop shows up
- every drop: time, error, how long until the WebSocket is back (rejoins LED-Control if Windows moved the adapter)
"""
import argparse, json, socket, subprocess, time
import websocket

ap = argparse.ArgumentParser()
ap.add_argument("node"); ap.add_argument("--host", default="192.168.4.1")
ap.add_argument("--bind", required=True); ap.add_argument("--wlan", required=True)
ap.add_argument("--minutes", type=float, default=60)
ap.add_argument("--gaps", default="60,90,120,180,300")
a = ap.parse_args()
T0 = time.time()
def ts(): return f"[{(time.time() - T0) / 60:6.2f} min {time.strftime('%H:%M:%S')}]"
def say(*x): print(ts(), *x, flush=True)

ws, rid, status_n, drops = None, 9000, 0, []
def wlan_state():
    out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
    name = None; res = {}
    for line in out.splitlines():
        k, _, v = line.partition(":"); k = k.strip(); v = v.strip()
        if k == "Name": name = v
        if name == a.wlan and k in ("State", "SSID", "Signal"): res[k] = v
    return res

def rejoin():
    subprocess.run(["netsh", "wlan", "disconnect", f"interface={a.wlan}"], capture_output=True); time.sleep(6)
    subprocess.run(["netsh", "wlan", "connect", "name=LED-Control", "ssid=LED-Control", f"interface={a.wlan}"], capture_output=True); time.sleep(6)

def connect(reason):
    global ws
    t = time.time(); tries = 0
    while True:
        tries += 1
        try:
            s = socket.socket(); s.settimeout(3); s.bind((a.bind, 0)); s.connect((a.host, 81))
            ws = websocket.create_connection(f"ws://{a.host}:81/", timeout=3, socket=s); ws.settimeout(0.2)
            if reason != "start": say(f"ws BACK after {time.time() - t:.1f} s ({tries} tries)")
            return time.time() - t
        except Exception as e:
            if tries == 1 or tries % 10 == 0: say(f"connect failed ({e}); wlan {wlan_state()}")
            if "10049" in str(e) and tries % 3 == 1: say("adapter not on LED-Control: rejoining"); rejoin()
            time.sleep(2)

def pump(until, want=None):
    global status_n
    while time.time() < until:
        try: data = ws.recv()
        except websocket.WebSocketTimeoutException: continue
        except Exception as e:
            t = time.time(); say(f"!! ws DROPPED: {type(e).__name__} {e}; wlan {wlan_state()}")
            back = connect("drop"); drops.append((t, back)); continue
        for line in str(data).split("\n"):
            if not line.startswith("{"): continue
            try: e = json.loads(line)
            except ValueError: continue
            if e.get("evt") == "status": status_n += 1
            if want is not None and e.get("evt") == "result" and e.get("id") == want: return e
    return None

def req(obj, wait=5.0):
    global rid
    rid += 1; obj["id"] = rid; t = time.perf_counter()
    try: ws.send(json.dumps(obj))
    except Exception as e:
        say(f"!! send failed ({e}) = the page would lose this GO"); connect("send"); return None, wait * 1000
    e = pump(time.time() + wait, rid)
    return e, (time.perf_counter() - t) * 1000

connect("start"); say("connected, wlan", wlan_state())
for k, text in ((0, "F0-100:2s"), (1, "F100-20:2s")):
    e, _ = req({"cmd": "prog", "node": a.node, "slot": k * 8 + 1, "text": text})
    if not (e and e.get("ok")): raise SystemExit(f"upload failed: {e}")
gaps = [int(x) for x in a.gaps.split(",")]
end = T0 + a.minutes * 60
k, gi, gos = 0, 0, []
while time.time() < end:
    g = gaps[gi % len(gaps)]; gi += 1
    say(f"pause {g} s")
    stop = min(end, time.time() + g)
    while time.time() < stop:   # one status count per minute
        status_n = 0; t1 = min(stop, time.time() + 60); t_start = time.time()
        pump(t1)
        el = time.time() - t_start
        if el > 5: say(f"  status lines {status_n} in {el:.0f} s ({status_n / el:.1f}/s)")
    if time.time() >= end: break
    e, ms = req({"cmd": "go", "node": a.node, "bank": k % 2, "play": [1], "stop": [], "t_ms": 0}); k += 1
    ok = bool(e and e.get("ok")); gos.append((g, ms, ok))
    say(f"GO after {g} s pause: {ms:.0f} ms {'ok' if ok else 'FAIL ' + str(e)}" + ("   <-- LATE" if ms > 500 or not ok else ""))
    req({"cmd": "prog", "node": a.node, "slot": (k % 2) * 8 + 1, "text": "F0-100:2s" if k % 2 == 0 else "F100-20:2s"})

print("\n==== SUMMARY ====")
print(f"duration {(time.time() - T0) / 60:.1f} min, drops {len(drops)}")
for t, back in drops: print(f"  drop at {time.strftime('%H:%M:%S', time.localtime(t))}, back after {back:.1f} s")
bad = [x for x in gos if x[1] > 500 or not x[2]]
print(f"GOs {len(gos)}, late or failed {len(bad)}, worst {max((x[1] for x in gos), default=0):.0f} ms")
for g, ms, ok in bad: print(f"  after {g} s pause: {ms:.0f} ms {'ok' if ok else 'FAIL'}")
