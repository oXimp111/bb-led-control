"""RELAY DUPLICATE TEST.  Hardware: control board (env `control`) + node board (env `node_fulltest`, 460800 baud) on USB; the relays
are only powered (they need no USB). Run it twice, once with the relays OFF (baseline) and once with them ON:
  python tools/relay_test.py --label off      then      python tools/relay_test.py --label on
It answers: how often does one command reach the node more than once, how far apart do the copies arrive, is a command ever
applied twice (a copy arriving after a LATER command was applied), does anything storm, and do the commands still get through.
  --ctl COM3 --node COM4 --n 150 --burst 300 --idle 30
Logs: test_logs/relaytest_<label>_<time>/ (node.log, control.log, result.txt)."""
import argparse, collections, json, os, random, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ftlib import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--ctl", default="COM3"); ap.add_argument("--node", default="COM4")
ap.add_argument("--label", default="run")
ap.add_argument("--n", type=int, default=150, help="SET commands, each followed by a node state check")
ap.add_argument("--burst", type=int, default=300, help="SET commands back to back, no checks in between")
ap.add_argument("--uploads", type=int, default=5, help="rounds of 8 full-size programs")
ap.add_argument("--idle", type=int, default=30, help="seconds of idle traffic measurement")
args = ap.parse_args()

logdir = os.path.join(ROOT, "test_logs", "relaytest_%s_%s" % (args.label, time.strftime("%Y%m%d_%H%M%S")))
os.makedirs(logdir, exist_ok=True)
out = open(os.path.join(logdir, "result.txt"), "w", encoding="utf-8")
def P(s=""):
    print(s, flush=True); out.write(s + "\n"); out.flush()

node = Node(args.node, 460800, "node", logdir)
ctl = Control(args.ctl, 115200, "control", logdir)
time.sleep(0.5)

# ------------------------------------------------------------------ identify and pair
m = node.mark(); node.send("status")
r = node.wait(lambda t: t.startswith("node ") and " fw " in t, m, 3)
if not r: P("ABORT: node does not answer on %s at 460800 baud. Is it flashed with node_fulltest?" % args.node); sys.exit(1)
NM = r[1].split()[1]
m = ctl.mark(); ctl.send(json.dumps({"cmd": "hello", "id": 1}))
h = ctl.wait(lambda t: t.startswith("{") and '"evt":"hello"' in t.replace(" ", ""), m, 3)
if not h: P("ABORT: control does not answer on %s." % args.ctl); sys.exit(1)
hello = json.loads(h[1]); CTLMAC = hello["mac"]
P("node %s   control %s   label '%s'" % (NM, CTLMAC, args.label))

def node_paired_to_control():
    m = node.mark(); node.send("status")
    r = node.wait(lambda t: t.startswith("paired to"), m, 3)
    return bool(r) and CTLMAC in r[1]

if not node_paired_to_control() or NM not in hello.get("peers", []):
    P("pairing ...")
    node.send("pair"); ctl.req({"cmd": "scan", "on": True})
    ok = ctl.wait(lambda t: NM in t and '"evt":"found"' in t.replace(" ", ""), 0, 10)
    r = ctl.req({"cmd": "pair", "node": NM}, 12); ctl.req({"cmd": "scan", "on": False})
    if not r.get("ok"): P("ABORT: pairing failed: %s" % r); sys.exit(1)
if not ctl.wait(lambda t: NM in t and '"online":true' in t.replace(" ", ""), 0, 10): P("ABORT: node not online"); sys.exit(1)
ctl.req({"cmd": "stop", "node": NM, "ch": "all"}); ctl.req({"cmd": "set", "node": NM, "ch": "all", "level": 0, "fade_ms": 0})
ctl.req({"cmd": "assign", "node": NM, "ch": "all", "slot": None})
time.sleep(0.5)

# ------------------------------------------------------------------ phase 1: idle traffic (storm check, heartbeat copies)
P("\n== 1. idle traffic, %d s ==" % args.idle)
r0 = node.report(); mc = ctl.mark(); t0 = time.time()
time.sleep(args.idle)
r1 = node.report(); dt = time.time() - t0
nstat = sum(1 for e in ctl.events(mc) if e.get("evt") == "status" and e.get("node") == NM)
d = lambda k: r1.get(k, 0) - r0.get(k, 0)
hb = d("heartbeats_sent")
P("node received %d packets (%.1f / s), node sent %d heartbeats, control reported %d status updates = %.2f per heartbeat"
  % (d("rx_total"), d("rx_total") / dt, hb, nstat, nstat / max(1, hb)))
P("node queue drops %d, queue max %d" % (d("queue_drops"), r1.get("queue_max", 0)))
idle_rx = d("rx_total") / dt

# ------------------------------------------------------------------ phase 2: SET with a state check after each one
P("\n== 2. %d SET commands, node state checked 70 ms after each result ==" % args.n)
model = {c: 0.0 for c in range(1, 9)}
fails = 0; rtts = []; bad = []
random.seed(7)
for i in range(args.n):
    ch = random.randint(1, 8)
    lv = random.choice([x for x in range(1, 100) if abs(x - model[ch]) > 5])
    r = ctl.req({"cmd": "set", "node": NM, "ch": ch, "level": lv, "fade_ms": 0})
    rtts.append(r["rtt"])
    if not r.get("ok"): fails += 1; continue
    model[ch] = float(lv)
    time.sleep(0.07)
    st = node.state()
    if not st: bad.append((i, "no state")); continue
    for c in range(1, 9):
        if abs(st["ch"][c]["level"] - model[c]) > 1.5: bad.append((i, "ch%d is %.1f, last command said %.0f" % (c, st["ch"][c]["level"], model[c])))
    for c in range(1, 9): model[c] = st["ch"][c]["level"] if abs(st["ch"][c]["level"] - model[c]) > 1.5 else model[c]
