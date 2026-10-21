"""Flash a bench board over its normal USB serial port while its firmware runs: no BOOT button, nobody at the bench.
Needs firmware that contains lib/serialota (node, node_fulltest, control, control_wstest, relay). A board that still runs
older firmware must be flashed ONCE the normal way (BOOT held): pio run -e <env> -t upload --upload-port COMx

    python tools/serial_flash.py node COM3            build env "node" and flash it to the board on COM3
    python tools/serial_flash.py control_wstest COM4 --no-build
    python tools/serial_flash.py --status COM3        what runs on the board (env name, app slot, state, SHA-256)
    python tools/serial_flash.py --reset COM3         reset the board (EN line)

Safety (see lib/serialota/serial_ota.h): the image goes into the other app slot; the running firmware stays until the
whole image arrived with the right MD5. The new firmware starts "pending" and is only kept after it booted, received a
test transfer and reported exactly this file's SHA-256. Otherwise this tool resets the board and the bootloader goes
back to the previous firmware by itself (a crashing image goes back without help). Exit code 0 = new firmware running
and confirmed, 1 = flash failed but the board runs its previous firmware, 2 = board needs attention (see message).
Windows note: the board is never reset by opening the port (DTR and RTS stay off)."""
import argparse, configparser, hashlib, os, subprocess, sys, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAUDS = (115200, 460800)
T = {"ping": 2.5, "ready": 8.0, "chunk": 2.5, "end": 20.0, "boot": 40.0, "after_reset": 6.0, "idle_abort": 11.0}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def env_baud(env):
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(os.path.join(ROOT, "platformio.ini"), encoding="utf-8")
    sec = "env:" + env
    if not cfg.has_section(sec):
        sys.exit(f"no [env:{env}] in platformio.ini")
    for _ in range(10):
        if cfg.has_option(sec, "monitor_speed"):
            return int(cfg.get(sec, "monitor_speed"))
        if not cfg.has_option(sec, "extends"):
            break
        sec = cfg.get(sec, "extends").strip()
    return int(cfg.get("env", "monitor_speed", fallback="115200"))


def open_serial(port, baud):            # replaced by tools/serial_flash_test.py
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.05
    s.dtr = False                       # both lines off BEFORE open: on these boards DTR = BOOT, RTS = EN (reset)
    s.rts = False
    s.open()
    return s


class Board:
    def __init__(self, port, baud):
        self.port, self.baud, self.ser, self.buf = port, None, None, b""
        self.reopen(baud)

    def reopen(self, baud):
        if self.ser is not None and self.baud == baud:
            return
        self.close()
        self.ser, self.baud, self.buf = open_serial(self.port, baud), baud, b""

    def close(self):
        if self.ser is not None:
            self.ser.close()
            self.ser = None

    def send(self, data):
        self.ser.write(data)

    def cmd(self, text):
        self.send(b"\n" + text.encode() + b"\n")

    def lines(self, timeout):
        """yields '@ota ...' lines as they arrive until timeout"""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                ln = raw.decode("utf-8", "replace").strip()
                i = ln.find("@ota ")
                if i >= 0:
                    yield ln[i:]

    def expect(self, prefixes, timeout):
        for ln in self.lines(timeout):
            if any(ln.startswith(p) for p in prefixes):
                return ln
        return None

    def reset(self):                    # EN pulse (RTS), BOOT (DTR) stays off: normal boot
        self.ser.rts = True
        time.sleep(0.2)
        self.ser.rts = False


def parse_pong(ln):
    p = ln.split()
    info = {"name": p[2] if len(p) > 2 else "?"}
    for kv in p[3:]:
        if "=" in kv:
            k, v = kv.split("=", 1)
            info[k] = v
    return info


def ping(b, bauds):
    """-> pong dict (also sets b's baud), or None if nothing with the updater answers"""
    for baud in bauds:
        b.reopen(baud)
        tries = 2
        while tries:
            tries -= 1
            b.ser.reset_input_buffer()
            b.buf = b""
            b.cmd("@ota ping")
            ln = b.expect(["@ota pong", "@ota resend", "@ota ok"], T["ping"])
            if ln and ln.startswith("@ota pong"):
                return parse_pong(ln)
            if ln:                      # still inside an earlier, interrupted transfer: it gives up after 10 s
                log(f"the board is still in an interrupted transfer; waiting {T['idle_abort']:.0f} s for it to give up")
                time.sleep(T["idle_abort"])
                tries = 2
    return None


def probe_order(*first):
    out = []
    for x in list(first) + list(BAUDS):
        if x and x not in out:
            out.append(x)
    return out


