"""Same as idle_go_test.py, but over the page's path: Wi-Fi to the control AP + WebSocket port 81.
The PC must be joined to LED-Control. Logs every disconnect and how long each GO took.
    python tools/idle_go_ws_test.py 70:4B:CA:21:4F:EC [--gaps 5,75,120] [--rounds 2]
"""
import argparse, json, sys, time
import websocket   # pip install websocket-client

ap = argparse.ArgumentParser()
ap.add_argument("node"); ap.add_argument("--host", default="192.168.4.1")
ap.add_argument("--bind", default="", help="local IP of the adapter on LED-Control")
ap.add_argument("--wlan", default="", help="Windows Wi-Fi interface name to rejoin LED-Control with if it gets moved off")
ap.add_argument("--gaps", default="5,75,120"); ap.add_argument("--rounds", type=int, default=2)
a = ap.parse_args()
T0 = time.time()
def ts(): return f"[{time.time() - T0:7.1f}s {time.strftime('%H:%M:%S')}]"
SECTIONS = ["F0-100:2s", "F100-20:2s"]
ws, rid, status_n = None, 2000, 0

def connect():
    global ws
    while True:
        try:
            sock = None
            if a.bind:   # PC on two networks with the same subnet: force the adapter joined to LED-Control
                import socket
                sock = socket.socket(); sock.settimeout(3); sock.bind((a.bind, 0)); sock.connect((a.host, 81))
            ws = websocket.create_connection(f"ws://{a.host}:81/", timeout=3, socket=sock); ws.settimeout(0.2)
            print(ts(), "ws connected"); return
        except Exception as e:
            print(ts(), "ws connect failed:", e)
            if a.wlan and "10049" in str(e):   # adapter lost LED-Control (Windows moved it): rejoin
                import subprocess   # disconnect first: Windows only sees LED-Control after a fresh scan
                subprocess.run(["netsh", "wlan", "disconnect", f"interface={a.wlan}"], capture_output=True); time.sleep(6)
                subprocess.run(["netsh", "wlan", "connect", "name=LED-Control", "ssid=LED-Control", f"interface={a.wlan}"], capture_output=True)
                print(ts(), "rejoined LED-Control")
            time.sleep(2)

def pump(until, want=None):
    """read until time `until`; return the result with id `want` if seen"""
    global status_n
    while time.time() < until:
        try: data = ws.recv()
        except websocket.WebSocketTimeoutException: continue
        except Exception as e:
            print(ts(), f"!! ws DROPPED: {type(e).__name__} {e}"); connect(); continue
        for line in str(data).split("\n"):
            if not line.startswith("{"): continue
            try: e = json.loads(line)
            except ValueError: continue
            if e.get("evt") == "status": status_n += 1
            if want is not None and e.get("evt") == "result" and e.get("id") == want: return e
    return None

def req(obj, wait=5.0):   # the page waits 5 s for a result
    global rid
    rid += 1; obj["id"] = rid; t = time.perf_counter()
    try: ws.send(json.dumps(obj))
    except Exception as e:
        print(ts(), f"!! send failed ({e}): the page would silently drop this"); connect()
        return None, wait * 1000
    e = pump(time.time() + wait, rid)
    return e, (time.perf_counter() - t) * 1000

connect()
ws.send(json.dumps({"cmd": "hello", "id": 1})); pump(time.time() + 1)
for k in (0, 1):
    e, _ = req({"cmd": "prog", "node": a.node, "slot": (k % 2) * 8 + 1, "text": SECTIONS[k]})
    if not (e and e.get("ok")): sys.exit(f"upload failed: {e}")
k = 0
e, ms = req({"cmd": "go", "node": a.node, "bank": 0, "play": [1], "stop": [], "t_ms": 0})
print(ts(), f"first GO {ms:.0f} ms {'ok' if e and e.get('ok') else e}")
rows = []
for _ in range(a.rounds):
    for g in [int(x) for x in a.gaps.split(",")]:
        req({"cmd": "prog", "node": a.node, "slot": ((k + 1) % 2) * 8 + 1, "text": SECTIONS[(k + 1) % 2]})
        status_n = 0
        print(ts(), f"idle {g:>3} s ...", flush=True)
        pump(time.time() + g)
        k += 1
        e, ms = req({"cmd": "go", "node": a.node, "bank": k % 2, "play": [1], "stop": [], "t_ms": 0})
        ok = bool(e and e.get("ok"))
        rows.append((g, ms, ok))
        print(ts(), f"GO after {g}s: {ms:6.0f} ms {'ok' if ok else 'FAIL ' + str(e)}  (status lines during idle: {status_n})"
              + ("  <-- slow" if ms >= 200 else ""))
print("\ngap  worst_ms  slow_or_failed")
for g in sorted({r[0] for r in rows}):
    sel = [r for r in rows if r[0] == g]
    print(f"{g:>3}  {max(r[1] for r in sel):8.0f}  {sum(r[1] >= 200 or not r[2] for r in sel)}/{len(sel)}")
