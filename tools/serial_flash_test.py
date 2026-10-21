"""PC test of the remote flashing path: the REAL lib/serialota code (compiled for the PC with a simulated board, test/serialota_host)
driven by the REAL tools/serial_flash.py through a fake serial port. No hardware needed.
    pip install ziglang pyserial      (ziglang = a C++ compiler; nothing else to install)
    python tools/serial_flash_test.py
The simulated board has two app slots and the ESP-IDF rollback rules, restarts on ESP.restart()/crash, and loses data
when the PC uses the wrong baud rate. Checked: normal flash, baud change, crash / no-updater / hanging / wrong image
(must go back to the previous firmware), refused role change, corrupted and lost bytes, cable pulled mid-transfer,
interrupted before confirm, board without the updater."""
import hashlib, os, shutil, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import serial_flash as sf  # noqa: E402

WORK = tempfile.mkdtemp(prefix="serialota_")
EXE = os.path.join(WORK, "board.exe")


def build():
    src = [os.path.join(ROOT, "test", "serialota_host", "board.cpp"), os.path.join(ROOT, "lib", "serialota", "serial_ota.cpp")]
    r = subprocess.run([sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1", "-Wall", "-I" + os.path.join(ROOT, "test", "serialota_host"),
                        "-I" + os.path.join(ROOT, "lib", "serialota"), *src, "-o", EXE], capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stdout + r.stderr)


def image(name, baud=115200, behave="", size=700_000, seed=0):
    import random
    rnd = random.Random(hash((name, baud, behave, size, seed)) & 0xFFFFFFFF)
    head = bytearray(rnd.randbytes(24))
    head[0], head[23] = 0xE9, 1
    body = bytes(head) + f"NAME:{name};BAUD:{baud};".encode() + (f"BEHAVE:{behave};".encode() if behave else b"")
    body += (b"" if behave == "noota" else b"@ota pong %s part=%s") + rnd.randbytes(size)   # format string = lib/serialota is inside
    return body + hashlib.sha256(body).digest()