def transfer(b, data, test=False):
    """send data with the chunk protocol -> (ok, why)"""
    b.buf = b""
    b.cmd(f"@ota test {len(data)}" if test else f"@ota begin {len(data)} {hashlib.md5(data).hexdigest()}")
    ln = b.expect(["@ota ready", "@ota error"], T["ready"])
    if not ln or ln.startswith("@ota error"):
        return False, ln or "no answer to begin"
    chunk = int(ln.split()[2])
    done, resends, silent, last_pct = 0, 0, 0, -1
    while done < len(data):
        part = data[done:done + chunk]
        frame = done.to_bytes(4, "little") + part
        b.send(frame + zlib.crc32(frame).to_bytes(4, "little"))
        last = done + len(part) >= len(data)
        ln = b.expect(["@ota ok", "@ota resend", "@ota error", "@ota done", "@ota test ok"], T["end"] if last else T["chunk"])
        if ln is None:                  # chunk or answer lost: sending it again is safe (the board answers a repeat again)
            silent += 1
            if silent > 3:
                return False, f"no answer after {done + len(part)} bytes" + (" (last chunk: the board may have restarted)" if last else "")
            continue
        silent = 0
        if ln.startswith("@ota error"):
            return False, ln
        if ln.startswith("@ota resend"):
            resends += 1
            if resends > 30:
                return False, "too many resends"
            continue
        done += len(part)
        if ln.startswith("@ota ok") and int(ln.split()[2]) != done:
            return False, f"board is at {ln.split()[2]}, expected {done}"
        pct = done * 100 // len(data)
        if not test and pct // 10 != last_pct // 10:
            log(f"  {pct:3d} %  ({done // 1024} KB of {len(data) // 1024} KB, {resends} resent)")
            last_pct = pct
    if test:
        return (ln.startswith("@ota test ok"), ln)
    return (ln.startswith("@ota done"), ln)


def back_to_previous(b, old, why):
    """new firmware not acceptable: reset (pending -> bootloader goes back), check what runs now"""
    log(f"NEW FIRMWARE NOT KEPT: {why}")
    log("resetting the board: the bootloader goes back to the previous firmware")
    b.reset()
    time.sleep(T["after_reset"])
    now = ping(b, probe_order(old.get("baud")))
    if now and now.get("sha") == old.get("sha"):
        log(f"OK: previous firmware is running again ({now['name']}, {now.get('part')}, {now.get('state')})")
        return 1
    if now:
        log(f"board answers but runs {now}; expected the previous firmware {old.get('sha', '?')[:16]}...")
        return 2
    log("BOARD DOES NOT ANSWER after the reset. It needs a BOOT flash: pio run -e <env> -t upload --upload-port " + b.port)
    return 2


