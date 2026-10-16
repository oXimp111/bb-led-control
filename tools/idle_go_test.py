"""Cue-list GO after a long pause, the way the page does it (USB JSON to the control board).
    python tools/idle_go_test.py COM5 70:4B:CA:21:4F:EC [--gaps 5,75,120] [--rounds 2]
Each round: GO section k (bank k%2), immediately upload section k+1 into the other bank (page prefetch),
then stay silent for the gap and GO again. Times every GO result. Normal ~80 ms; >= 200 ms = retried.
"""
import argparse, json, sys, time
import serial

ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("node")
ap.add_argument("--gaps", default="5,75,120")
ap.add_argument("--rounds", type=int, default=2)
a = ap.parse_args()
ser = serial.Serial(a.port, 115200, timeout=0.05)
rid = 1000
SECTIONS = ["F0-100:2s", "F100-20:2s"]   # short sections: they finish and hold, like a cue waiting for its GO

def req(obj, wait=6.0):
    global rid
    rid += 1; obj["id"] = rid
    t0 = time.perf_counter()
    ser.write((json.dumps(obj) + "\n").encode())
    buf = b""
    while time.perf_counter() - t0 < wait:
        buf += ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line[:1] != b"{": continue
            try: e = json.loads(line)
            except ValueError: continue
            if e.get("evt") == "result" and e.get("id") == rid:
                return e, (time.perf_counter() - t0) * 1000
    return None, wait * 1000

def load(k):
    bank = k % 2
    e, ms = req({"cmd": "prog", "node": a.node, "slot": bank * 8 + 1, "text": SECTIONS[k % 2]})
    if not (e and e.get("ok")): sys.exit(f"upload section {k} failed: {e}")
    return ms

def go(k, t_ms=0):
    return req({"cmd": "go", "node": a.node, "bank": k % 2, "play": [1], "stop": [], "t_ms": t_ms})

time.sleep(1.0); ser.read(65536)
load(0); load(1)
k = 0
e, ms = go(k)
print(f"first GO (section {k + 1}) {ms:.0f} ms {'ok' if e and e.get('ok') else e}")
rows = []
for _ in range(a.rounds):
    for g in [int(x) for x in a.gaps.split(",")]:
        load(k + 1)                                   # prefetch next section into the other bank, as the page does
        print(f"idle {g:>3} s ...", end="", flush=True)
        end = time.time() + g
        while time.time() < end: ser.read(512)        # silent: no command, only node status lines arrive
        k += 1
        e, ms = go(k)
        ok = bool(e and e.get("ok"))
        rows.append((g, ms, ok))
        print(f" GO {ms:6.0f} ms  {'ok' if ok else 'FAIL ' + str(e)}  {'<-- retried' if ms >= 200 else ''}")
print("\ngap  worst_ms  slow_or_failed")
for g in sorted({r[0] for r in rows}):
    sel = [r for r in rows if r[0] == g]
    print(f"{g:>3}  {max(r[1] for r in sel):8.0f}  {sum(r[1] >= 200 or not r[2] for r in sel)}/{len(sel)}")
