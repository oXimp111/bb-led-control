from playwright.sync_api import sync_playwright
import pathlib
u = pathlib.Path("manual.html").resolve().as_uri()
with sync_playwright() as p:
    b = p.chromium.launch(channel="chrome"); pg = b.new_page()
    pg.goto(u); pg.wait_for_timeout(800)
    pg.pdf(path="LED画框控制软件_使用手册.pdf", format="A4", print_background=True, prefer_css_page_size=True,
           display_header_footer=True, header_template="<span></span>",
           footer_template='<div style="font-size:8px;width:100%;text-align:center;color:#777"><span class="pageNumber"></span> / <span class="totalPages"></span></div>',
           margin={"top":"14mm","bottom":"15mm","left":"13mm","right":"13mm"})
    b.close()
