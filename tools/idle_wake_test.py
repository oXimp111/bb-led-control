"""Idle-wake test: does a node answer the FIRST command after a quiet period?
Needs only the control board (USB) and one paired node. Usage:
    python tools/idle_wake_test.py COM3 70:4B:CA:21:4F:EC [--gaps 5,30,75,120] [--rounds 2]
For each gap: wait that long with no commands, send `set ch1 level 0`, time the result.
A normal round trip is ~10-50 ms. >= 120 ms means the first send was lost and the control retried (CMD_RETRY_MS 120).
"""
import argparse, json, sys, time
import serial

ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("node")
ap.add_argument("--gaps", default="5,30,75,120")
ap.add_argument("--rounds", type=int, default=2)
a = ap.parse_args()

ser = serial.Serial(a.port, 115200, timeout=0.05)
rid = 100

def drain():
    while ser.read(256):
        pass

def req(obj, wait=3.0):
    global rid
    rid += 1; obj["id"] = rid
    t0 = time.perf_counter()
    ser.write((json.dumps(obj) + "\n").encode())
    buf = b""
    while time.perf_counter() - t0 < wait:
        buf += ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line[:1] != b"{":
                continue
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("evt") == "result" and e.get("id") == rid:
                return e, (time.perf_counter() - t0) * 1000
    return None, wait * 1000

time.sleep(1.5); drain()
e, ms = req({"cmd": "set", "node": a.node, "ch": 1, "level": 0, "fade_ms": 0})
if not e or not e.get("ok"):
    sys.exit(f"warm-up failed: {e}. Is the node paired and online?")
print(f"warm-up ok {ms:.0f} ms")

rows = []
for rnd in range(a.rounds):
    for g in [int(x) for x in a.gaps.split(",")]:
        print(f"idle {g:>3} s ...", end="", flush=True)
        end = time.time() + g
        while time.time() < end:
            ser.read(512)          # keep the port drained; send nothing
        e, ms = req({"cmd": "set", "node": a.node, "ch": 1, "level": 0, "fade_ms": 0})
        ok = bool(e and e.get("ok"))
        rows.append((g, ms, ok))
        print(f" {ms:6.0f} ms  {'ok' if ok else 'FAIL ' + str(e)}  {'<-- retried' if ms >= 110 else ''}")

print("\ngap  worst_ms  retried")
for g in sorted({r[0] for r in rows}):
    sel = [r for r in rows if r[0] == g]
    print(f"{g:>3}  {max(r[1] for r in sel):8.0f}  {sum(r[1] >= 110 or not r[2] for r in sel)}/{len(sel)}")
