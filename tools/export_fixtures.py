"""Compile shows with the REAL page code (web/control.html) in headless Chrome and save the result as tools/fixtures/compiled.json.
The full node test then sends exactly what the page would send. usage: python tools/export_fixtures.py"""
import json, os, re, subprocess, sys, tempfile, html
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = next(p for p in [r"C:\Program Files\Google\Chrome\Application\chrome.exe", r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"] if os.path.exists(p))
src = open(os.path.join(ROOT, "web", "control.html"), encoding="utf-8").read()
JS = r"""
window.addEventListener('load', () => setTimeout(() => {
  const out = {};
  try {
    const W1 = frameIds('W1');
    const pick = m => Object.fromEntries(W1.map(id => [id, m[id]]));
    out.shows = DEFAULT_SHOWS.map(raw => { const n = normalizeShow(raw); return { name: n.name, loop: n.loop, length_ms: showLen(n), programs: pick(compileShow(n)) }; });
    const one = (id, ...clips) => ({ frames: [id], clips });
    const cue = normalizeShow({ name: 'cue fulltest', stepped: true, first: 'Opening', cues: [{ t: 3000, name: 'Two' }, { t: 6000, name: 'Three' }, { t: 9000, name: 'Four' }], tracks: [
      one('W1-1', { t: 0, fx: 'F', a: 0, b: 100, dur: 3000 }, { t: 3000, fx: 'H', a: 100, dur: 3000 }, { t: 6000, fx: 'F', a: 100, b: 0, dur: 3000 }),
      one('W1-2', { t: 0, fx: 'H', a: 50, dur: 2000 }, { t: 4000, fx: 'H', a: 80, dur: 1000 }),
      one('W1-3', { t: 0, fx: 'B', a: 0, b: 100, per: 1000, dur: 9000 }),
      one('W1-4', { t: 0, fx: 'B', a: 10, b: 90, per: 700, dur: 9000 }),
      one('W1-5', { t: 6000, fx: 'F', a: 0, b: 100, dur: 2000 }),
      one('W1-6', { t: 9000, fx: 'H', a: 60, dur: 3000 }),
      one('W1-7', { t: 0, fx: 'H', a: 30, dur: 1000 }, { t: 9500, fx: 'H', a: 90, dur: 1000 })] });
    const cs = compileSections(cue);
    out.cue = { name: cue.name, errors: cs.errors, sections: cs.sections.map(s => ({ name: s.name, start: s.start, len: s.len, programs: pick(s.programs) })) };
  } catch (e) { out.error = String(e.stack); }
  const pre = document.createElement('pre'); pre.id = 'res'; pre.textContent = JSON.stringify(out); document.body.prepend(pre);
}, 300));
"""
i = src.rindex("</body>")
page = src[:i] + "<script>" + JS + "</script></body>" + src[i + 7:]
tmp = os.path.join(tempfile.gettempdir(), "lf_fixture_page.html")
open(tmp, "w", encoding="utf-8").write(page)
dom = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--virtual-time-budget=10000", "--dump-dom", "file:///" + tmp.replace("\\", "/")],
                     capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120).stdout
m = re.search(r'<pre id="res">(.*?)</pre>', dom, re.S)
if not m: sys.exit("no result from the page")
data = json.loads(html.unescape(m.group(1)))
if "error" in data: sys.exit("page error: " + data["error"])
os.makedirs(os.path.join(ROOT, "tools", "fixtures"), exist_ok=True)
json.dump(data, open(os.path.join(ROOT, "tools", "fixtures", "compiled.json"), "w", encoding="utf-8"), indent=1)
print("saved tools/fixtures/compiled.json:", len(data["shows"]), "shows,", len(data["cue"]["sections"]), "cue sections, errors", data["cue"]["errors"])
for s in data["cue"]["sections"]: print(" ", s["name"], s["len"], {k: v for k, v in s["programs"].items() if v})
