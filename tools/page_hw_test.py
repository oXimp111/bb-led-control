"""Run the REAL web/control.html in headless Chrome against the REAL control board, with one node paired as wall W2.
Reproduces: "cue list: GO says nothing played". Hardware: control board (env `control`) + node board (env `node_fulltest` or `node`).
  python tools/page_hw_test.py --ctl COM3 [--node COM4]
The page's USB transport is replaced by a bridge to the COM port (Web Serial cannot be automated); everything else is the page's own code.
--node is optional: with it the node's real engine state is printed after every GO (needs the node_fulltest build, 460800 baud).
Output: test_logs/pagehw_<time>/ (page_log.txt = the page's Log pane, control.log, node.log) and a summary on screen."""
import argparse, asyncio, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ftlib import Control, Node
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--ctl", default="COM3"); ap.add_argument("--node", default=None)
ap.add_argument("--section-ms", type=int, default=3000)
args = ap.parse_args()
logdir = os.path.join(ROOT, "test_logs", "pagehw_" + time.strftime("%Y%m%d_%H%M%S")); os.makedirs(logdir, exist_ok=True)
ctl = Control(args.ctl, 115200, "control", logdir)
node = None
if args.node:
    try: node = Node(args.node, 460800, "node", logdir)
    except Exception as e: print("node port not opened:", e)
CHROME = next(p for p in [r"C:\Program Files\Google\Chrome\Application\chrome.exe", r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"] if os.path.exists(p))

def say(s): print(s.encode("ascii", "replace").decode(), flush=True)

def node_levels():
    if not node: return ""
    st = node.state()
    if not st: return "node state: no answer"
    return "node: " + " ".join("ch%d=%s %.0f%% slot%d" % (n, c["mode"], c["level"], c["slot"]) for n, c in sorted(st["ch"].items()) if n <= 4)

async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CHROME)
        pg = await b.new_page()
        pg.on("pageerror", lambda e: say("PAGE ERROR " + str(e)))
        await pg.expose_function("pyWrite", lambda line: ctl.send(line))
        await pg.goto("file:///" + os.path.join(ROOT, "web", "control.html").replace("\\", "/"))
        await pg.wait_for_timeout(800)
        await pg.evaluate("""() => { window.Bridge = { name: 'serial', async open() { return true; }, write(o) { pyWrite(JSON.stringify(o)); }, close() {} };
          transport.close(); transport = Bridge; live.nodes = {}; request({ cmd: 'hello' }); }""")
        fed = [ctl.mark()]
        async def feed():
            while True:
                with ctl.lock: new = ctl.lines[fed[0]:]; fed[0] = len(ctl.lines)
                for _, t in new:
                    if t.startswith("{"): await pg.evaluate("l => { try { onEvent(JSON.parse(l)); } catch (e) { log('er', 'bad line ' + l); } }", t)
                await asyncio.sleep(0.02)
        fe = asyncio.create_task(feed())
        say("waiting for a paired node to report in (up to 15 s)...")
        macs = []
        for _ in range(30):
            macs = await pg.evaluate("Object.keys(live.nodes)")
            if macs: break
            await asyncio.sleep(0.5)
        if not macs: say("FAIL: the control board reports no node. Is the node powered and paired? (hello answer below)"); say(await pg.evaluate("document.getElementById('log').innerText")); fe.cancel(); await b.close(); return
        mac = macs[0]; say("node found: " + mac + "  info " + json.dumps(await pg.evaluate("m => live.nodes[m]", mac)))
        await pg.evaluate("m => assignNode(m, 'W2 test node', 'W2')", mac)
        sec = args.section_ms
        await pg.evaluate("""sec => {
          const one = (id, ...clips) => ({ frames: [id], clips });
          const sh = normalizeShow({ name: 'W2 cue test', stepped: true, first: 'One', cues: [{ t: sec, name: 'Two' }, { t: 2 * sec, name: 'Three' }], tracks: [
            one('W1-1', { t: 0, fx: 'H', a: 70, dur: sec }),                                             // section 1: only a wall without a node
            one('W2-1', { t: sec, fx: 'H', a: 80, dur: sec }, { t: 2 * sec, fx: 'F', a: 80, b: 0, dur: sec }),
            one('W2-2', { t: sec, fx: 'H', a: 40, dur: sec }),
            one('W2-3', { t: 2 * sec, fx: 'H', a: 100, dur: sec })] });
          S.shows.push(sh); refreshShowSel(); byId('showSel').value = sh.id; setMode('run'); cue = null; cueSync(); }""", sec)
        await asyncio.sleep(5)    # the page uploads sections 1 and 2 by itself
        say("after prefetch: loaded=%s  status: %s" % (await pg.evaluate("JSON.stringify(cue.loaded)"), await pg.evaluate("byId('cueStatus').textContent")))
        for k in (1, 2, 3):
            await pg.evaluate("document.getElementById('toast').textContent = ''")
            await pg.click("#cueGo")
            await asyncio.sleep(1.2)
            say("--- GO %d: k=%s | status: %s | toast: %s" % (k, await pg.evaluate("cue.k"), await pg.evaluate("byId('cueStatus').textContent"), await pg.evaluate("document.getElementById('toast').textContent")))
            lv = node_levels()
            if lv: say("    " + lv)
            await asyncio.sleep(sec / 1000 - 0.6)
        await asyncio.sleep(1)
        txt = await pg.evaluate("document.getElementById('log').innerText")
        open(os.path.join(logdir, "page_log.txt"), "w", encoding="utf-8").write(txt)
        say("--- page Log (tx/rx lines, heartbeats hidden), last 40:")
        for l in txt.splitlines()[-40:]: say("   " + l[:200])
        fe.cancel(); await b.close()
    say("logs in " + logdir)

try: asyncio.run(main())
finally:
    ctl.close()
    if node: node.close()
