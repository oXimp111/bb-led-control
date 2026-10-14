"""(No keyboard needed anywhere: the person only presses buttons on the boards.)
Tiny shared channel between the test scripts and the instruction window (tools/ft_console.py).
say(text) shows text; ask(text) shows text and blocks until the person presses Enter in the window."""
import json, os, time, tempfile
DIR = os.path.join(tempfile.gettempdir(), "lf_fulltest_ui")
os.makedirs(DIR, exist_ok=True)
STATUS, ACK = os.path.join(DIR, "status.json"), os.path.join(DIR, "ack.txt")
_seq = int(time.time())

def say(text, wait=False, beep=False):
    global _seq
    _seq += 1
    tmp = STATUS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"seq": _seq, "text": text, "wait": wait, "beep": beep}, f)
    os.replace(tmp, STATUS)
    print("[ui]", text.replace("\n", " | "), flush=True)
    return _seq

def ask(text, timeout=900):
    seq = say(text, wait=True, beep=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if int(open(ACK).read().strip() or 0) >= seq:
                return True
        except (OSError, ValueError):
            pass
        time.sleep(0.2)
    return False
