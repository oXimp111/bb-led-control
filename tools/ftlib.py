"""Library for tools/fulltest.py: serial ports, packet/engine models (independent Python re-implementation of
lib/protocol/protocol.h and lib/engine), and the check collector. Nothing here talks to the page or uses the firmware's code,
so a mistake in the firmware cannot hide behind a copy of the same mistake."""
import json, re, struct, threading, time, sys, os
import serial

# ------------------------------------------------------------------ protocol model (protocol.h, LF_PROTO_VER 3)
PROTO_VER = 3
MAGIC = 0x464C
BCAST = bytes([0xFF] * 6)
T = dict(PAIR_REQ=1, PAIR_ACK=2, HEARTBEAT=3, ACK=4, SET=10, PROG=11, ASSIGN=12, PLAY=13, STOP=14, IDENTIFY=15, UNPAIR=16, SYNC=17, GO=18)
TNAME = {v: k for k, v in T.items()}
STEP = dict(H=0, F=1, B=2, K=3)


def mac_bytes(s): return bytes(int(x, 16) for x in s.split(":"))
def mac_str(b): return ":".join("%02X" % x for x in b)


def header(typ, target, seq=0): return struct.pack("<HBB6sH", MAGIC, PROTO_VER, typ, target, seq)


def parse_dur(tok):
    m = re.fullmatch(r"([\d.]+)(ms|s)?", tok)
    if not m: raise ValueError("duration " + tok)
    v = float(m.group(1))
    return int(v * 1000 + 0.5) if m.group(2) == "s" else int(v + 0.5)


def parse_prog(text):
    p = {"loop": False, "steps": []}
    for tok in re.split(r"[\s,]+", text.strip()):
        if not tok: continue
        if tok == "loop": p["loop"] = True; continue
        m = re.fullmatch(r"K:(\S+)", tok, re.I)
        if m: p["steps"].append(("K", 0, 0, 0, parse_dur(m.group(1)))); continue
        m = re.fullmatch(r"H(\d+):(\S+)", tok, re.I)
        if m: p["steps"].append(("H", int(m.group(1)), 0, 0, parse_dur(m.group(2)))); continue
        m = re.fullmatch(r"F(\d+)-(\d+):(\S+)", tok, re.I)
        if m: p["steps"].append(("F", int(m.group(1)), int(m.group(2)), 0, parse_dur(m.group(3)))); continue
        m = re.fullmatch(r"B(\d+)-(\d+)/(\S+?):(\S+)", tok, re.I)
        if m: p["steps"].append(("B", int(m.group(1)), int(m.group(2)), parse_dur(m.group(3)), parse_dur(m.group(4)))); continue
        raise ValueError("step " + tok)
    return p


def prog_bytes(p):          # LfProgram, 218 bytes: n_steps, flags, 24 x (type a b period_ms(u16) duration_ms(u32)), packed
    out = struct.pack("<BB", len(p["steps"]), 1 if p["loop"] else 0)
    for i in range(24):
        if i < len(p["steps"]):
            t, a, b, per, dur = p["steps"][i]
            out += struct.pack("<BBBHI", STEP[t], a, b, per, dur)
        else:
            out += bytes(9)
    assert len(out) == 218
    return out


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8): crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def prog_crc(p):
    if not p["steps"]: return 0
    return crc16(prog_bytes(p)) or 1


def total_ms(p): return sum(s[4] for s in p["steps"])


def level_at(p, t, entry=0.0):
    """Level 0..100 of program p after t ms (loop wraps, finished = end level, K keeps the carried level)."""
    if not p["steps"]: return 0.0
    tot = total_ms(p)
    if p["loop"] and tot > 0: t %= tot
    carry = entry
    for typ, a, b, per, dur in p["steps"]:
        if t < dur:
            if typ == "K": return carry
            if typ == "H": return float(a)
            if typ == "F": return float(b) if dur == 0 else a + (b - a) * t / dur
            ph = (t % per) / per; tri = 2 * ph if ph < .5 else 2 - 2 * ph
            return a + (b - a) * tri
        t -= dur
        if typ == "F": carry = float(b)
        elif typ != "K": carry = float(a)
    return carry


