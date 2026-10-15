RELAY INSTALL (for the installer, and for her Claude)
=====================================================
Goal: one EXTRA ESP32 board (the "relay") is placed between the control board and the far wall to extend the radio range.
The 4 wall boards and the control board are NOT touched: do NOT reflash them. Only the relay board is flashed.

What you need: the spare ESP32 (same 30-pin board as the others), a USB cable, the laptop that already built this project before
(PlatformIO is installed and has already compiled "node" once, so no internet is needed), and later a USB charger or power bank for the relay.

1) Unzip this file into a NEW folder, for example C:\LEDframes_relay  (do not overwrite the old project folder).
2) Open PowerShell in that folder (File Explorer: click the address bar, type powershell, Enter).
3) Plug in ONLY the spare board by USB. Find its COM number: Device Manager > Ports (COM & LPT) > "USB-SERIAL CH340 (COMx)". Remember x.
4) Build (about 1 minute):          pio run -e relay
   It must end with [SUCCESS]. If it says it must download something and there is no internet, tell Sean.
5) Flash. The board needs the BOOT button:
      pio run -e relay -t upload --upload-port COMx        (replace COMx, e.g. COM5)
   When the screen shows "Connecting......" HOLD the BOOT button on the board and keep holding until it shows "Writing", then let go.
   When finished, tap the EN button once.
6) Check: pio device monitor --port COMx --baud 115200
   You must see:   [boot] LED frame RELAY fw 1
   After 5 seconds, and every 5 s, a "[relay] 5 s:" block appears. Leave the control board and the nodes powered: lines with CONTROL and node
   (each with an rssi number) should appear. Press Ctrl+C to leave the monitor.
   If you see "FULL CAPABILITY NODE TEST" or no "RELAY" text, the wrong firmware is on the board: repeat step 5.
7) Placing the relay: see docs\RELAY.md ("Where to put it"). Short version: halfway between the control board and the far wall, high up,
   away from metal and power supplies, powered by a USB charger/power bank. Send Sean a photo of the monitor text from step 6 at the chosen spot,
   and a screenshot of the control page showing the far node (ONLINE, and its RSSI).
8) The far wall's board may blink its status LED fast (as if disconnected) while it works: that is normal with the relay. The page is what counts.

Troubleshooting: "Failed to connect" while flashing = you were not holding BOOT early enough; run the command again and hold BOOT from the start.
