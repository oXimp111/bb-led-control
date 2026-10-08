# Generates manual screenshots from web/control.html (Playwright + installed Chrome).
from playwright.sync_api import sync_playwright
import pathlib
url = pathlib.Path("../../web/control.html").resolve().as_uri()

MARK_JS = """(items)=>{
 document.querySelectorAll('.mk').forEach(e=>e.remove());
 for(const [sel,n,pos,idx] of items){
  const els=document.querySelectorAll(sel); const el=els[idx||0]; if(!el){console.log('MISSING '+sel);continue;}
  const r=el.getBoundingClientRect(); const d=document.createElement('div'); d.className='mk'; d.textContent=n;
  let x=r.left, y=r.top;
  if(pos==='tr'){x=r.right} else if(pos==='c'){x=r.left+r.width/2;y=r.top+r.height/2} else if(pos==='bl'){y=r.bottom} else if(pos==='l'){x=r.left;y=r.top+r.height/2}
  else if(pos==='r'){x=r.right;y=r.top+r.height/2} else if(pos==='b'){x=r.left+r.width/2;y=r.bottom}
  d.style.cssText=`position:fixed;z-index:99999;left:${x-17}px;top:${y-17}px;width:22px;height:22px;border-radius:50%;background:#e5322d;color:#fff;font:700 13px/22px Arial;text-align:center;border:2px solid #fff;box-shadow:0 1px 4px #000a;pointer-events:none`;
  document.body.appendChild(d);
 }}"""
CLEAR_JS = "document.querySelectorAll('.mk').forEach(e=>e.remove())"

