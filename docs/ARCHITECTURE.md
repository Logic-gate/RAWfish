# Architecture: engine, controller, interface (AI generated)

**Status: done (0.47.9, October 2026): the seven steps are in.** It
describes where RAWfish-indRAW is going, so that any interface -- RAWfish-indRAW's or
RAWfish's -- gets the same camera behaviour by talking to one process.

## Why

Today the camera's *behaviour* is split between the engine (C, Android
side) and the interface (its QML and its C++ viewfinder item): the exposure
modes, the spot measure, the focus sequences, the zoom/loupe geometry and
part of the measurements live in the interface. That made iteration easy, but:
- another interface (RAWfish's) would have to re-implement all of it, and
  would drift from RAWfish-indRAW's;
- the exposure loop goes engine -> pipe -> interface (QML) -> pipe -> engine
  for every measurement, which costs latency;
- the logic that had the most bugs (exposure, focus, loupe) can only be
  tried on the phone.

## The three layers

Terms used below: **engine** = `librawfish-ir.so`; **controller** = the new C
layer; **interface** = the app, made of its **QML** (screens and, today,
the camera logic) and a **C++ item** (`indrawpreview.cpp`: receives the
frames, draws them, and today measures the viewfinder and computes zebra
and peaking).

| Layer | World | One job |
|---|---|---|
| **Engine** -- `librawfish-ir.so` | Android (bionic), built with the NDK | commands to the sensor and returns from it: Camera2 requests, frames, metadata, files, and the **raw measurements** made on the buffers only it holds |
| **Controller** -- new C code in the `rawfish-ir-cli` process | Linux (glibc), built with the Sailfish SDK | the **camera's behaviour**: exposure modes, focus sequences, geometry, decisions; testable on a PC with simulated measurements |
| **Interface** -- the app (RAWfish-indRAW's, or RAWfish's): its QML, and a C++ item that receives and draws the frames | Qt | showing images and states, gestures, menus, visual aids |

### Rules between the layers

1. **Calls go downwards only.** The interface talks to the controller (the
   process's stdin/stdout, as today). The controller calls the engine's C
   API. The engine never calls the controller.
2. **Engine events go up through a pipe, not a callback.** Camera2 calls the
   engine back on Android threads; glibc code must not run there (the reason
   the engine is a bionic library in the first place). The engine writes its
   events (frame result, measurement, file written) to an internal pipe; the
   controller reads it on its own thread. A plain `write()` is safe between
   the two worlds.
3. **The engine holds no policy.** It applies what it is told and reports
   what happened. No "if priority mode then ...", no thresholds of
   behaviour; device facts (crop type, white level, focus calibration
   table) are fine.
4. **The interface holds no camera logic.** It sends gestures and choices in
   high-level terms and draws the states it receives. Display-only work
   stays there: zebra and peaking (computed on the image it already has),
   the circle, the level, the menus.
5. **One frame protocol.** SF2P (viewfinder frames) and SF2M (events) stay
   the transport to the interface; SF2M gains the controller's states.

## Where each function goes

| Function | Today | Goes to |
|---|---|---|
| Camera2 session, requests, 3A modes, regions, crop, flash, AE lock, focus hold | engine | engine (unchanged) |
| Focus calibration table (distance <-> lens value) | engine + CLI files | engine (a device fact) |
| RAW meter: mean, clipping, histogram, spot box | engine | engine (unchanged) |
| Viewfinder measure: mean, histogram, spot box | engine (`vfmeter`, since 0.47.1) | engine |
| Exposure modes: automatic, ISO / speed priority, manual; the loop; the spot measure in automatic; the target calibration; the manual +/- offset | controller (`ctl_exposure.c`, since 0.47.3) | controller |
| Focus sequences: once -> hold on lock; manual tap -> adopt the distance; manual steps in diopters | controller (`ctl_focus.c`, since 0.47.5) | controller |
| Zoom / loupe geometry (zoom, frame and display windows; centre-only HALs); tap coordinates | controller (`ctl_view.c`, since 0.47.7) | controller |
| Stack, burst, bracket, flash decisions; held-pair rule (photos take the held pair) | controller (`ctl_shoot.c`, since 0.47.8); flash rule in the engine | controller decides, engine executes |
| Stack accumulation, DNG, Exif, previews | engine | engine (unchanged) |
| Override file check, device profiles | CLI | controller (it opens the session) |
| Zebra, peaking (interface, C++ item); level, grid, histogram drawing, wheels, menus, gallery (interface, QML) | interface | interface (unchanged) |

## The protocol that follows (interface <-> controller)

Derived from the split, not negotiated; listed here so an interface knows
what to send and what it gets. Commands are in the interface's terms; the
controller turns them into engine calls.

**Commands (interface -> controller)**, one line each:

- `exposure auto` · `exposure iso N` (ISO priority) · `exposure speed T`
  (speed priority, Σ if T is over the ceiling) · `exposure manual T N` ·
  `ev STEPS`
- `focus once` · `focus continuous` · `focus distance D` · `focus step +1|-1`
- `ref X Y` (a tap: the reference point, in display-window coordinates) ·
  `ref clear`
- `zoom F` · `loupe F` · `loupe pan DX DY` (in display-window units) ·
  `loupe close`
- `shoot` with the current settings (type, quality, processing, bracket,
  burst, stack, timer handled by the interface) · `abort`
- settings, as today: `wb`, `scene`, `flash`, `preview` (viewfinder
  processing), `afwait`, `calib`

**Events (controller -> interface)**, SF2M lines:

- `state exposure mode=… iso=… exp_ns=… held=0|1 offset=…` -- what the wheels
  and the +/- number show
- `state focus mode=… lens_m=… af=searching|sharp|failed held=0|1`
- `state ref x=… y=… light=0|1` -- where to draw the circle (display coords)
- `state view crop=x,y,w,h` -- the display magnification the interface
  applies to the frames (the part the HAL cannot crop)
- `hist source=raw|vf r=… g=… b=… clip_lo=… clip_hi=…`
- unchanged: `caps`, `captured`, `written`, `progress`, `stack_done`

## Testing the controller

The controller has no Android and no Qt in it: a host test feeds it
simulated engine events (measurements at given exposures, AF states) and
checks its decisions -- the exposure pair it converges to, the number of
corrections of a spot measure, the crop for a pan on a centre-only HAL, the
pair a photo is taken with. These are the cases that produced most of the
bugs so far.

## Migration, piece by piece

Each step leaves RAWfish-indRAW working, with its own interface as the first client
(nothing should change for the user):

1. **Event pipe** engine -> controller, and the controller skeleton in the
   `rawfish-ir-cli` process (passing today's commands through). *Done*:
   `sailfish/controller.c` (started before the stream; `indraw_set_event_fd`
   gives the engine the pipe's write end, `indraw_emit` is the way back to the
   stream, in order with the frames); frame-bound events (`fcrop`) stay with
   their frame; the host test `sailfish/tests/controller_test.c` runs in
   `check-host.sh`. Without the controller (an older library), the engine
   streams its events itself, as before.
2. **Viewfinder measure** into the engine; the interface's C++ item stops
   measuring. *Done* (0.47.1): the engine measures the RGB frame it is about
   to send (mean, clipping, histogram; the tap's spot mapped through the
   frame's real crop, densely sampled) and sends one `vfmeter` event at most
   every 100 ms; the item only parses it. (On the RGB frame rather than the
   YUV one: the conversion exists anyway, and the measure is then identical
   to the one the item made.) The app's setMeterSpot is gone: one spot, the
   engine's, for both meters.
3. **Exposure** into the controller (modes, loop, spot, target, offset,
   held-pair rule); `ExposureLogic.qml` becomes a view. *3a done* (0.47.2):
   `sailfish/ctl_exposure.c` is the port of ExposureLogic.qml as pure logic
   (commands and state through callbacks, time as a parameter), with the
   host test `sailfish/tests/exposure_test.c`: a simulated scene and a HAL
   that applies a pair two frames late; seven rules checked (calibration,
   priority convergence without hunting, manual offset, the AE button's
   sends, bounded spot then hold, stacked preview transform, pause). Not
   wired yet at 3a. *3b done* (0.47.3): the controller reads `caps`, `ae`,
   `meter` and `vfmeter` on its reader thread, takes the interface's `xp ...`
   commands on stdin, issues the engine's `exposure` / `ev` / `meter` lines
   itself (one recursive lock for every engine caller), and reports
   `xpstate`; `ExposureLogic.qml` is a view (ENGINE.md "Controller
   commands"). Long engine commands (a photo, a burst, a stack) run on the
   CLI thread with the lock released and the controller told
   (`ctl_engine_busy`): its events keep flowing and its own engine commands
   are deferred, then re-applied (0.47.4).
4. **Focus sequences** into the controller. *Done* (0.47.5):
   `sailfish/ctl_focus.c` (modes, the tap's search with the stale-lock guard
   and the 3 s limit, hold, the manual distance and its steps, the late lock,
   the couplings with the exposure: paused during the search, the spot
   after), its host test `sailfish/tests/focus_test.c`, the `fc ...` commands
   and the `fcstate` event; the page converts the tap to frame coordinates
   (step 5) and draws the circle the controller reports.
5. **Geometry** (zoom, loupe, taps) into the controller. *Done* (0.47.7):
   `sailfish/ctl_view.c` (zoom / frame / display windows, the HAL part of
   the loupe on freeform and centre-only HALs, the gesture rule, the 80 ms
   loupe throttle, taps mapped to the viewfinder frame and handed to the
   focus behaviour), its host test `view_test.c`, the `vw ...` commands and
   the `vwstate` event (the item applies the view window itself). The page
   keeps only the view rotation and the drawing.
6. **Shooting decisions** (stack, burst, bracket, flash) into the controller.
   *Done* (0.47.8): `sailfish/ctl_shoot.c` turns a shutter request (`sh`)
   into the engine's capture lines from the exposure and focus states (the
   held-pair rule, the manual distance, a stack for a Σ speed, a burst or
   three shots for a bracket, the processing modes for JPEG); the CLI runs
   them as long commands; host test `shoot_test.c`. (The flash's own rule
   -- no automatic flash on a held pair -- was already the engine's.)
7. Clean-up. *Done* (0.47.9): the item no longer exposes the live AE / AF
   states and lens position (the controller's business); the page's header
   says what it is; `check-host.sh` runs the four module tests (exposure,
   focus, view, shoot) and the pass-through test on every check.

What the interface still owns, by design: the view rotation and the
drawing; gestures and menus; the self-timer; the stack overlay (from the
engine's progress events); zebra, peaking, level, grid; the gallery; the
settings files. What it will never see again: a pair to send, a focus
state to interpret, a crop to compute.

RAWfish's interface can plug in after step 3 for exposure, and fully after
step 6.
