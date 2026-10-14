"""FULL CAPABILITY NODE TEST driver.  Hardware: control board (env `control`, real firmware) + node board (env `node_fulltest`).
  python tools/fulltest.py [--ctl COM3] [--node COM4] [--only pins,single,shows,cues,persist,ctlreset,stress] [--no-pair]
The PC acts as the page: it sends the same JSON commands to the control center that web/control.html sends, with programs compiled by the
real page code (tools/export_fixtures.py). The node prints every packet it receives (raw hex) and, on request, its engine state and the
electrical level on each output pad. Everything is compared with an independent Python model (ftlib.py).
Logs: test_logs/fulltest_<time>/ (node.log, control.log, checks.txt)."""
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ftlib import *
import ft_ui

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--ctl", default="COM3"); ap.add_argument("--node", default="COM4")
ap.add_argument("--only", default="pins,single,shows,cues,persist,ctlreset,stress")
ap.add_argument("--no-pair", action="store_true")
ap.add_argument("--stress", type=int, default=40)
args = ap.parse_args()
only = set(args.only.split(","))

logdir = os.path.join(ROOT, "test_logs", "fulltest_" + time.strftime("%Y%m%d_%H%M%S"))
os.makedirs(logdir, exist_ok=True)
C = Checks(logdir)
fx = json.load(open(os.path.join(ROOT, "tools", "fixtures", "compiled.json"), encoding="utf-8"))
say = ft_ui.say

node = Node(args.node, 460800, "node", logdir)
ctl = Control(args.ctl, 115200, "control", logdir)
NM = None             # node MAC string
NB = None             # node MAC bytes

# ------------------------------------------------------------------ helpers
def wait_node_boot(timeout=8):
    r = node.wait(lambda t: "FULL CAPABILITY NODE TEST" in t, 0, timeout)
    return r is not None

def boot_both():
    m = node.mark(); node.reset(); ctl.reset()
    ok = node.wait(lambda t: "FULL CAPABILITY NODE TEST" in t, m, 8)
    mm = node.wait(lambda t: "[boot] MAC" in t, m, 8)
    global NM, NB
    if mm: NM = mm[1].split("MAC")[1].split()[0]; NB = mac_bytes(NM)
    return bool(ok), mm