with sync_playwright() as p:
    b = p.chromium.launch(channel="chrome")
    pg = b.new_page(viewport={"width": 1500, "height": 900}, device_scale_factor=1.5)
    pg.on("console", lambda m: print("console:", m.text) if ('MISSING' in m.text or 'rror' in m.text) else None)
    pg.goto(url); pg.wait_for_timeout(800)

    def shot(name, marks=None, clip=None, wait=250):
        pg.wait_for_timeout(wait)
        pg.evaluate("document.querySelectorAll('.toast').forEach(t=>t.style.display='none')")
        if marks: pg.evaluate(MARK_JS, marks)
        kw = {"clip": clip} if clip else {}
        pg.screenshot(path=f"img/{name}.png", **kw)
        pg.evaluate(CLEAR_JS)

    def tab(t): pg.click(f'[data-tab={t}]'); pg.wait_for_timeout(300)
    def mode(m): pg.click(f'[data-mode={m}]'); pg.wait_for_timeout(300)

    # ---- assign programs to frames (so pictures show names) ----
    assign = {"W1": "Breathe", "W2": "Demo", "W3": "Slow fade in", "W4": "Fade out"}
    counts = {"W1": 8, "W2": 4, "W3": 7, "W4": 5}
    for w, name in assign.items():
        for ch in range(1, counts[w] + 1):
            pg.click(f'.frame[data-id="{w}-{ch}"]'); pg.wait_for_timeout(120)
            val = pg.evaluate("(n)=>[...document.querySelectorAll('#fProg option')].find(o=>o.textContent.startsWith(n+' —')).value", name)
            pg.select_option('#fProg', val); pg.wait_for_timeout(150)
            pg.click('#fClose'); pg.wait_for_timeout(80)

    shot("01_header", [('[data-tab=control]', 1, 'tl'), ('#modeSeg', 2, 'tl'), ('#btnSim', 3, 'tl')],
         clip={"x": 0, "y": 0, "width": 1500, "height": 56})
    shot("02_control_edit", [('#editBar .modeHint', 1, 'tl'), ('#btnRot', 2, 'tl'), ('.frame[data-id="W1-3"]', 3, 'tl')])
    pg.click('.frame[data-id="W2-2"]'); pg.wait_for_timeout(300)
    shot("03_frame_modal", [('#fProg', 1, 'tl'), ('#fMsg', 2, 'tl'), ('#fClose', 3, 'tl')])
    pg.click('#fClose')

    mode('run')
    pg.select_option('#showSel', index=0)
    shot("06_control_run", [('#showSel', 1, 'tl'), ('#btnPlayShow', 2, 'tl'), ('#btnPlayAll', 3, 'tl'), ('#btnStopAll', 4, 'tl'), ('#btnAllOff', 5, 'tl'), ('.frame[data-id="W1-3"]', 6, 'tl')])
    pg.click('#btnPlayShow'); pg.wait_for_timeout(2600)
    pg.click('#btnSim'); pg.wait_for_timeout(500)
    shot("07_simulator", [('#btnSim', 1, 'tl'), ('#sim .roomwrap', 2, 'tl')], wait=600)
    pg.click('#btnStopAll'); pg.wait_for_timeout(300)
    pg.click('#btnSim')
    mode('edit')

    tab('programs')
    shot("08_show_editor", [('#progSeg', 1, 'tl'), ('#shList', 2, 'tl'), ('.edLeft .shBtns', 3, 'tl'), ('#edName', 4, 'tl'), ('#edLoop', 5, 'tl'), ('#btnPlayPrev', 6, 'tl'), ('#edTime', 7, 'tl'),
        ('#btnUndo', 8, 'tl'), ('#btnSelAll', 9, 'tl'), ('#btnAddClip', 10, 'tl'), ('#stgMs', 11, 'tl'), ('#btnCopy', 12, 'tl'), ('#zOut', 13, 'tl'), ('#tlScroll .ruler', 14, 'tl'), ('[data-wall="W1"]', 15, 'tl'), ('[data-row="W1-1"]', 16, 'tl'), ('.clip', 17, 'tl'), ('#edSvg', 18, 'tl')])
    pg.click('.clip[data-id="W1-3"][data-i="1"]'); pg.wait_for_timeout(200)
    pg.click('.clip[data-id="W1-3"][data-i="1"]'); pg.wait_for_timeout(300)
    shot("10_clip_edit", [('#cFx', 1, 'tl'), ('#cA', 2, 'tl'), ('#cT', 3, 'tl'), ('#cD', 4, 'tl'), ('#cDel', 5, 'tl')])
    pg.click('#cOk'); pg.wait_for_timeout(200)
    pg.click('[data-row="W1-2"]'); pg.click('#btnAddClip'); pg.wait_for_timeout(300)
    pg.select_option('#cFx', 'B')
    shot("11_clip_new", [('#cFx', 1, 'tl'), ('#cA', 2, 'tl'), ('#cB', 3, 'tl'), ('#cP', 4, 'tl'), ('#cOk', 5, 'tl'), ('#cCancel', 6, 'tl')])
    pg.click('#cCancel'); pg.wait_for_timeout(200)
    pg.click('#btnSelNone')

    pg.click('[data-v=sp]'); pg.wait_for_timeout(300)
    pg.click('#spList > *:nth-child(4)'); pg.wait_for_timeout(300)
    pg.click('#spPlay'); pg.wait_for_timeout(1800)
    shot("14_single_frame", [('#spList', 1, 'tl'), ('#spView .shBtns', 2, 'tl'), ('#spName', 3, 'tl'), ('#spLoop', 4, 'tl'), ('#spCount', 5, 'tl'), ('#spAdd', 6, 'tl'),
        ('.spTbl th:nth-child(2)', 7, 'tl'), ('.spTbl th:nth-child(3)', 8, 'tl'), ('.spTbl th:nth-child(4)', 9, 'tl'), ('.spTbl th:nth-child(5)', 10, 'tl'), ('.spTbl th:nth-child(6)', 11, 'tl'), ('#spRows tr:nth-child(1) td:last-child', 12, 'tl'),
        ('#spText', 13, 'tl'), ('#spGraph', 14, 'tl'), ('#spSwatch', 15, 'tl'), ('#spPlay', 16, 'tl')])
    pg.click('#spStop')
    b.close()
print("done")