def level_range(p, t, entry, lo=20, hi=5):
    """(min, max) of the level over [t-lo, t+hi] ms: tolerance for the 10 ms output tick and message latency."""
    vals = [level_at(p, max(0, t + d), entry) for d in range(-lo, hi + 1, 1)]
    return min(vals), max(vals)


def duty_from_level(pct):    # lf::levelToDuty, 14 bit, gamma 2.2
    if pct <= 0: return 0
    if pct >= 100: return 16383
    return int(round((pct / 100.0) ** 2.2 * 16383))


# expected packet bodies (everything after the 12 byte header) for each command, as the control firmware must send them
def body(typ, **k):
    if typ == "SET": return struct.pack("<BBI", k["mask"], k["level"], k.get("fade", 0))
    if typ == "PROG": return struct.pack("<B", k["slot"]) + prog_bytes(k["prog"])
    if typ == "ASSIGN": return struct.pack("<BB", k["mask"], k["slot"])
    if typ == "PLAY": return struct.pack("<BI", k["mask"], k.get("offset", 0))
    if typ == "STOP": return struct.pack("<B", k["mask"])
    if typ == "IDENTIFY": return struct.pack("<BB", k["mask"], k["seconds"])
    if typ == "SYNC": return struct.pack("<BI", k["mask"], k["t"])
    if typ == "GO": return struct.pack("<BBBI", k["bank"], k["play"], k["stop"], k.get("offset", 0))
    raise KeyError(typ)


def mask_of(chs): return sum(1 << (c - 1) for c in chs)


# ------------------------------------------------------------------ serial ports
class Port:
    def __init__(self, name, baud, label, logdir):
        self.name, self.label = name, label
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = name, baud, 0.1
        self.s.dtr = False; self.s.rts = False
        self.s.open()
        self.lines = []           # (host_time, text)
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)
        self.log = open(os.path.join(logdir, label + ".log"), "w", encoding="utf-8")
        self.alive = True
        self.th = threading.Thread(target=self._read, daemon=True); self.th.start()

    def _read(self):
        buf = b""
        while self.alive:
            try:
                d = self.s.read(max(1, self.s.in_waiting))   # returns as soon as a byte arrives (read(4096) would wait out the timeout)
            except Exception as e:      # port vanished
                with self.cv: self.lines.append((time.time(), "!!PORT ERROR " + str(e))); self.cv.notify_all()
                return
            if not d: continue
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                text = ln.decode("utf-8", "replace").rstrip("\r")
                with self.cv:
                    self.lines.append((time.time(), text)); self.cv.notify_all()
                    self.log.write("%.3f %s\n" % (time.time(), text)); self.log.flush()

    def send(self, text):
        self.s.write((text + "\n").encode()); self.s.flush()
        with self.lock: self.log.write("%.3f >>> %s\n" % (time.time(), text)); self.log.flush()

    def mark(self):
        with self.lock: return len(self.lines)

    def wait(self, pred, start=0, timeout=5.0):
        """First line index >= start whose text satisfies pred -> (idx, text), or None on timeout."""
        end = time.time() + timeout; i = start
        with self.cv:
            while True:
                while i < len(self.lines):
                    if pred(self.lines[i][1]): return i, self.lines[i][1]
                    i += 1
                left = end - time.time()
                if left <= 0: return None
                self.cv.wait(left)

    def since(self, start):
        with self.lock: return [t for _, t in self.lines[start:]]

    def reset(self):      # EN pulse (DTR/RTS wiring of these boards: RTS low = reset)
        self.s.rts = True; time.sleep(0.12); self.s.rts = False

    def close(self):
        self.alive = False; time.sleep(0.2)
        try: self.s.close()
        except Exception: pass


