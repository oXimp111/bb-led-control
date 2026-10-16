"""Does a dead Wi-Fi page connection slow the control board down? (the "GO lags after a long pause" bug)
Needs: control board on USB, one paired node, and a PC Wi-Fi adapter joined to LED-Control.
    python tools/dead_ws_test.py COM5 70:4B:CA:21:4F:EC --bind 192.168.4.2 --wlan Wi-Fi
1. reset the control board, time GO over USB (baseline)
2. open a WebSocket like the page, then drop the PC's Wi-Fi without closing it (= a laptop Wi-Fi blip)
3. time GO over USB again for --secs seconds
"""
import argparse, json, socket, subprocess, time
import serial, websocket

ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("node")
ap.add_argument("--bind", required=True); ap.add_argument("--wlan", required=True)
ap.add_argument("--secs", type=int, default=60)
a = ap.parse_args()
T0 = time.time()
def ts(): return f"[{time.time() - T0:6.1f}s {time.strftime('%H:%M:%S')}]"

ser = serial.Serial(); ser.port = a.port; ser.baudrate = 115200; ser.timeout = 0.05; ser.dtr = False; ser.rts = False
ser.open()
rid = 5000
def req(obj, wait=15.0):
    global rid
    rid += 1; obj["id"] = rid; t = time.perf_counter()
    ser.write((json.dumps(obj) + "\n").encode()); buf = b""
    while time.perf_counter() - t < wait:
        buf += ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line[:1] != b"{": continue
            try: e = json.loads(line)
            except ValueError: continue
            if e.get("evt") == "result" and e.get("id") == rid: return e, (time.perf_counter() - t) * 1000
    return None, wait * 1000

def go_series(label, secs, every=3.0):
    worst, n, k = 0, 0, 0
    end = time.time() + secs
    while time.time() < end:
        e, ms = req({"cmd": "go", "node": a.node, "bank": k % 2, "play": [1], "stop": [], "t_ms": 0}); k += 1
        ok = bool(e and e.get("ok")); worst = max(worst, ms); n += 1
        print(ts(), f"{label} GO {ms:7.0f} ms {'ok' if ok else 'FAIL'}" + ("   <-- LAG" if ms > 400 else ""), flush=True)
        time.sleep(every)
    return worst, n

print(ts(), "resetting control board"); ser.rts = True; time.sleep(0.1); ser.rts = False; time.sleep(5); ser.read(65536)
for slot, text in ((1, "F0-100:1s"), (9, "F100-20:1s")):
    e, _ = req({"cmd": "prog", "node": a.node, "slot": slot, "text": text})
    if not (e and e.get("ok")): raise SystemExit(f"upload failed {e} (node online?)")
base = go_series("baseline", 12)

s = socket.socket(); s.settimeout(3); s.bind((a.bind, 0)); s.connect(("192.168.4.1", 81))
ws = websocket.create_connection("ws://192.168.4.1:81/", timeout=3, socket=s)
ws.send(json.dumps({"cmd": "hello", "id": 1})); time.sleep(1)
print(ts(), "page-like WebSocket open; now dropping the PC's Wi-Fi WITHOUT closing it")
subprocess.run(["netsh", "wlan", "disconnect", f"interface={a.wlan}"], capture_output=True)
dead = go_series("dead-client", a.secs)
print(ts(), "rejoining LED-Control")
time.sleep(4); subprocess.run(["netsh", "wlan", "connect", "name=LED-Control", "ssid=LED-Control", f"interface={a.wlan}"], capture_output=True)
print(f"\nbaseline worst {base[0]:.0f} ms over {base[1]} GOs;  with a dead page connection worst {dead[0]:.0f} ms over {dead[1]} GOs")