def flash(port, env, image, force=False, any_role=False):
    data = open(image, "rb").read()
    if len(data) < 64 or data[0] != 0xE9:
        sys.exit(f"{image} is not an ESP32 app image")
    if b"@ota pong %s" not in data:   # lib/serialota missing: the Arduino core would confirm it by itself = board stranded
        log(f"REFUSED: {image} has no USB self-update (lib/serialota). Sent this way the board would keep it and could")
        log("never be flashed remotely again. Flash such firmware with BOOT held (pio run -e <env> -t upload).")
        return 2
    want_sha = data[-32:].hex() if data[23] == 1 else hashlib.sha256(data).hexdigest()
    new_baud = env_baud(env)
    b = Board(port, new_baud)
    try:
        old = ping(b, probe_order(new_baud))
        if not old:
            log(f"no answer on {port} from a firmware with USB self-update (tried {', '.join(map(str, BAUDS))} baud).")
            log(f"Flash it ONCE with BOOT held: pio run -e {env} -t upload --upload-port {port}   (or the board is busy/off)")
            return 2
        old["baud"] = b.baud
        log(f"{port}: running {old['name']} ({old.get('part')}, {old.get('state')}, sha {old.get('sha', '?')[:16]}...) at {b.baud} baud")
        if old["name"].split("_")[0] != env.split("_")[0] and not any_role:
            log(f"REFUSED: the board runs '{old['name']}', you asked for '{env}' (another kind of board). Use --any-role if that is really wanted.")
            return 2
        if old.get("state") == "pending":   # flashing on top of it could leave no bootable app: go back to the confirmed one first
            log("the running firmware was never confirmed (an earlier flash was interrupted): resetting, the board goes back to its confirmed firmware")
            b.reset()
            time.sleep(T["after_reset"])
            old = ping(b, probe_order(old["baud"], new_baud))
            if not old or old.get("state") == "pending":
                log(f"after the reset the board runs {old}; not flashing. Check it with --status.")
                return 2
            old["baud"] = b.baud
            log(f"{port}: now running {old['name']} ({old.get('part')}, {old.get('state')}, sha {old.get('sha', '?')[:16]}...)")
        if old.get("sha") == want_sha and old.get("state") != "pending" and not force:
            log("the board already runs exactly this image; nothing to do (--force to flash anyway)")
            return 0
        t0 = time.time()
        log(f"sending {image} ({len(data) // 1024} KB) ...")
        for attempt in (1, 2):
            ok, why = transfer(b, data)
            if ok or "last chunk" in (why or ""):   # last answer lost: the board may already run the new image (checked below)
                break
            log(f"transfer failed: {why}")
            if attempt == 2:
                log("giving up; the board still runs its previous firmware (nothing was switched)")
                return 1
            wait = 1.0 if (why or "").startswith("@ota error") else T["idle_abort"]   # error = the board already dropped it
            log(f"waiting {wait:.0f} s for the board to drop the partial image, then trying again")
            time.sleep(wait)
            if not ping(b, probe_order(old["baud"])):
                log("board does not answer any more")
                return 2
        log(f"image accepted (MD5 ok) in {time.time() - t0:.0f} s; board restarts into it" if ok else "last answer lost; checking whether the board restarted into the new image")
        # ---- the new firmware must boot, show up as pending, pass a test transfer and report the right SHA
        b.reopen(new_baud)
        b.buf = b""
        name = None
        for ln in b.lines(T["boot"]):
            p = ln.split()
            if p[1] == "pending" or (p[1] == "boot" and "state=pending" in ln):
                name = p[2]
                break
            if p[1] == "boot":         # booted but not pending = the bootloader already went back (the new image crashed)
                log(f"NEW FIRMWARE CRASHED: the board came back with {ln[10:]}")
                return 1
        if not name:
            return back_to_previous(b, old, f"no '@ota pending' from the new firmware within {T['boot']:.0f} s at {new_baud} baud")
        if name != env:
            return back_to_previous(b, old, f"the new firmware calls itself '{name}', expected '{env}'")
        time.sleep(0.5)
        ok, why = transfer(b, os.urandom(8192), test=True)
        if not ok:
            return back_to_previous(b, old, f"test transfer to the new firmware failed: {why}")
        now = ping(b, [new_baud])
        if not now or now.get("sha") != want_sha:
            return back_to_previous(b, old, f"SHA-256 does not match the file: {now}")
        b.cmd("@ota confirm")
        if not b.expect(["@ota confirmed"], T["ping"] * 2):
            return back_to_previous(b, old, "no answer to confirm")
        now = ping(b, [new_baud])
        if not now or now.get("state") != "valid":
            return back_to_previous(b, old, f"state after confirm: {now}")
        log(f"DONE: {port} runs {env} ({now['part']}, confirmed, sha {want_sha[:16]}...) at {new_baud} baud, {time.time() - t0:.0f} s in total")
        return 0
    finally:
        b.close()


def status(port):
    b = Board(port, BAUDS[0])
    try:
        info = ping(b, probe_order())
        if not info:
            print(f"{port}: no firmware with USB self-update answers (tried {', '.join(map(str, BAUDS))} baud)")
            return 2
        match = ""
        for env in ("node", "node_fulltest", "control", "control_wstest", "relay"):
            f = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
            if os.path.exists(f) and open(f, "rb").read()[-32:].hex() == info.get("sha"):
                match = f"  = current build of '{env}'"
        print(f"{port} @ {b.baud}: {info['name']}  slot {info.get('part')}  state {info.get('state')}  sha {info.get('sha', '?')[:16]}...{match}")
        return 0
    finally:
        b.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("env", nargs="?")
    ap.add_argument("port", nargs="?")
    ap.add_argument("--status", metavar="COM")
    ap.add_argument("--reset", metavar="COM")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--bin", help="image to send instead of .pio/build/<env>/firmware.bin")
    ap.add_argument("--force", action="store_true", help="flash even if the board already runs this image")
    ap.add_argument("--any-role", action="store_true", help="allow e.g. control firmware on a board that runs node firmware")
    a = ap.parse_args(argv)
    if a.status:
        return status(a.status)
    if a.reset:
        b = Board(a.reset, BAUDS[0])
        b.reset()
        b.close()
        print(f"{a.reset}: reset")
        return 0
    if not (a.env and a.port):
        ap.error("give <env> <COMx>, or --status COMx")
    image = a.bin or os.path.join(ROOT, ".pio", "build", a.env, "firmware.bin")
    if not a.no_build and not a.bin:
        log(f"building {a.env} ...")
        r = subprocess.run(["pio", "run", "-e", a.env], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-3000:], r.stderr[-2000:])
            return 2
    return flash(a.port, a.env, image, a.force, a.any_role)


if __name__ == "__main__":
    sys.exit(main())
