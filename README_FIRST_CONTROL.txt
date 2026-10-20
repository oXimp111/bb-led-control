CONTROL BOARD UPDATE 2026-10-19 (control fw 4)
==============================================
Only the CONTROL board is flashed. The 4 wall boards and the relay are NOT touched.
New in this version: one show for the whole evening (Q1-Q19), choosing a show always starts at Q1, choosing a show never changes the lights.

STEP 1. Unzip to C:\
   Right-click LEDframes_2026-10-19.zip > Extract All... > change the path to   C:\   > Extract.
   You now have the folder   C:\LEDframes_2026-10-19

STEP 2. Open PowerShell in that folder
   Open the folder C:\LEDframes_2026-10-19 in File Explorer, click the address bar at the top, type   powershell   and press Enter.

STEP 3. Flash the control board
   Plug in ONLY the control board by USB. Its COM number: Device Manager > Ports (COM & LPT) > "USB-SERIAL CH340 (COMx)".
   Type this line (replace COM5 with your number) and press Enter:

      pio run -e control -t upload --upload-port COM5

   When it shows "Connecting......", HOLD the BOOT button on the board and keep holding until it shows "Writing", then let go.
   It must end with [SUCCESS]. Then tap the EN button once.
   ("Failed to connect" = BOOT was not held in time: run the line again and hold BOOT from the start.)

STEP 4. Check the firmware version
   Type this line (same COM number) and press Enter, then tap the EN button on the board:

      pio device monitor --port COM5 --baud 115200

   You must see:   [boot] LED frame control center fw 4, protocol 3
   and:            open http://192.168.4.1/
   If it says fw 3, the old firmware is still on the board: repeat step 3.
   Press Ctrl+C to leave.

AFTER FLASHING: load the show
   Connect the laptop to Wi-Fi LED-Control (password ledframes), open http://192.168.4.1/
   Programs > Import > choose  C:\LEDframes_2026-10-19\怨种闺蜜_相框灯_v15_one_show.json
   Run mode > show dropdown: choose "怨种闺蜜 v14 Q1-Q19" (the old A / B / C shows can stay as a backup).
   Before the show press Reset once in the Cue list panel, so the first GO plays Q1.