rtts.sort()
P("commands failed: %d of %d    result time ms: median %.0f, p95 %.0f, max %.0f" % (fails, args.n, rtts[len(rtts) // 2] * 1000, rtts[int(len(rtts) * .95)] * 1000, rtts[-1] * 1000))
P("state mismatches (node level different from what the last acknowledged command set): %d" % len(bad))
for b in bad[:10]: P("   #%d %s" % b)

# ------------------------------------------------------------------ phase 3: back-to-back burst (the harshest case for late copies)
P("\n== 3. burst: %d SET commands back to back ==" % args.burst)
fails3 = 0; rt3 = []
for i in range(args.burst):
    r = ctl.req({"cmd": "set", "node": NM, "ch": random.randint(1, 8), "level": random.randint(0, 100), "fade_ms": 0}); rt3.append(r["rtt"])
    if not r.get("ok"): fails3 += 1
rt3.sort()
P("commands failed: %d of %d    result time ms: median %.0f, p95 %.0f, max %.0f" % (fails3, args.burst, rt3[len(rt3) // 2] * 1000, rt3[int(len(rt3) * .95)] * 1000, rt3[-1] * 1000))

# ------------------------------------------------------------------ phase 4: full-size program uploads (231 byte packets)
P("\n== 4. program uploads: %d rounds of 8 x 24-step programs ==" % args.uploads)
fails4 = 0; crc_bad = 0
for rd in range(args.uploads):
    for slot in range(1, 9):
        text = "loop " + " ".join("F%d-%d:%dms" % ((i * 7 + slot * rd) % 100, (i * 13 + slot) % 100, 200 + 10 * i) for i in range(24))
        r = ctl.req({"cmd": "prog", "node": NM, "slot": slot, "text": text}, 10)
        if not r.get("ok"): fails4 += 1
    st = node.state()
    for slot in range(1, 9):
        text = "loop " + " ".join("F%d-%d:%dms" % ((i * 7 + slot * rd) % 100, (i * 13 + slot) % 100, 200 + 10 * i) for i in range(24))
        want = prog_crc(parse_prog(text))
        if not st or st["prg"].get(slot, (None,))[0] != want: crc_bad += 1
P("uploads failed: %d of %d    program checksum wrong on the node after a round: %d of %d" % (fails4, args.uploads * 8, crc_bad, args.uploads * 8))

# ------------------------------------------------------------------ analysis of everything the node received
P("\n== 5. what the node received (from node.log) ==")
rx = node.rx_list(0)
cmds = [x for x in rx if x["src"] == CTLMAC and x["hdr_ok"] and x["type"] in (T["SET"], T["PROG"], T["ASSIGN"], T["PLAY"], T["STOP"], T["IDENTIFY"], T["GO"])]
copies = collections.defaultdict(list)
for x in cmds: copies[(x["type"], x["seq"])].append(x)
hist = collections.Counter(len(v) for v in copies.values())
P("commands seen: %d distinct, %d packets" % (len(copies), len(cmds)))
P("copies per command: " + ", ".join("%d copies: %d" % (k, hist[k]) for k in sorted(hist)))
spread = [(v[-1]["us"] - v[0]["us"]) / 1000.0 for v in copies.values() if len(v) > 1]
if spread:
    spread.sort()
    P("time between first and last copy of the same command (ms): median %.2f, p95 %.2f, max %.2f" % (spread[len(spread) // 2], spread[int(len(spread) * .95)], spread[-1]))
# replay the node's duplicate rule: only a repeat of the LAST applied sequence number is recognised
last = None; applied = collections.Counter(); reacks = 0
for x in cmds:
    if x["ver"] != PROTO_VER: continue
    if last is not None and x["seq"] == last: reacks += 1; continue
    applied[(x["type"], x["seq"])] += 1; last = x["seq"]
twice = {k: v for k, v in applied.items() if v > 1}
P("repeats the node recognised and ignored (re-ACKed): %d" % reacks)
P("COMMANDS APPLIED MORE THAN ONCE (a late copy after another command): %d" % len(twice))
for k, v in list(twice.items())[:10]: P("   %s seq %d applied %d times" % (TNAME.get(k[0], k[0]), k[1], v))
own = [x for x in rx if x["src"] == NM]
P("packets the node received that carry its OWN address (relay copies of its heartbeats/ACKs): %d" % len(own))
rep = node.report()
P("node totals: rx %d, queue drops %d, queue max %d, bad header %d" % (rep.get("rx_total", 0), rep.get("queue_drops", 0), rep.get("queue_max", 0), rep.get("rx_badhdr", 0)))

P("\n== SUMMARY (%s) ==" % args.label)
P("idle receive rate %.1f packets/s | failed commands %d+%d of %d | state mismatches %d | uploads failed %d, wrong checksum %d | applied twice %d | max copy spread %s ms"
  % (idle_rx, fails, fails3, args.n + args.burst, len(bad), fails4, crc_bad, len(twice), "%.2f" % spread[-1] if spread else "-"))
P("logs: " + logdir)
node.close(); ctl.close()
