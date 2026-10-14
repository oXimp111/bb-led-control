"""Flash one board with PlatformIO. No keyboard needed: the person only holds the board's BOOT button while this retries.
usage: ft_flash.py <env> <COMx> <label>"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from ft_ui import say
env, port, label = sys.argv[1:4]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
say("NEXT: flash the %s board (%s).\n\n  >>> PRESS AND HOLD the BOOT button on the %s board NOW and keep holding it. <<<\n\n"
    "I keep trying to connect for about 2 minutes. I will say RELEASE when it is writing." % (label, port, label), beep=True)
time.sleep(8)
for attempt in range(1, 8):
    p = subprocess.Popen(["pio", "run", "-e", env, "-t", "upload", "--upload-port", port], cwd=ROOT, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    released = False
    out = []
    for line in p.stdout:
        out.append(line)
        if not released and "Writing at" in line:
            released = True
            say("RELEASE the BOOT button now (%s board).\n\nWriting is under way. Do not unplug anything." % label, beep=True)
    p.wait()
    text = "".join(out)
    if p.returncode == 0:
        say("%s board flashed OK. Nothing more to press; I reset it myself." % label, beep=True)
        print(text[-300:])
        sys.exit(0)
    say("Still waiting for the %s board to connect (try %d/7).\n\n  >>> KEEP HOLDING the BOOT button on the %s board. <<<" % (label, attempt, label))
    time.sleep(1)
say("Could not flash the %s board. Check the USB cable, then tell Claude." % label, beep=True)
sys.exit(1)
