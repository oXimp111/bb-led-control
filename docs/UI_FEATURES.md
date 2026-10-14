# LED Frame Control UI: feature and shortcut reference

Source material for the user manual (`web/control.html`, one file, runs in Chrome or Edge).
Update this file whenever a feature or shortcut changes.

## Known behaviours (not bugs)
- **Frame boxes on the Control page lag behind fast programs.** The main interface shows the level each node
  *reports* in its status message (about every 500 ms; the simulator copies this). A blink faster than about
  1 s therefore looks slow or wrong there. The Simulator drawer reads the output live and is accurate.
  The real hardware behaves the same way. Decision: leave as is.
- Refreshing the page always returns to the Control tab (the Programs tab remembers Entire set / Single frame).
- Everything is saved automatically in the browser (localStorage). Use the same browser and profile each time.
  If saving is blocked, a message appears at start-up.
- Programs are limited to 24 steps everywhere (`MAX_STEPS` in the page = `LF_MAX_STEPS` in the firmware, protocol v3). Cue-list shows count the limit per section.

## Header (all pages)
| Control | What it does |
|---|---|
| Tabs: Control / Programs | Switch page |
| Edit / Run switch | Edit = assign programs to frames. Run = play things |
| Find nodes | Pair new wall nodes (on the node: hold BOOT 3 s and release; lights pulse). Not needed in the simulator |
| Connection: Simulator / USB (Web Serial) | Choose the virtual room or the real control ESP32 over USB |
| Simulator ▸ | Open or close the Simulator drawer (switches to Run mode). Slider = packet loss (retries up to 5 times; a frame shows `!` only if all tries fail) |
| Log | Show or hide the traffic log. Double-click the log to show or hide heartbeat lines |

## Control page
Frame numbers equal channel numbers, counted left to right while facing the wall.

**Edit mode**
- Click a frame: popup with the node, channel and board pin (see `docs/WIRING.md`), an **Identify this frame** button (pulses only that strip for 5 s) and a Program dropdown. It lists only single-frame programs (never shows).
  The choice saves at once and is sent to the node if one is paired.
- "Flip view 180°": turns the room drawing round.
- "Control corner" dropdown: marks which corner the control centre is in (for the drawing only).

**Run mode**
- Show dropdown (lists only shows) and **Play show**: uploads every frame's part and starts all nodes together.
  A show over 24 steps on any frame is refused (marked ⚠ in the list).
- **Play all frames**: sends and plays each frame's own assigned program.
- **Stop all**; **All off** (fades to 0 over 1 s).
- Click a frame: resends its program and plays it. That frame then leaves the running show.
  A frame with no program shows a message.
- A show marked ⏭ is a **cue list**: the Play show button is replaced by a panel with a big **GO ▶ <next section>** button (Space or Enter also work).
  Each press starts the next section. If the current section is still playing, its frames freeze at their present level and the next section takes over;
  if it has finished, the lights hold their last level until the press. Also: **Previous**, **Replay section**, **Go to** (asks first), **Reset** (asks first).
  The panel shows the sections, the playing section's time, and "next section ready ✓" once it has been sent to the nodes (GO is instant then).
  Clicking a frame takes that frame out of the cue list until Reset.

**Side panel**
- Nodes list: click a node for its popup (rename, change wall, Identify = pulse 5 s, Remove node).
- Frame badges: ● sending, ✓ received by node, ! failed (no ACK), ? no node on that wall.
  ▶ = playing. Glow = level reported by the node.

## Programs tab
Toggle at the top: **Entire set | Single frame**. The last choice is remembered.
The two libraries never mix. Shows live in "Entire set"; frame programs live in "Single frame".

### Single frame
Programs are unlimited in number, up to 24 steps each. Every edit saves immediately.
Editing a program does not resend anything. The next Run-mode click on a frame sends the current version.

- **Left list**: click to select. Shows "used by N frames" under each name.
  - New: adds "H100:1s".
  - Duplicate.
  - Rename: Enter confirms.
  - Delete: warns "used by N frames" and unassigns those frames.
- **Step table**: one row per step.
  - Type: Hold, Fade or Blink.
  - Level (%).
  - To level (%) (Fade and Blink only).
  - Blink period (Blink only).
  - Duration.
  - Row buttons: ↑ move up, ↓ move down, ✕ delete.
  - Time units: type `500ms`, `1.5s`, or a bare number (= ms). Values display as `10s`, `1.5s` or `250ms`.
    A red box means the value was refused.
  - Loop checkbox.
  - Step counter "n / 24". "+ Add step" is disabled at 24.
- **Program text box** (below the table), for example `loop H100:10s B0-100/1s:5s H0:0`.
  - `H<lvl>:<dur>` hold.
  - `F<a>-<b>:<dur>` fade.
  - `B<a>-<b>/<period>:<dur>` blink.
  - `loop` makes it repeat.
  - Valid text updates the table as you type. Invalid text shows the error in red and leaves the table alone.
    Leaving the box restores the last saved text.
