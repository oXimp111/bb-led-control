from playwright.sync_api import sync_playwright
import pathlib
url = pathlib.Path("../../web/control.html").resolve().as_uri()
with sync_playwright() as p:
    b = p.chromium.launch(channel="chrome")
    pg = b.new_page(viewport={"width":1500,"height":900})
    pg.on("console", lambda m: print("console:", m.text))
    pg.goto(url); pg.wait_for_timeout(800)
    pg.screenshot(path="img/_x1.png")
    pg.click('[data-tab=programs]'); pg.wait_for_timeout(500)
    pg.screenshot(path="img/_x2.png")
    pg.click('[data-v=sp]'); pg.wait_for_timeout(500)
    pg.screenshot(path="img/_x3.png")
    b.close()
