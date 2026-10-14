"""Instruction window for the full capability node test. Leave it open; follow what it says."""
import json, os, sys, time, msvcrt
sys.path.insert(0, os.path.dirname(__file__))
from ft_ui import STATUS, ACK
os.system("title LED test - instructions & mode con cols=100 lines=30 & color 0F")
last = 0
print("=" * 90 + "\n  LED FRAME TEST: instructions will appear here. Keep this window open.\n" + "=" * 90, flush=True)
while True:
    try:
        d = json.load(open(STATUS, encoding="utf-8"))
    except (OSError, ValueError):
        time.sleep(0.3); continue
    if d["seq"] != last:
        last = d["seq"]
        if d.get("beep"): print("\a", end="")
        print("\n" + ("*" * 90 if d.get("wait") else "-" * 90) + "\n" + d["text"] + "\n" + ("*" * 90 if d.get("wait") else ""), flush=True)
        if d.get("wait"):
            while msvcrt.kbhit(): msvcrt.getch()
            input("   >>> press ENTER here when done: ")
            open(ACK, "w").write(str(last))
            print("   thanks, working...", flush=True)
    time.sleep(0.3)