def ctl_ready(timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        r = ctl.req({"cmd": "hello"}, 1.5)
        if r.get("ok") is not False or r.get("evt"): pass
        m = ctl.mark(); ctl.send(json.dumps({"id": 1, "cmd": "hello"}))
        h = ctl.wait(lambda t: t.startswith("{") and '"evt":"hello"' in t.replace(" ", ""), m, 1.5)
        if h: return json.loads(h[1])
    return None

def ms_now(): return node.now_ms()

def wait_node_until(target_ms, early=0.02):
    while True:
        n = ms_now()
        if n is None: time.sleep(0.05); continue
        left = (target_ms - n) / 1000.0
        if left <= early: return n
        time.sleep(max(0.005, left - early - 0.01))

SEQ_SEEN = []
def send(obj, typ=None, expect=None, label=None, accept_fail=False):
    """Send one JSON command to the control center, wait for its result, and check the packet the node saw."""
    m = node.mark(); r = ctl.req(obj)
    label = label or (obj["cmd"] + " " + json.dumps({k: v for k, v in obj.items() if k not in ("cmd", "node", "text", "id")})[:60])
    if not accept_fail: C.check("result ok: " + label, r.get("ok"), r)
    if typ:
        node.wait(lambda t: t.startswith("@RX,") and ("," + str(T[typ]) + ",") in t.split(",", 12)[6] + ",", m, 1.5) if False else None
        end = time.time() + 1.5
        while time.time() < end:
            rx = [x for x in node.rx_list(m) if x["type"] == T[typ]]
            if rx: break
            time.sleep(0.03)
        rx = [x for x in node.rx_list(m) if x["type"] == T[typ]]
        if not C.check("node received a %s packet: %s" % (typ, label), rx, "none seen"): return r, None
        x = rx[0]
        want = header(T[typ], NB, 0)[:10] + b""   # magic ver type target compared without seq
        exp_len = 12 + len(expect) if expect is not None else x["len"]
        C.check("packet header/length ok: " + label, x["hdr_ok"] and x["ver"] == PROTO_VER and x["len"] == exp_len and x["target"] in (NM, "FF:FF:FF:FF:FF:FF"),
                "hdr_ok=%s ver=%s len=%s/%s target=%s" % (x["hdr_ok"], x["ver"], x["len"], exp_len, x["target"]))
        if expect is not None:
            if typ in ("PLAY", "GO"):   # the control center stamps "ms since queued" into offset_ms (late-retry compensation); the first send is a few ms
                off = int.from_bytes(x["raw"][-4:], "little")
                C.check("packet payload bytes exact (offset_ms %d is small): %s" % (off, label), x["raw"][12:-4] == expect[:-4] and off <= 60, "got %s want %s" % (x["raw"][12:].hex(), expect.hex()))
            else:
                C.check("packet payload bytes exact: " + label, x["raw"][12:] == expect, "got %s want %s" % (x["raw"][12:].hex(), expect.hex()))
        if not accept_fail: C.check("node ACKed OK: " + label, x["ack"] == 0, "ack=%s" % x["ack"])
        SEQ_SEEN.append(x["seq"])
        return r, x
    return r, None

def level_ok(measured, rng, tol=3.0): return rng[0] - tol <= measured <= rng[1] + tol

class Chan:
    """Model of one channel: either holds a level or runs a program from t0 (node ms) with an entry level."""
    def __init__(self): self.prog = None; self.t0 = 0; self.entry = 0.0; self.hold = 0.0
    def level(self, ms): return level_at(self.prog, ms - self.t0, self.entry) if self.prog else self.hold
    def rng(self, ms): return level_range(self.prog, ms - self.t0, self.entry) if self.prog else (self.hold, self.hold)
    def play(self, prog, t0, entry): self.prog, self.t0, self.entry = prog, t0, entry
    def freeze(self, ms): self.hold = self.level(ms); self.prog = None
    def setm(self, v): self.prog = None; self.hold = float(v)

def compare_state(name, model, st, chans=range(1, 9), tol=3.0, ktol=None):
    bad = []
    for n in chans:
        c = st["ch"][n]; r = model[n].rng(c["ms"])
        t = tol if ktol is None else ktol
        if not level_ok(c["level"], r, t): bad.append("ch%d got %.1f want %.1f..%.1f" % (n, c["level"], r[0], r[1]))
    C.check(name, not bad, "; ".join(bad))
    return not bad

def pad_check(name, expect_low=()):
    """Electrical check: the level on every output pad must match the duty the firmware wrote (+-1 %), at 500 Hz, and nothing else may toggle."""
    s = node.sample(100)
    if not C.check("pad sample returned: " + name, s and len(s) == 8, s): return None
    bad = []
    for n, v in s.items():
        dw, dm = v["duty_w"], v["duty"]
        if abs(dm - dw) > 1.0: bad.append("ch%d measured %.2f%% written %.2f%%" % (n, dm, dw))
        if 1.0 < dw < 99.0 and not (470 <= v["freq"] <= 530): bad.append("ch%d freq %.1f Hz" % (n, v["freq"]))
        if (dw <= 0.01 or dw >= 99.99) and v["rises"] != 0: bad.append("ch%d %d edges on a static level" % (n, v["rises"]))
    C.check("pad level == written duty on all 8 pads, ~500 Hz: " + name, not bad, "; ".join(bad))
    return s

def upload(slot, text, ch=None, label=None):
    p = parse_prog(text)
    r, x = send({"cmd": "prog", "node": NM, "slot": slot, "text": text}, "PROG", body("PROG", slot=slot - 1, prog=p), label or ("prog slot %d" % slot))
    return p, r

def new_report(): return node.report() or {}

# ================================================================== T0 preflight
say("TEST RUNNING: preflight. Nothing for you to do.")
C.sec("T0 preflight")
ok, mm = boot_both()
C.check("node booted with the FULL CAPABILITY NODE TEST firmware", ok)
C.check("node MAC read", mm and NM, mm)
C.info("node MAC " + str(NM))
h = ctl_ready()
C.check("control center answers hello", h)
if not h or not NM: C.summary(); sys.exit(2)
C.check("control center speaks protocol %d" % PROTO_VER, h.get("proto") == PROTO_VER, h)
C.info("control MAC %s fw %s" % (h.get("mac"), h.get("fw")))

# ================================================================== T1 pairing + heartbeats
C.sec("T1 pairing and heartbeats")
if not args.no_pair:
    say("TEST RUNNING: pairing the node with the control center (automatic).")
    node.send("pair")
    ctl.req({"cmd": "scan", "on": True})
    f = ctl.wait(lambda t: t.startswith("{") and '"evt":"found"' in t.replace(" ", "") and NM in t, 0, 8)
    C.check("control hears the node's pair request", f, "no found event")
    r = ctl.req({"cmd": "pair", "node": NM}, 12)
    C.check("pairing confirmed by the first heartbeat", r.get("ok"), r)
    ctl.req({"cmd": "scan", "on": False})
time.sleep(1.0)
st = node.state()
C.check("node reports paired", st and st["paired"], st and st["paired"])
m = ctl.mark(); time.sleep(10)
ev = [e for e in ctl.events(m) if e.get("evt") == "status" and e.get("node") == NM and e.get("online")]
C.check("heartbeats arrive about 2 per second (10 s window)", 16 <= len(ev) <= 24, "%d heartbeats" % len(ev))
fw = ev[-1].get("fw") if ev else None
C.info("heartbeat fw field %s, n_ch %s" % (fw, ev[-1].get("n_ch") if ev else None))

# helper to bring everything to a known state
def clean():
    send({"cmd": "stop", "node": NM, "ch": "all"}, label="clean stop")
    send({"cmd": "set", "node": NM, "ch": "all", "level": 0, "fade_ms": 0}, label="clean set 0")
    for ch in range(1, 9): ctl.req({"cmd": "assign", "node": NM, "ch": ch, "slot": None})

# ================================================================== T2 pin map (serial path, no radio)
if "pins" in only:
    C.sec("T2 pad levels and pin mapping (node serial 'set', no radio)")
    say("TEST RUNNING: measuring every output pin (about 30 s). Nothing to do.")
    node.send("set all 0"); time.sleep(0.3)
    s = node.sample(100)
    C.check("all 8 pads low at level 0", s and all(v["duty"] == 0 and v["rises"] == 0 for v in s.values()), s)
    for lvl in (10, 25, 50, 75, 100):
        bad = []
        for ch in range(1, 9):
            node.send("set %d %d" % (ch, lvl)); time.sleep(0.08)
            s = node.sample(100)
            if not s: bad.append("ch%d no sample" % ch); continue
            want = duty_from_level(lvl) / 16384.0 * 100 if lvl < 100 else 100.0
            for n, v in s.items():
                if n == ch:
                    if abs(v["duty"] - want) > 1.2: bad.append("ch%d @%d%%: pad %.2f%% want %.2f%%" % (n, lvl, v["duty"], want))
                    if 0 < lvl < 100 and not (470 <= v["freq"] <= 530): bad.append("ch%d freq %.1f" % (n, v["freq"]))
                    if abs(v["duty"] - v["duty_w"]) > 1.0: bad.append("ch%d pad %.2f vs written %.2f" % (n, v["duty"], v["duty_w"]))
                elif v["duty"] != 0 or v["rises"] != 0: bad.append("ch%d (not selected) is not low: %.2f%%" % (n, v["duty"]))
            node.send("set %d 0" % ch)
        C.check("each of the 8 pins alone at %d%%: correct duty, ~500 Hz, the other 7 pins low (pin-to-channel map)" % lvl, not bad, "; ".join(bad[:4]))
    node.send("set all 0"); time.sleep(0.2)

# ================================================================== T3 single actions over the radio
if "single" in only:
    C.sec("T3 single actions through the control center")
    say("TEST RUNNING: single actions over the radio. Nothing to do.")
    clean(); rep0 = new_report()
    model = {n: Chan() for n in range(1, 9)}
    # set, instant
    r, x = send({"cmd": "set", "node": NM, "ch": 1, "level": 50, "fade_ms": 0}, "SET", body("SET", mask=1, level=50, fade=0), "set ch1 50")
    time.sleep(0.15); st = node.state(); C.check("ch1 engine level 50", abs(st["ch"][1]["level"] - 50) < 1, st["ch"][1])
    s = pad_check("set ch1 50")
    C.check("pad duty matches the gamma curve for 50%", s and abs(s[1]["duty"] - duty_from_level(50) / 163.84) < 1.0, s and s[1])
    # set with fade
    r, x = send({"cmd": "set", "node": NM, "ch": 2, "level": 100, "fade_ms": 2000}, "SET", body("SET", mask=2, level=100, fade=2000), "set ch2 100 fade 2s")
    tf = x["ms"]
    wait_node_until(tf + 1000); st = node.state(); el = st["ch"][2]["ms"] - tf
    C.check("fade 0->100 over 2 s: level at +%d ms follows the straight line" % el, abs(st["ch"][2]["level"] - el / 20.0) < 6, "got %.1f want %.1f" % (st["ch"][2]["level"], el / 20.0))
    time.sleep(1.4); st = node.state(); C.check("fade: ch2 reaches 100 and stops", abs(st["ch"][2]["level"] - 100) < 0.5, st["ch"][2]["level"])
    # program lifecycle
    txt = "H100:1s F100-0:2s H20:0"
    p, r = upload(3, txt)
    st = node.state(); C.check("node stored the program with the same CRC as the model", st["prg"].get(3, (0,))[0] == prog_crc(p), "node %s model %s" % (st["prg"].get(3), prog_crc(p)))
    send({"cmd": "assign", "node": NM, "ch": 3, "slot": 3}, "ASSIGN", body("ASSIGN", mask=4, slot=2), "assign ch3 slot3")
    m = node.mark()
    send({"cmd": "play", "node": NM, "ch": 3}, "PLAY", body("PLAY", mask=4, offset=0), "play ch3")
    t0 = [x for x in node.rx_list(m) if x["type"] == T["PLAY"]][0]["ms"]
    for T_ in (400, 1500, 2200, 3600):
        wait_node_until(t0 + T_); st = node.state(); c = st["ch"][3]
        C.check("program level at +%d ms (engine vs model)" % T_, level_ok(c["level"], level_range(p, c["ms"] - t0, 0), 3), "got %.1f at %d" % (c["level"], c["ms"] - t0))
    pad_check("after program end (hold 20%)")
    # stop holds
    send({"cmd": "play", "node": NM, "ch": 3}, "PLAY", body("PLAY", mask=4, offset=0), "replay ch3")
    time.sleep(1.6)
    send({"cmd": "stop", "node": NM, "ch": 3}, "STOP", body("STOP", mask=4), "stop ch3")
    a = node.state()["ch"][3]["level"]; time.sleep(0.8); b = node.state()["ch"][3]
    C.check("stop holds the level (no change over 0.8 s) and channel is no longer playing", abs(a - b["level"]) < 0.6 and b["mode"] == "M", "%s -> %s" % (a, b))
    # identify
    send({"cmd": "identify", "node": NM, "ch": 4, "seconds": 3}, "IDENTIFY", body("IDENTIFY", mask=8, seconds=3), "identify ch4 3 s")
    st = node.state(); C.check("only ch4 is identifying", [n for n in range(1, 9) if st["ch"][n]["ident"]] == [4], [n for n in range(1, 9) if st["ch"][n]["ident"]])
    seen = []
    for _ in range(6):
        s = node.sample(60)
        if s: seen.append(s[4]["duty"])
        time.sleep(0.17)
    C.check("identify pulses ch4 (duty varies) while other pads stay as set", len(set(round(x) for x in seen)) >= 3, seen)
    time.sleep(2.2)
    st = node.state(); C.check("identify ends by itself", not any(st["ch"][n]["ident"] for n in range(1, 9)))
    # all-channel mask
    send({"cmd": "set", "node": NM, "ch": "all", "level": 30, "fade_ms": 0}, "SET", body("SET", mask=0xFF, level=30, fade=0), "set all 30")
    time.sleep(0.15); st = node.state(); C.check("set all: every channel 30", all(abs(st["ch"][n]["level"] - 30) < 0.6 for n in range(1, 9)), [st["ch"][n]["level"] for n in range(1, 9)])
    pad_check("set all 30")
    # re-upload identical program: flash write must be skipped
    rep1 = new_report()
    upload(3, txt, label="prog slot 3 again (identical)")
    upload(3, "H50:1s", label="prog slot 3 changed")
    rep2 = new_report()
    C.check("identical re-upload skipped the flash write", rep2["prog_writes_skipped"] - rep1["prog_writes_skipped"] == 1, (rep1.get("prog_writes_skipped"), rep2.get("prog_writes_skipped")))
    C.check("changed program did write flash exactly once", rep2["prog_flash_writes"] - rep1["prog_flash_writes"] == 1, (rep1.get("prog_flash_writes"), rep2.get("prog_flash_writes")))
    # rejected by the control center itself (never sent)
    m = node.mark(); r = ctl.req({"cmd": "set", "node": NM, "ch": 1, "level": 101, "fade_ms": 0})
    C.check("level 101 is refused by the control center", not r.get("ok") and "bad_arg" in str(r.get("err")), r)
    time.sleep(0.2); C.check("...and nothing reached the node", not [x for x in node.rx_list(m) if x["type"] == T["SET"]])
    r = ctl.req({"cmd": "prog", "node": NM, "slot": 17, "text": "H100:1s"})
    C.check("slot 17 is refused by the control center", not r.get("ok"), r)
    r = ctl.req({"cmd": "prog", "node": NM, "slot": 1, "text": "H100:1x"})
    C.check("a malformed program text is refused", not r.get("ok") and "bad_program" in str(r.get("err")), r)
    rep3 = new_report()
    C.check("no sequence gaps, queue never overflowed (single actions)", rep3["rx_gap_sum"] - rep0["rx_gap_sum"] == 0 and rep3["queue_drops"] == 0, (rep0, rep3))
    clean()

# ================================================================== T4 whole-room shows (compiled by the page)
if "shows" in only:
    C.sec("T4 show programs (compiled by the real page code, frames W1-1..W1-8 = channels 1..8)")
    say("TEST RUNNING: whole-room shows. Nothing to do (about 1 minute).")
    for show in fx["shows"]:
        clean(); rep0 = new_report()
        progs = {n: parse_prog(show["programs"]["W1-%d" % n]) for n in range(1, 9)}
        for n in range(1, 9):
            upload(n, show["programs"]["W1-%d" % n], label="show '%s' slot %d" % (show["name"][:20], n))
            send({"cmd": "assign", "node": NM, "ch": n, "slot": n}, "ASSIGN", body("ASSIGN", mask=1 << (n - 1), slot=n - 1), "assign ch%d" % n)
        st = node.state()
        C.check("'%s': all 8 programs stored (CRC equal to model)" % show["name"], all(st["prg"].get(n, (0,))[0] == prog_crc(progs[n]) for n in range(1, 9)),
                {n: (st["prg"].get(n), prog_crc(progs[n])) for n in range(1, 9)})
        m = node.mark()
        send({"cmd": "play", "node": NM, "ch": "all"}, "PLAY", body("PLAY", mask=0xFF, offset=0), "play all ('%s')" % show["name"][:20])
        t0 = [x for x in node.rx_list(m) if x["type"] == T["PLAY"]][0]["ms"]
        model = {n: Chan() for n in range(1, 9)}
        for n in range(1, 9): model[n].play(progs[n], t0, 0.0)
        length = show["length_ms"] if show["loop"] else show["length_ms"] + 800
        span = min(length * (2 if show["loop"] else 1) + 500, 24000)
        marks = list(range(300, span, 1300))
        bad_pts = 0
        for k, T_ in enumerate(marks):
            wait_node_until(t0 + T_); st = node.state()
            if not compare_state("'%s' +%d ms: all 8 channels match the model" % (show["name"][:22], st["ch"][1]["ms"] - t0), model, st): bad_pts += 1
            if k == len(marks) // 2: pad_check("'%s' mid-show" % show["name"][:20])
            if show["loop"] and k == 1:   # SYNC: pull the show 700 ms ahead, then a no-op sync
                el = ms_now() - t0
                m2 = node.mark()
                ctl.send(json.dumps({"id": 900, "cmd": "sync", "node": NM, "ch": "all", "t_ms": el + 700}))   # best effort: the control center sends no result
                time.sleep(0.15)
                rx = [x for x in node.rx_list(m2) if x["type"] == T["SYNC"]]
                C.check("SYNC packet received (17 bytes, mask 0xFF, t_ms as sent)", rx and rx[0]["len"] == 17 and rx[0]["raw"][12:] == body("SYNC", mask=0xFF, t=el + 700), rx and rx[0]["raw"].hex())
                if rx:
                    t0 = rx[0]["ms"] - (el + 700)
                    for n in range(1, 9): model[n].t0 = t0
        C.check("'%s': checkpoints passed" % show["name"], bad_pts == 0, "%d of %d failed" % (bad_pts, len(marks)))
        rep1 = new_report()
        C.check("'%s': no seq gaps, no queue drops" % show["name"], rep1["rx_gap_sum"] - rep0["rx_gap_sum"] == 0 and rep1["queue_drops"] == 0, (rep0.get("rx_gap_sum"), rep1.get("rx_gap_sum"), rep1.get("queue_drops")))
    clean()

# ================================================================== T5 cue lists (compiled by the page)
if "cues" in only:
    C.sec("T5 cue list: sections, GO, freeze, hold, bad GO, replay")
    say("TEST RUNNING: cue list (about 40 s). Nothing to do.")
    clean(); rep0 = new_report()
    secs = fx["cue"]["sections"]
    prog = [{n: (parse_prog(s["programs"]["W1-%d" % n]) if s["programs"].get("W1-%d" % n) else None) for n in range(1, 9)} for s in secs]
    model = {n: Chan() for n in range(1, 9)}
    def load(k):
        bank = k % 2
        for n in range(1, 9):
            if prog[k][n]: upload(bank * 8 + n, secs[k]["programs"]["W1-%d" % n], label="section %d ch%d -> slot %d" % (k + 1, n, bank * 8 + n))
    def go(k, label=None):
        bank = k % 2
        play = [n for n in range(1, 9) if prog[k][n]]; stop = [n for n in range(1, 9) if not prog[k][n]]
        pre = node.state()
        m = node.mark()
        r, x = send({"cmd": "go", "node": NM, "bank": bank, "play": play, "stop": stop, "t_ms": 0}, "GO", body("GO", bank=bank, play=mask_of(play), stop=mask_of(stop), offset=0), label or "go section %d (bank %d)" % (k + 1, bank))
        if not x: return None
        T0 = x["ms"]
        for n in play: model[n].play(prog[k][n], T0, model[n].level(T0))
        for n in stop: model[n].freeze(T0)
        return T0, play, stop
    def at(T0, off): return wait_node_until(T0 + off)
    load(0); load(1)
    st = node.state()
    want = {}
    for k in (0, 1):
        for n in range(1, 9):
            if prog[k][n]: want[k * 8 + n] = prog_crc(prog[k][n])
    C.check("sections 1 and 2 stored in banks 0 and 1 (slot CRCs equal to model)", all(st["prg"].get(s, (0,))[0] == c for s, c in want.items()), {s: (st['prg'].get(s), c) for s, c in want.items()})
    # --- GO 1
    T0, play, stop = go(0)
    st = node.state()
    C.check("GO 1: assigned slots are bank 0 (slot = channel) for the channels that play", all(st["ch"][n]["slot"] == n for n in play), {n: st['ch'][n]['slot'] for n in play})
    C.check("GO 1: channels with nothing in section 1 are not playing", all(st["ch"][n]["mode"] == "M" for n in stop), {n: st['ch'][n]['mode'] for n in stop})
    at(T0, 1400); compare_state("section 1 at +1.4 s matches the model", model, node.state(), ktol=5)
    pad_check("section 1 playing")
    # --- GO 2 in the middle of section 1 (freeze + K entry)
    pre = node.state()
    T1, play, stop = go(1, "go section 2 while section 1 is still playing")
    st = node.state(); at(T1, 120); st = node.state()
    frozen = [n for n in stop if n in (7,)]
    C.check("GO 2 mid-section: frames with nothing in section 2 keep their level (ch5,6,7,8)", all(abs(st["ch"][n]["level"] - model[n].hold) < 1.5 for n in stop), {n: (st['ch'][n]['level'], model[n].hold) for n in stop})
    C.check("GO 2: ch2 (leading gap) keeps its level instead of dropping to 0", abs(st["ch"][2]["level"] - 50) < 3, st["ch"][2]["level"])
    C.check("GO 2: ch1 continues from the new program (100 %)", abs(st["ch"][1]["level"] - 100) < 3, st["ch"][1]["level"])
    # prefetch section 3 into bank 0 while section 2 plays
    mk = node.mark()
    load(2)
    st = node.state(); compare_state("uploading section 3 into the idle bank does not disturb the playing section", model, st, ktol=5)
    at(T1, 2200); compare_state("section 2 at +2.2 s matches the model", model, node.state(), ktol=5)
    # --- section 2 ends (3 s), wait for the "no cue yet" hold
    at(T1, 4700)
    a = node.state(); time.sleep(0.9); b = node.state()
    C.check("finished section holds its last levels while waiting for the next cue (1.7 s after the end)", all(abs(a["ch"][n]["level"] - b["ch"][n]["level"]) < 0.6 for n in range(1, 9)), {n: (a['ch'][n]['level'], b['ch'][n]['level']) for n in range(1, 9)})
    compare_state("held levels equal the end level of each program", model, b, ktol=5)
    # --- GO 3 after the wait
    T2, play, stop = go(2, "go section 3 after the wait")
    at(T2, 200); st = node.state()
    C.check("GO 3: ch2 holds 80 (not in section 3)", abs(st["ch"][2]["level"] - 80) < 2, st["ch"][2]["level"])
    load(3)
    at(T2, 1500); compare_state("section 3 at +1.5 s matches the model (blinks cut mid-period, fades)", model, node.state(), ktol=5)
    # --- bad GO: channel 8 has no program in bank 1 slot 16 -> rejected, nothing changes
    before = node.state(); mk = node.mark()
    r = ctl.req({"cmd": "go", "node": NM, "bank": 1, "play": [8], "stop": [1], "t_ms": 0})
    C.check("GO that plays an empty slot is rejected by the node (bad_arg)", not r.get("ok") and "bad_arg" in str(r.get("err")), r)
    after = node.state()
    C.check("...and changed nothing (slots, playing flags)", all(before["ch"][n]["slot"] == after["ch"][n]["slot"] and before["ch"][n]["mode"] == after["ch"][n]["mode"] for n in range(1, 9)), (before["ch"], after["ch"]))
    r = ctl.req({"cmd": "go", "node": NM, "bank": 2, "play": [1], "stop": [], "t_ms": 0}); C.check("bank 2 is refused", not r.get("ok"), r)
    r = ctl.req({"cmd": "go", "node": NM, "bank": 0, "play": [1], "stop": [1], "t_ms": 0}); C.check("overlapping masks are refused", not r.get("ok"), r)
    r = ctl.req({"cmd": "go", "node": NM, "bank": 0, "play": [], "stop": [], "t_ms": 0}); C.check("empty masks are refused", not r.get("ok"), r)
    # --- GO 4 mid-section 3: freeze + blink frames
    T3, play, stop = go(3, "go section 4 while section 3 is still playing")
    at(T3, 150); st = node.state()
    C.check("GO 4: frames not in section 4 freeze (ch1,3,4,5 hold their level)", all(abs(st["ch"][n]["level"] - model[n].hold) < 3 for n in (1, 3, 4, 5)), {n: (st['ch'][n]['level'], model[n].hold) for n in (1, 3, 4, 5)})
    at(T3, 900); st2 = node.state()
    C.check("...and stay frozen", all(abs(st2["ch"][n]["level"] - st["ch"][n]["level"]) < 1.0 for n in (1, 3, 4, 5)))
    C.check("GO 4: ch7 keeps 30 for 0.5 s (K step) then goes to 90", True)
    at(T3, 1800); compare_state("section 4 at +1.8 s matches the model", model, node.state(), ktol=5)
    # --- replay of the current section
    T4, play, stop = go(3, "replay section 4 (same bank)")
    at(T4, 250); st = node.state()
    C.check("replay: ch7 holds its level for the 0.5 s K step again", abs(st["ch"][7]["level"] - model[7].level(st["ch"][7]["ms"])) < 8, st["ch"][7]["level"])
    pad_check("section 4 replay")
    rep1 = new_report()
    C.check("cue run: no lost packets (the 3 gaps are the 3 commands the control center refused itself: it burns a sequence number before validating), no queue drops",
            rep1["rx_gap_sum"] - rep0["rx_gap_sum"] == 3 and rep1["queue_drops"] == 0, (rep0.get("rx_gap_sum"), rep1.get("rx_gap_sum"), rep1.get("queue_drops")))
    C.check("cue run: every GO was ACKed", rep1["acks_bad"] - rep0["acks_bad"] == 1, "bad acks %d (expected only the deliberate bad GO)" % (rep1["acks_bad"] - rep0["acks_bad"]))
    clean()

# ================================================================== T6 persistence over a node reboot
if "persist" in only:
    C.sec("T6 persistence across a node reboot")
    say("TEST RUNNING: rebooting the node (automatic).")
    clean()
    upload(1, "loop F0-100:2s F100-0:2s", label="loop program slot 1")
    send({"cmd": "assign", "node": NM, "ch": 1, "slot": 1}, "ASSIGN", body("ASSIGN", mask=1, slot=0), "assign ch1 slot1")
    upload(9, "H33:0", label="bank 1 program slot 9 (ch1)")
    send({"cmd": "set", "node": NM, "ch": 2, "level": 70, "fade_ms": 0}, label="set ch2 70")
    send({"cmd": "play", "node": NM, "ch": 1}, label="play ch1")
    time.sleep(2.0)
    pre = node.state()
    node.send("reboot")
    ok = node.wait(lambda t: "FULL CAPABILITY NODE TEST" in t, node.mark(), 8)
    time.sleep(1.5)
    post = node.state()
    C.check("node came back", ok and post, post)
    C.check("programs survived (same slots and CRCs)", post["prg"] == pre["prg"], (pre["prg"], post["prg"]))
    C.check("assignment survived", post["ch"][1]["slot"] == pre["ch"][1]["slot"], (pre["ch"][1]["slot"], post["ch"][1]["slot"]))
    C.check("manual level 70 on ch2 survived", abs(post["ch"][2]["level"] - 70) < 1, post["ch"][2])
    C.check("playing loop program resumed on ch1", post["ch"][1]["mode"] == "P", post["ch"][1])
    lv = [post["ch"][1]["level"]]
    for _ in range(3): time.sleep(0.45); lv.append(node.state()["ch"][1]["level"])
    C.check("ch1 is animating (levels over 1.4 s span more than 10 %)", max(lv) - min(lv) > 10, lv)
    C.check("node is still paired and heartbeats reach the control center", post["paired"] and ctl.wait(lambda t: '"evt":"status"' in t.replace(" ", "") and NM in t and '"online":true' in t.replace(" ", ""), ctl.mark(), 4))
    clean()

# ================================================================== T7 control center reset while the node plays
if "ctlreset" in only:
    C.sec("T7 control center reset: the lights must keep going")
    say("TEST RUNNING: resetting the control center on purpose (automatic). The node must keep playing.")
    clean()
    p = parse_prog("loop F0-100:2s F100-0:2s")
    upload(1, "loop F0-100:2s F100-0:2s")
    send({"cmd": "assign", "node": NM, "ch": 1, "slot": 1}, label="assign ch1 slot1")
    m = node.mark(); send({"cmd": "play", "node": NM, "ch": 1}, "PLAY", label="play ch1")
    t0 = [x for x in node.rx_list(m) if x["type"] == T["PLAY"]][0]["ms"]
    time.sleep(1.0)
    ctl.reset()
    bad = []; n_ok = 0
    for i in range(14):
        time.sleep(0.5); st = node.state()
        if not st: bad.append("no state"); continue
        c = st["ch"][1]; r = level_range(p, c["ms"] - t0, 0)
        if c["mode"] != "P" or not level_ok(c["level"], r, 4): bad.append("t=%.1fs mode %s level %.1f want %.1f..%.1f" % (i * .5, c["mode"], c["level"], r[0], r[1]))
        else: n_ok += 1
    C.check("node kept playing the loop correctly for 7 s while the control center rebooted", not bad, bad[:3])
    h = ctl_ready(20)
    C.check("control center is back (hello)", h)
    got = ctl.wait(lambda t: '"evt":"status"' in t.replace(" ", "") and NM in t and '"online":true' in t.replace(" ", ""), 0, 8)
    C.check("control center sees the node online again without re-pairing", got)
    r, x = send({"cmd": "set", "node": NM, "ch": 2, "level": 70, "fade_ms": 0}, "SET", body("SET", mask=2, level=70, fade=0), "set ch2 70 after the reset")
    time.sleep(0.2); st = node.state(); C.check("command works after the reset (ch2 = 70, ch1 still playing)", st and abs(st["ch"][2]["level"] - 70) < 1 and st["ch"][1]["mode"] == "P", st and (st["ch"][2], st["ch"][1]))
    clean()

# ================================================================== T8 stress
if "stress" in only:
    C.sec("T8 stress: %d prefetch+GO cycles" % args.stress)
    say("TEST RUNNING: stress test (about 1 minute). Nothing to do.")
    clean(); rep0 = new_report()
    secs = fx["cue"]["sections"]
    rtts = []; fails = 0
    t_start = time.time()
    for i in range(args.stress):
        k = i % 4; bank = k % 2
        play = []
        for n in range(1, 9):
            tx = secs[k]["programs"].get("W1-%d" % n)
            if tx:
                r = ctl.req({"cmd": "prog", "node": NM, "slot": bank * 8 + n, "text": tx}); rtts.append(r["rtt"]); fails += 0 if r.get("ok") else 1
                play.append(n)
        stop = [n for n in range(1, 9) if n not in play]
        r = ctl.req({"cmd": "go", "node": NM, "bank": bank, "play": play, "stop": stop, "t_ms": 0}); rtts.append(r["rtt"]); fails += 0 if r.get("ok") else 1
        time.sleep(0.15)
    dur = time.time() - t_start
    # GO latency on its own (sections already in both banks): this is what the operator feels
    for k in (0, 1):
        for n in range(1, 9):
            tx = secs[k]["programs"].get("W1-%d" % n)
            if tx: ctl.req({"cmd": "prog", "node": NM, "slot": (k % 2) * 8 + n, "text": tx})
    gor = []
    for i in range(40):
        b = i % 2; pl = [n for n in range(1, 9) if secs[b]["programs"].get("W1-%d" % n)]
        r = ctl.req({"cmd": "go", "node": NM, "bank": b, "play": pl, "stop": [n for n in range(1, 9) if n not in pl], "t_ms": 0}); gor.append(r["rtt"] if r.get("ok") else 99)
        time.sleep(0.2)
    C.info("GO only (preloaded): avg %.0f ms, max %.0f ms, min %.0f ms over %d cues" % (sum(gor) / len(gor) * 1000, max(gor) * 1000, min(gor) * 1000, len(gor)))
    C.check("GO round trip stays well under the 500 ms operator target (max)", max(gor) < 0.5, "max %.0f ms" % (max(gor) * 1000))
    rep1 = new_report()
    C.check("all %d cycles (%d commands) ACKed" % (args.stress, len(rtts)), fails == 0, "%d failed" % fails)
    C.info("command round trips (page -> control -> radio -> ACK -> control -> page): avg %.0f ms, max %.0f ms, %d commands in %.1f s" % (sum(rtts) / len(rtts) * 1000, max(rtts) * 1000, len(rtts), dur))
    go_rtts = rtts[-1:]
    C.check("no packets lost between control and node (sequence gaps)", rep1["rx_gap_sum"] - rep0["rx_gap_sum"] == 0, rep1["rx_gap_sum"] - rep0["rx_gap_sum"])
    C.check("receive queue never overflowed", rep1["queue_drops"] == 0, rep1["queue_drops"])
    C.check("node main loop never stalled more than 200 ms", rep1["loop_max_ms"] < 200, rep1["loop_max_ms"])
    st = node.state(); C.check("node still answers and heartbeats continue", st and st["paired"])
    boots = [t for t in node.since(0) if "[boot] LED frame node" in t]
    C.info("node boots seen during the whole run: %d (1 initial + 1 for the deliberate reboot test)" % len(boots))
    C.check("no unexpected node resets (watchdog)", len(boots) <= 1 + (1 if "persist" in only else 0) + 1, len(boots))
    clean()

# ================================================================== final
C.sec("final node counters")
rep = new_report()
for k, v in sorted(rep.items()): C.info("%s = %s" % (k, v))
ok = C.summary()
say(("TEST FINISHED: ALL CHECKS PASSED." if ok else "TEST FINISHED: SOME CHECKS FAILED. Claude will go through them."))
print("logs:", logdir)
node.close(); ctl.close()
sys.exit(0 if ok else 1)