class Node(Port):
    """The node under test (node_fulltest firmware, 460800 baud)."""
    def rx_list(self, start=0):
        out = []
        for t in self.since(start):
            if not t.startswith("@RX,"): continue
            f = t.split(",")
            if len(f) < 14: continue
            try:
                raw = bytes.fromhex(f[13]) if f[13] else b""
                out.append(dict(ms=int(f[1]), us=int(f[2]), src=f[3], len=int(f[4]), hdr_ok=f[5] == "1", ver=int(f[6] or 0), type=int(f[7] or 0),
                                target=f[8], seq=int(f[9] or 0), gap=int(f[10] or 0), dup=f[11] == "1", ack=int(f[12]) if f[12] != "" else None, raw=raw))
            except ValueError:
                pass
        return out

    def state(self, timeout=3.0):
        m = self.mark(); self.send("ft state")
        r = self.wait(lambda t: t == "@END", m, timeout)
        if not r: return None
        st = {"ch": {}, "prg": {}}
        for t in self.since(m)[: r[0] - m + 1]:
            f = t.split(",")
            if f[0] == "@ST": st.update(ms=int(f[1]), paired=f[2] == "1", pairing=f[3] == "1", hb=int(f[4]))
            elif f[0] == "@CH": st["ch"][int(f[1])] = dict(mode=f[2], level=float(f[3]), slot=int(f[4]), duty=int(f[5]), ident=f[6] == "1", ms=int(f[7]))
            elif f[0] == "@PRG": st["prg"][int(f[1])] = (int(f[2]), int(f[3]))
        return st

    def sample(self, ms=100):
        m = self.mark(); self.send("ft sample %d" % ms)
        r = self.wait(lambda t: t.startswith("@SMPEND"), m, ms / 1000 + 3)
        if not r: return None
        res = {}
        for t in self.since(m)[: r[0] - m]:
            f = t.split(",")
            if f[0] == "@SMP":
                res[int(f[4])] = dict(t0=int(f[1]), t1=int(f[2]), win_us=int(f[3]), duty=float(f[5]), rises=int(f[6]), freq=float(f[7]), duty_w=float(f[8]), level=float(f[9]))
        return res

    def report(self):
        m = self.mark(); self.send("ft report")
        r = self.wait(lambda t: t == "@RPTEND", m, 3)
        if not r: return None
        rep = {}
        for t in self.since(m)[: r[0] - m]:
            f = t.split(",")
            if f[0] == "@RPT": rep[f[1]] = int(f[2])
        return rep

    def now_ms(self):
        m = self.mark(); self.send("ft ping")
        r = self.wait(lambda t: t.startswith("@PONG,"), m, 2)
        return int(r[1].split(",")[1]) if r else None


class Control(Port):
    """The real control center firmware (USB 115200, one JSON object per line)."""
    def __init__(self, *a, **k):
        super().__init__(*a, **k); self.rid = 100

    def events(self, start=0):
        out = []
        for t in self.since(start):
            if t.startswith("{"):
                try: out.append(json.loads(t))
                except ValueError: pass
        return out

    def req(self, obj, timeout=8.0):
        self.rid += 1; obj = dict(obj); obj["id"] = self.rid
        m = self.mark(); t0 = time.time(); self.send(json.dumps(obj))
        def isres(t):
            return t.startswith("{") and '"evt":"result"' in t.replace(" ", "") and ('"id":%d' % self.rid) in t.replace(" ", "")
        r = self.wait(isres, m, timeout)
        if not r: return dict(ok=False, err="host_timeout", rtt=time.time() - t0)
        d = json.loads(r[1]); d["rtt"] = time.time() - t0
        return d


# ------------------------------------------------------------------ check collector
class Checks:
    def __init__(self, logdir):
        self.rows = []; self.section = ""; self.t0 = time.time()
        self.f = open(os.path.join(logdir, "checks.txt"), "w", encoding="utf-8")

    def sec(self, name):
        self.section = name
        self._p("\n=== %s ===" % name)

    def _p(self, s):
        print(s, flush=True); self.f.write(s + "\n"); self.f.flush()

    def check(self, name, cond, detail=""):
        cond = bool(cond)
        self.rows.append((self.section, name, cond, detail))
        self._p(("PASS  " if cond else "FAIL  ") + name + ("" if cond else "   -> " + str(detail)))
        return cond

    def info(self, s): self._p("      " + s)

    def summary(self):
        bad = [r for r in self.rows if not r[2]]
        self._p("\n%d checks, %d passed, %d FAILED  (%.0f s)" % (len(self.rows), len(self.rows) - len(bad), len(bad), time.time() - self.t0))
        for s, n, _, d in bad: self._p("  FAIL [%s] %s -> %s" % (s, n, d))
        return not bad