- **Graph**: level against time, with a time axis. A loop shows two cycles, with a dashed line at the loop point.
- **Preview**: glowing swatch with Play (press again to restart) and Stop, with time and level readout.
  It uses the same Engine as the nodes. A program without loop stops at its end and holds the last level.
- No keyboard shortcuts on this page. Leaving the page or switching the toggle stops the preview.

### Entire set (show editor)
A show is a whole-room timeline with one row per frame (W1-1 … W4-5), grouped by wall.
Every change saves automatically. Maximum 24 steps per frame (gaps count as steps). Rows over the limit turn red,
and Run mode will not play that show.

- **Left list**: New, Duplicate, Rename, Delete (keeps at least one show), Export (one file), Import (file with one or more shows),
  Export all.
- **Top bar**: show name, Loop checkbox (with Loop on, "length" in ms appears), total length, warning text,
  Play/Pause, Stop, time readout.
- **Tools bar**:
  - Undo / Redo (100 steps).
  - Select all / Clear.
  - "+ Clip on selected rows": opens the clip popup.
  - Stagger: offset in ms, then Apply.
  - Copy, Paste, Duplicate.
  - Zoom −, + and Fit.
  - Legend: Hold blue, Fade green, Blink orange.
- **Right**: room preview (live glow of every frame at the playhead) and a help card.

**Mouse**
- Drag on an empty lane: new Hold clip.
- Drag a clip: move it. Drag a clip's edges: resize.
- Click a clip: select it. Click the selected clip again: opens the clip popup (effect, levels, period ms, start s, length s, Delete).
- Shift or Ctrl + click clips: multi-select.
- Click a row label to select the row. Shift = range, Ctrl = toggle. Click a wall name for the whole wall. Click ▸ to collapse a wall.
- Click or drag the ruler: move the playhead.
- Ctrl + mouse wheel: zoom.
- Snaps to 100 ms. Hold **Alt** for free (1 ms) placement.
- Overlapping clips are refused (shown red).

**Stagger (chase)**: select rows (or clips) that have a clip at the playhead. The first keeps its time and each next frame
starts the offset later.

**Keyboard** (only on the Entire set view, not while typing in a box or with a popup open)
| Keys | Action |
|---|---|
| Space | Play / pause preview |
| Delete or Backspace | Delete selected clips |
| Ctrl+C / Ctrl+V | Copy / paste clips (paste at the playhead) |
| Ctrl+D | Duplicate right after the selected clip(s). Repeat to build a sequence. With one clip selected, the selected rows get the copy too |
| Ctrl+Z | Undo |
| Ctrl+Y or Ctrl+Shift+Z | Redo |
| Ctrl+A | Select all rows |
| Esc | Clear selection |
| Space or Enter (Run mode, cue-list panel open) | GO: play the next section (ignored while typing or with a popup open) |
| Enter | Confirm in the rename dialog |

(Cmd works in place of Ctrl on Mac.)

### Cue lists in the show editor
- Tick **Cue list** next to the show name (Loop is switched off). Click the ruler to place the playhead, then **+ Cue at playhead**.
  Orange flags mark where each section starts; click a flag to rename it, change its time or delete it (the first flag only has a name).
- In a cue-list show a gap between clips holds the level instead of going dark. To switch a frame off, place a 0 % clip.
- A frame with no clip inside a section is left alone: it keeps its level.
- The 24-step limit applies per section, not to the whole show. The Play button in the editor plays the whole timeline as a preview (the nodes cannot play it in one go).
- Console tests: `selfTest()` and, with the simulator selected, `await cueSelfTest()` (about 12 s).

## Data and testing
- Programs: `{id, name, text}`. Shows: `{id, name, loop, length_ms, tracks…}`. Frame assignments store the program id.
- Old saves with index-based assignments are converted on load. Empty preset slots are dropped.
- Self-test: open the browser console (F12) and run `selfTest()`. It must end with `SELFTEST PASS`.
- Cue-list shows are normal shows plus `stepped: true`, `cues: [{t, name}]` and optional `first` (name of section 1). The cue position (`lf-cue`) is kept in localStorage.
- Cue-list rules in Run mode are tested by `await cueSelfTest()` (simulator) and, on real hardware, by `python tools/fulltest.py` (group T5). Results: docs/TEST_RESULTS.md, raw logs: `test_logs/`.
- Node-side program text format (`H F B K`) and serial commands: see `CLAUDE.md`.
- Which firmware goes on which board for production: `CLAUDE.md`, "FLASHING FOR PRODUCTION". Pin-to-frame table: `docs/WIRING.md`.
- User manual (Chinese, highlighted updates): `docs/manual/manual.html` and the PDF next to it.
