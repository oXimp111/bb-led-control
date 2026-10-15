# Show operator card: cue-list show over Wi-Fi

Print this. Keep it next to the laptop.

## Before doors open (about 5 minutes)
1. Power on everything: the control board, the 4 wall boards and the relay. Wait 10 seconds.
2. On the control computer, join Wi-Fi **LED-Control** (password **ledframes**). It will say "no internet": stay connected.
   - Joining this Wi-Fi disconnects the computer from any other Wi-Fi. If anything else on this computer needs its Wi-Fi during the show (remote access, a network control protocol, licensing), use a separate USB Wi-Fi adapter or another laptop just for LED-Control.
3. Open **http://192.168.4.1/** in Chrome or Edge.
4. Check the **Nodes** list: all 4 nodes show **online**. If one is offline, check its power, then the relay's power and cable. Do not continue until all 4 are online.
5. Switch to **Run** mode (top of the page), tab **Control**.
6. In the **Show** dropdown, pick the cue-list show. The big **GO** panel appears.
7. Wait until the panel says: **Ready. Nothing has played yet. · next section ready ✓**. The page is sending sections 1 and 2 to the nodes. This can take several seconds. Do not press GO before this.
8. Optional: in Edit mode, click a frame and press **Identify this frame** to check it lights up. Switch back to Run mode afterwards. (Selecting the show again re-sends nothing unless it changed.)

## During the show
- **GO:** press **Space**, **Enter** or the **GO** button once per cue. A second press within 0.3 s is ignored on purpose.
- While a section plays, the page sends the next one to the nodes in the background. Press GO when the panel says **next section ready ✓**.
- If you press GO before it says ready, the page finishes sending first and then starts the cue. On a weak wall this can take a few seconds.
- If a message says **"N node(s) did not confirm the cue"**: press **GO** again once. Then check the relay and the wall boards' power.
- Leave **5 to 10 seconds** between cues when you can, so the next section has time to arrive.

## Do not, during the show
- Do **not** click individual frames. They leave the cue list until Reset.
- Do **not** use Previous, Go to or Find nodes. They re-send programs.
- Do **not** reload the page. If you do, the next GO must re-send first and takes longer.
- Do **not** edit programs, upload, or unplug anything.

## If something goes wrong
| What you see | What to do |
|---|---|
| A wall's frames did not change after GO | Press GO once more. Check the relay is powered. |
| A node shows offline | The lights on that wall keep their last state. Check the node's power, then the relay's. |
| The page lost the control board ("link lost, reconnecting") | It reconnects by itself. Check the laptop is still on LED-Control. Lights are not affected. |
| Everything is frozen | The lights keep playing. Reconnect to Wi-Fi, reopen the page, and press GO for the next cue. The page remembers which section was playing. |

## After the show
- Press **Reset** (confirm). The next GO starts at section 1.
- Lights stay as they are. Use **All off** to turn them off.

## What stays safe if the radio fails
Each wall board plays its own program and never turns its lights off when the radio drops. Only new commands (GO, edits) need the radio.
