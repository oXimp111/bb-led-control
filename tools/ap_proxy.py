"""Lets a browser on a PC whose home network is also 192.168.4.x open the PRODUCTION control page (192.168.4.1).
localhost:8080 -> 192.168.4.1:80 (page), localhost:81 -> 192.168.4.1:81 (WebSocket), both sent out of the PC adapter
that is joined to LED-Control (--wlan). Open http://localhost:8080/ ; the page then talks to ws://localhost:81/.
Plain byte relay: when one side closes or fails, the other side is closed too; otherwise it never closes anything,
so a dead Wi-Fi link stays dead for the page exactly as it would without the proxy.
    python tools/ap_proxy.py --wlan Wi-Fi
"""
import argparse, socket, subprocess, threading

ap = argparse.ArgumentParser()
ap.add_argument("--wlan", required=True); ap.add_argument("--host", default="192.168.4.1")
a = ap.parse_args()

def wlan_ip():   # the adapter's address can change after a rejoin: look it up for every new connection
    o = subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"(Get-NetIPAddress -InterfaceAlias '{a.wlan}' -AddressFamily IPv4).IPAddress"], capture_output=True, text=True).stdout
    return o.strip().splitlines()[0] if o.strip() else None

def pipe(src, dst):
    try:
        while True:
            d = src.recv(65536)
            if not d: break
            dst.sendall(d)
    except OSError: pass
    for s in (src, dst):
        try: s.close()
        except OSError: pass

def serve(lport, rport):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("127.0.0.1", lport)); ls.listen(8)
    while True:
        c, _ = ls.accept()
        try:
            up = socket.socket(); up.settimeout(4); up.bind((wlan_ip(), 0)); up.connect((a.host, rport)); up.settimeout(None)
        except Exception as e:
            print(f"[proxy] :{lport} upstream failed: {e}", flush=True); c.close(); continue
        print(f"[proxy] :{lport} -> {a.host}:{rport} via {up.getsockname()[0]}", flush=True)
        threading.Thread(target=pipe, args=(c, up), daemon=True).start()
        threading.Thread(target=pipe, args=(up, c), daemon=True).start()

threading.Thread(target=serve, args=(8080, 80), daemon=True).start()
serve(81, 81)