class SimBoard:
    """the board on one 'COM port': one process per boot; survives the PC opening/closing the port"""
    def __init__(self, d):
        self.d, self.lock, self.out, self.p, self.boots = d, threading.Lock(), b"", None, 0
        self.faults, self.big_writes = {}, 0
        self.start()

    def start(self):
        self.boots += 1
        if self.boots > 40:
            raise RuntimeError("board restarted too often")
        self.p = subprocess.Popen([EXE, self.d], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        threading.Thread(target=self._reader, args=(self.p,), daemon=True).start()

    def _reader(self, p):
        while True:
            try:
                b = os.read(p.stdout.fileno(), 65536)
            except OSError:
                b = b""
            if not b:
                break
            with self.lock:
                self.out += b
        code = p.wait()
        if p is self.p and code in (3, 5):     # ESP.restart() or panic: the chip boots again
            self.start()

    def reset(self):                           # EN pulse
        p = self.p
        self.p = None
        p.kill()
        p.wait()
        self.start()

    def baud(self):
        try:
            return int(open(os.path.join(self.d, "baud")).read().strip())
        except (OSError, ValueError):
            return 0

    def write(self, data):
        if len(data) > 100:                    # a firmware chunk
            self.big_writes += 1
            k = self.big_writes
            if self.faults.get("flip") == k:
                data = bytearray(data); data[10] ^= 0x55; data = bytes(data)
            if self.faults.get("drop") == k:
                data = data[: len(data) // 2]
            if self.faults.get("cut") and self.faults["cut"] <= k < self.faults["cut"] + self.faults.get("cutlen", 3):
                return                         # cable pulled for some chunks
        try:
            self.p.stdin.write(data)
            self.p.stdin.flush()
        except (OSError, AttributeError):
            pass                               # chip is rebooting: bytes lost

    def kill(self):
        if self.p:
            p, self.p = self.p, None
            p.kill()


class FakePort:
    def __init__(self, board, baud):
        self.b, self.baudrate, self._rts, self.dtr = board, baud, False, False

    def write(self, data):
        if self.baudrate == self.b.baud():
            self.b.write(data)

    def read(self, n):
        end = time.time() + 0.05
        while True:
            with self.b.lock:
                m = self.b.faults.get("mute")
                if m and self.b.big_writes == m and b"@ota ok" in self.b.out and b"\n" in self.b.out[self.b.out.find(b"@ota ok"):]:
                    i = self.b.out.find(b"@ota ok")            # the board's answer to chunk m is lost on the way
                    self.b.out = self.b.out[:i] + self.b.out[self.b.out.find(b"\n", i) + 1:]
                    self.b.faults["mute"] = None
                if self.b.out:
                    out, self.b.out = self.b.out[:n], self.b.out[n:]
                    return out if self.baudrate == self.b.baud() else b""
            if time.time() > end:
                return b""
            time.sleep(0.002)

    def reset_input_buffer(self):
        with self.b.lock:
            self.b.out = b""

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, v):
        if self._rts and not v:
            self.b.reset()
        self._rts = v

    def close(self):
        pass


BOARDS = {}
sf.open_serial = lambda port, baud: FakePort(BOARDS[port], baud)
sf.T.update(boot=6.0, after_reset=2.0)


def new_board(port, first_image):
    d = os.path.join(WORK, port)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    open(os.path.join(d, "app0.bin"), "wb").write(first_image)
    open(os.path.join(d, "state"), "w").write("boot app0\napp0 flashed\napp1 empty\n")
    if port in BOARDS:
        BOARDS[port].kill()
    BOARDS[port] = SimBoard(d)
    time.sleep(0.3)
    return BOARDS[port]


def put(name, data):
    p = os.path.join(WORK, name + ".bin")
    open(p, "wb").write(data)
    return p


results = []


def check(label, cond, detail=""):
    results.append(cond)
    print(("PASS " if cond else "FAIL ") + label + ("" if cond else f"   [{detail}]"), flush=True)


def running(port):
    b = sf.Board(port, 115200)
    info = sf.ping(b, sf.probe_order())
    b.close()
    return info or {}


def main():
    build()
    v1, v2, v3 = image("node", seed=1), image("node", seed=2), image("node", seed=3)
    sha = lambda img: img[-32:].hex()

    print("\n== normal flash, then the same image again, then a baud change and back")
    new_board("COM3", v1)
    rc = sf.flash("COM3", "node", put("v2", v2))
    r = running("COM3")
    check("node v2 flashed and confirmed", rc == 0 and r.get("sha") == sha(v2) and r.get("state") == "valid" and r.get("part") == "app1", (rc, r))
    rc = sf.flash("COM3", "node", put("v2", v2))
    check("same image again: nothing sent", rc == 0 and BOARDS["COM3"].boots == 2, (rc, BOARDS["COM3"].boots))
    ft = image("node_fulltest", baud=460800, seed=4)
    rc = sf.flash("COM3", "node_fulltest", put("ft", ft))
    r = running("COM3")
    check("node_fulltest (460800 baud) flashed over node (115200)", rc == 0 and r.get("sha") == sha(ft) and r.get("state") == "valid", (rc, r))
    rc = sf.flash("COM3", "node", put("v3", v3))
    r = running("COM3")
    check("back to node (115200) from node_fulltest (460800)", rc == 0 and r.get("sha") == sha(v3) and r.get("state") == "valid", (rc, r))

    print("\n== bad images must never stay on the board")
    boots = BOARDS["COM3"].boots
    rc = sf.flash("COM3", "node", put("noota", image("node", behave="noota", seed=5)))
    r = running("COM3")
    check("image without the updater (would confirm itself): refused, nothing sent", rc == 2 and r.get("sha") == sha(v3) and BOARDS["COM3"].boots == boots, (rc, r))
    for behave, why in (("crash", "crashes at boot"), ("hang", "hangs at boot")):
        rc = sf.flash("COM3", "node", put(behave, image("node", behave=behave, seed=5)))
        r = running("COM3")
        check(f"image that {why}: tool reports failure, previous firmware runs", rc == 1 and r.get("sha") == sha(v3) and r.get("state") == "valid", (rc, r))
    rc = sf.flash("COM3", "node", put("wrongname", image("relay", seed=6)), any_role=False)
    r = running("COM3")
    check("image of another env sent under the 'node' name: rolled back", rc == 1 and r.get("sha") == sha(v3), (rc, r))
    rc = sf.flash("COM3", "control", put("ctl", image("control", seed=7)))
    r = running("COM3")
    check("control firmware onto a node board: refused, nothing sent", rc == 2 and r.get("sha") == sha(v3), (rc, r))
    rc = sf.flash("COM3", "node", put("toobig", image("node", size=1_400_000, seed=8)))
    r = running("COM3")
    check("image larger than an app slot: refused by the board", rc == 1 and r.get("sha") == sha(v3), (rc, r))

    print("\n== damaged transfers")
    v4, v5, v6 = image("node", seed=9), image("node", seed=10), image("node", seed=11)
    BOARDS["COM3"].faults, BOARDS["COM3"].big_writes = {"flip": 5}, 0
    rc = sf.flash("COM3", "node", put("v4", v4))
    check("one corrupted chunk: resent, flash OK", rc == 0 and running("COM3").get("sha") == sha(v4), rc)
    BOARDS["COM3"].faults, BOARDS["COM3"].big_writes = {"drop": 7}, 0
    rc = sf.flash("COM3", "node", put("v5", v5))
    check("half a chunk lost: resent after 1 s, flash OK", rc == 0 and running("COM3").get("sha") == sha(v5), rc)
    BOARDS["COM3"].faults, BOARDS["COM3"].big_writes = {"cut": 50}, 0
    t = time.time()
    rc = sf.flash("COM3", "node", put("v6", v6))
    check(f"3 chunks in a row lost: sent again, flash OK ({time.time() - t:.0f} s)", rc == 0 and running("COM3").get("sha") == sha(v6), rc)
    v6b = image("node", seed=16)
    BOARDS["COM3"].faults, BOARDS["COM3"].big_writes = {"mute": 30}, 0
    rc = sf.flash("COM3", "node", put("v6b", v6b))
    check("board's answer to a chunk lost: chunk sent again, not written twice, flash OK", rc == 0 and running("COM3").get("sha") == sha(v6b), rc)
    BOARDS["COM3"].faults, BOARDS["COM3"].big_writes = {"cut": 20, "cutlen": 1000}, 0
    sf.T["chunk"] = 0.3                        # the tool gives up after 4 silent tries, mid-transfer
    b = sf.Board("COM3", 115200)
    ok, why = sf.transfer(b, image("node", seed=17))
    b.close()
    sf.T["chunk"] = 2.5
    BOARDS["COM3"].faults = {}
    r = running("COM3")
    check("tool gave up mid-transfer: next ping waits for the board to drop it, old firmware still runs", not ok and r.get("sha") == sha(v6b) and r.get("state") == "valid", (ok, why, r))
    v6 = v6b

    print("\n== interrupted before confirm")
    v7 = image("node", seed=12)
    b = sf.Board("COM3", 115200)
    ok, why = sf.transfer(b, v7)
    b.close()
    time.sleep(1.5)
    r = running("COM3")
    check("image arrived, not confirmed: board runs it as pending", ok and r.get("sha") == sha(v7) and r.get("state") == "pending", (ok, why, r))
    b = sf.Board("COM3", 115200)
    b.reset()
    b.close()
    time.sleep(1.5)
    r = running("COM3")
    check("...a reset goes back to the confirmed firmware", r.get("sha") == sha(v6) and r.get("state") == "valid", r)
    b = sf.Board("COM3", 115200)
    sf.transfer(b, v7)
    time.sleep(1.5)
    ok, why = sf.transfer(b, image("node", seed=18))
    b.close()
    check("board refuses to flash on top of its unconfirmed firmware", not ok and "pending_reset_first" in (why or ""), (ok, why))
    v8 = image("node", seed=13)
    rc = sf.flash("COM3", "node", put("v8", v8))
    r = running("COM3")
    check("tool resets an unconfirmed firmware away first, then flashes", rc == 0 and r.get("sha") == sha(v8) and r.get("state") == "valid", (rc, r))

    print("\n== the real firmware images")
    for env in ("node", "node_fulltest", "control", "control_wstest", "relay"):
        f = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
        if os.path.exists(f):
            d = open(f, "rb").read()
            check(f"{env}: image contains the updater, SHA-256 appended, fits an app slot", b"@ota pong %s" in d and d[23] == 1 and len(d) <= 0x140000, len(d))

    print("\n== board without the updater (old firmware)")
    new_board("COM4", image("control", behave="noota", seed=14))
    rc = sf.flash("COM4", "control", put("c1", image("control", seed=15)))
    check("tool says: flash once with BOOT held (exit 2), board untouched", rc == 2 and BOARDS["COM4"].boots == 1, rc)

    for bd in BOARDS.values():
        bd.kill()
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
