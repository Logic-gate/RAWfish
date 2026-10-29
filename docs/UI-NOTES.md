# (AI written, human thought)
# RAWfish-indRAW UI & behaviour specification

Reference for the Qt/Sailfish app and the engine's UI-facing behaviour. The
engine (rawfish-ir-cli; indraw in the sources) exposes every control atomically; the UI composes them.
Graphical/structural reference: RAWfish v1.3.1-alpha.9 -- we follow its layout
and components closely, but with the explicit differences in "vs RAWfish" below.

Build note: changes are delivered as patches, applied with `git apply` and
committed one by one; the git history of the repository is the authority.


## Core principles

- **Everything atomic.** Each setting is independent and explicit. No combined
  command except `easy` (quick auto JPEG). Grouped controls (scene, effect) are
  neutralised at open and, when chosen, grey out the atomic controls they drive.
- **Nothing hidden.** When the engine applies something it reports it; no silent
  values. Reports carry the actually-applied exposure/iso/focus with an
  image/metadata match check.
- **WYSIWYG viewfinder, strict.** The viewfinder shows what the photo will be.
  NO display gain that brightens/darkens after the fact (RAWfish's
  `renderExposure`), neither in the viewfinder nor in the file. More light = more
  ISO or longer exposure.
- **Gain.** The only brightness amplification is the ISO (any level, incl. past
  the analog ceiling: controlled, visible, deliberate). The engine never adds digital
  gain on top, and sends no separate gain command to the HAL. Stacking
  (accumulating real exposures) is our own thing, not a cosmetic gain.
- **Sensor-bias corrections are kept** (not gain): white balance (corrects the
  native green cast) and the tone curve -- applied to the JPEG, NEVER to the RAW,
  which stays linear and raw.
- **Code** is commented in English, concise.
- **i18n**: all UI text via `qsTr()`, never hard-coded; infrastructure ready for
  every language, English only for now. The engine stays in technical English
  JSON (not translated).
- **UI version badge** ("ui NN") shown in the deck so the running build is always
  identifiable.


## Orientation / viewfinder / photo

- Viewfinder is faithful to the capture framing (RAWfish's sometimes was not).
- The viewfinder FILLS the screen width, no crop, no black borders. (The sensor
  is 4:3 landscape; rotated upright it is 3:4, and the component's fit math
  accounts for the display rotation so it fills by width without cropping.)
- **UI rotation** works in every orientation: the image is rotated by
  (sensor orientation - screen rotation) and a tap is mapped back through the
  inverse of the same transform (and the loupe), so the AE/AF circle lands
  where the user pressed. Portrait: deck under the viewfinder; landscape:
  viewfinder on the left at full height, deck on the right, the four notch
  buttons in a row at its top.
- Photos show upright in the gallery (JPEG EXIF orientation from the sensor
  mounting angle).


## Screen layout (portrait), top to bottom

1. **Notch band** (front camera) in black, with four invisible TEXT buttons
   (no icons) around the cutout -- two left, two right. Currently: zebra, peak
   (left); level, AE (right). (The grid moved to the settings menu.) AE = reset full auto-exposure. The settings menu is
   NOT here: it opens by a swipe-down on the viewfinder.

2. **4:3 viewfinder.**
   - Unpinch = a display loupe to judge FOCUS; it does not zoom the photo
     (this is focus_crop, distinct from zoom_crop). A red frame marks
     focus-check mode.
   - Two fingers spreading/closing (more than 15 %) zoom the loupe about the
     centre. While the loupe is on, the gallery and menu swipes are off, so
     ONE finger pans the window (two moving together pan too); a touch
     without movement is still a tap (reference point).
   - Tap the metering/focus circle (see Focus).

3. **Control deck** (black background), modelled on RAWfish: a tiny caption
   header (CAM / EV / SPEED / ISO) over a four-column grid, proportions
   20% / 41% / 19% / rest.
   - **Col 1 (~1/5)**: focal (= camera 0/1/2), image type (raw | jpg beauty |
     jpg yuv), focus (distance if manual | auto | cont | macro). Invisible cycle
     buttons (tiny caption + large value, RAWfish CycleValueButton sizes).
   - **Col 2 (~2/5)**: EV wheel overlaid on the histogram band (same height);
     zoom + / - buttons (single tap steps, double tap max/reset); the Jolla
     system shutter icon.
   - **Cols 3 & 4 (thin)**: shutter-speed and ISO wheels.

Deck text is WHITE, except the headers (CAM/EV/SPEED/ISO) and the notch buttons,
which keep their ambience colour (notch buttons use secondaryHighlightColor).


## Control behaviour

- **Exposure.** Auto by default. Touch the ISO or shutter wheel -> that wheel
  goes manual (priority); touch both -> fully manual.
- **EV wheel.** In auto/semi-auto it shifts exposure; in manual it is replaced by
  a NUMBER showing the stops +/- from what auto would choose.
- **Wheels.** Move on their own in auto, freeze into manual when touched. Values
  come from the sensor's capabilities, like RAWfish. In auto, the real
  intermediate value may be shown in place of the nearest notch, the wheel
  turning if it crosses to another notch. Active (selected) digit WHITE, inactive
  grey.
- **Shutter wheel.** 1/2 s and shorter are single exposures (1/3-stop
  notches). Longer positions are STACKED exposures, marked by a Greek sigma
  drawn 30 % smaller than the digits: photo-equivalent Σ1s, 2s, 4s, 8s, 15s,
  30s, 1m, 2m, 4m, 8m. Under each, on a second line set close to it, the
  real time the shot will take ("(2:48)"): sub-frames of the ceiling length
  plus the HAL's latency between them, measured on the device at every stack
  and remembered (default 200 ms per frame). Selecting a Σ speed switches
  the image type to raw and greys the TYPE button (a stack is a DNG); the
  previous type comes back with a single-exposure speed.
- **Stacking.** The engine adds the sub-frames in 32-bit pixels and writes the
  DNG as one long exposure would be: the single frame's black and white
  levels, the light of all the sub-frames on top (a Σ8s is 16 times brighter
  than a 1/2 s), the extra black pedestals removed, and what saturates
  clipped at white -- as in a real long exposure, so choose the duration (and
  ISO) as for one; EXIF exposure = the photo-equivalent. The formulas, the
  noise to expect and what lowers it: docs/STACKING.md. While a Σ speed is
  selected the viewfinder says "Σ stacking: experimental feature". The focus is frozen where the live focus left it (the
  lens position the HAL reports; no new scan), the preview is paused and the
  app shows black with the progress (frames done, time left) and an
  "abort -- keep what is done" button: an abort writes the frames added so
  far, labelled with the exposure really reached. The "Denoise" setting
  takes 2x or 4x the frames and divides them out: same brightness, noise
  divided by 1.4 or 2, the shot that much longer (shown in the brackets).
  Hard limit: 4096 frames (8 min x4). The screen is kept on for the whole
  stack (black, so cheap on an OLED) and every control but the abort is
  locked, so the user always sees the shot is running and cannot disturb
  it; if the user switches the screen off anyway, the engine is kept only
  while the stack runs and stops right after it; the app closing mid-stack
  aborts it the same way (the frames so far are kept).
- **Metering/focus circle.** Tap in auto/semi-auto -> adjusts focus AND exposure
  there; in manual -> focus only. With the focus button on `cont`, the point is
  followed continuously (no lock); on `auto`, one scan then hold. The circle
  STAYS (full while the AF works, dimmer once settled) until a tap on it or
  the AE button hands the regions back to the HAL's defaults (centre); without
  a circle the reference is the HAL's: centre AF, centre-weighted AE.
- **Consistency rules (audit, ui 113).** One notion of a held exposure
  (ExposureLogic.fixedPair: a wheel touched, or a spot held in automatic):
  photos and bursts carry that pair, so they match the viewfinder (a held
  spot used to be shot in the HAL's AE). With a held pair the flash "auto"
  does not fire, "on" fires a single flash. A scene drives the exposure: a
  tap does not start the spot measure under one, and switching a scene on
  stops it. A (re)started engine (camera, size, back from the background)
  is a new session: the page clears the circle and the loupe so both agree.
  Simple mode has no wheels: switching to it returns to automatic exposure.
  The AE button does the same thing in both orientations (landscape did not
  remove the circle) and keeps the loupe's exposure still. The page's own
  back gesture (Silica, to the gallery) is off while the loupe is open or a
  stack runs: it took the loupe's left-to-right pans (ui 115).
- **The focus loupe is a real crop of the viewfinder.** Unpinching crops the
  PREVIEW request (engine "loupe FACTOR CX CY"; photos, RAW and the meter keep
  the photo crop), so the viewfinder shows the sensor's real detail, up to
  the HAL's maximum over the digital zoom (4x on the JP2); beyond, the item
  magnifies the display. Two kinds of HAL (SCALER_CROPPING_TYPE, caps
  crop_free): FREEFORM crops anywhere, so the HAL window follows the pan;
  CENTER_ONLY re-centres every crop (a pan sent to it does nothing -- the
  "pan no longer works" seen on ui 110), so the HAL crop is kept just wide
  enough to contain the panned view and the display does the rest: full
  real detail near the middle, less towards the edges, a pan that works
  everywhere. The display never predicts the HAL: the engine sends, right
  before each frame whose crop changed, the crop that frame was really taken
  with ("fcrop", from its capture result), and the viewfinder item maps the
  requested view window into it. A HAL applies a new crop a few frames
  late; computing the display from the request made the view jump in and
  out while panning (ui 115).
- **A tap: focus first, with the HAL's AE on; then the light (ui 124).**
  One sequence in every mode. 1. Focus: "tap ... ae af" -- the HAL's AE
  region and one search on the point, while the HAL's own AE keeps running.
  The app sends no exposure change during the search. Its outcome is the
  first final AF state after the search was seen to start (a stale "locked"
  from the previous tap is ignored; a HAL that never shows the scan is
  accepted after 300 ms); then "once" holds the lens, manual focus adopts
  the distance. 2. Light: the meters are on the spot (the tap's box); in
  automatic the app sets the exposure from it (bounded, then held), in a
  priority mode the loop resumes on it, in manual nothing moves. A tap on a
  held exposure first gives the HAL its AE back (stopSpot) for the search.
  History: ui 118-123 did the light first, under an app-imposed pair (AE
  off): the JP2's AF, which relies on its 3A, was then slow and reported
  "not sure" (state 5, dashed circle) even when sharp. Focus first with the
  AE on (as in ui 117) is fast and sure; the order "light first, so the AF
  sees a well-exposed point" was not borne out: the HAL's AE with the
  region already does that.
- **The meter's preview frame.** In raw, the meter request (one capture
  every 200 ms, RAW + preview outputs) carried the photo crop and its result
  was not recorded, so its preview frame reached the UI with the wrong crop
  and no "fcrop": the view beat. It now carries the preview crop (its RAW
  output ignores crops) and records its crop like the preview's.
- **The focus window does not follow the loupe all the way.** AF region
  at least 5 % of the sensor's short side (engine tap, 0.46.7): with the
  spot-sized region, the JP2 locked 2 taps out of 20 in the loupe at 4x.
- **The spot is a box on the scene.** Centre and half-size in sensor
  coordinates, set at the tap: 12 % of the frame, so as many times smaller
  as the loupe magnifies. The viewfinder meter maps it into each frame
  through the frame's real crop (fcrop) -- defined on the frame, it measured
  another part of the scene once the loupe's crop changed; the RAW meter
  reads it on the full array (it ignored the loupe). Both sample the box
  densely enough for about a thousand points however small it is (at 4x
  the old sampling would have left ~50).
- **Spot light meter in manual and semi-automatic exposure**: above the
  circle, the light at the point as EV100 = log2(N^2/t) - log2(ISO/100) +
  log2(measure / target), from each measured frame and the pair it was taken
  with (N from the caps line's `aperture`), lightly smoothed.
- **No HAL crop changes while a finger moves the loupe.** Each change
  replaces the preview request and drops frames; changed on every move, the
  pan became jerky (ui 116). During a pinch or a pan the HAL frame stays at
  the zoom window (one change at the start) and the view is a display
  enlargement; when the finger lifts, the HAL crops again for real detail
  (one change). The loupe command is sent only when its HAL part changes,
  from a handler on the derived value (halLoupe): a handler on the gesture
  flag read the old value and never sent the sharp crop (ui 117).
- **The circle shows the outcome of its own search**: once sharp or failed
  it stays so (latched), even when "focus hold" then turns the AF off (whose
  state 0 had made the circle look as if it were searching again).
- **The loupe and the exposure.** Peaking works on real detail. While a
  finger moves the loupe without a light reference, the exposure does not
  move (AE held in automatic, the loop paused); once it lifts, exposure,
  wheels and AE work as outside the loupe (ui 126: held for the whole loupe,
  the wheels looked blocked); a tap in the loupe meters a spot as
  small as the loupe shows (the engine sizes the region on the frame), then
  holds. Taps are sent in viewfinder-frame coordinates and mapped to the
  sensor by the engine, the one place that knows the crop (this also fixes
  taps under the digital zoom). One coordinate model in the page
  ("Coordinates": zoom window, frame window, display window).
- **A tap fixes focus and exposure.** Focus: the scan runs once on the
  point; when it has locked, the lens is held at the position the HAL
  reports ("focus hold": AF off at that position -- a locked focus is a
  position, not an AF state; the same rule freezes the lens for a stack).
  Light: every circle points the app's meters at the spot (viewfinder
  measure, or the RAW meter in raw). In automatic exposure the APP sets the
  exposure from the spot, in at most four corrections (each on a frame taken
  with the previous pair; time first up to a hand-held limit, then ISO),
  then holds that pair until the circle is removed (it never disappears by
  itself: a tap on it, the AE button, or a focus-mode change); in priority modes the loop
  follows the spot; in full manual the +/- indicator reads it. The HAL's own
  AE region was tried for this (0125: region, then AE lock once converged)
  and did not work on the JP2: the HAL lets go of a region, and its
  "converged" state came before the region had any effect, so the lock held
  the whole-frame exposure. History: 0119 drove the exposure from the spot
  continuously (jerky preview, wandering focus); the bounded version keeps
  the principle "the preview request changes only after a user action".
- **Manual focus and the circle.** In distance (manual) mode a tap runs one
  autofocus scan on the point; when it settles, the distance it found
  (through the calibration, inverted by the engine) becomes the manual
  distance, shown under the circle, and the volume keys continue from it.
- **Level.** The notch button "level" draws a thin line on the true horizon
  from the accelerometer (QtSensors, optional): rotated by the phone's roll
  relative to the displayed page, shifted by its pitch at the view's pixels
  per degree (from the 35 mm-equivalent focal length); two fixed marks at the
  centre; green within 1 degree both ways. Yaw (compass heading) plays no part
  in levelling and is not shown.
- **Focus button (cycling).** "once" (Camera2 AUTO: one scan where you
  tap, then hold), "auto" (continuous focus by the HAL; a tap then only
  meters the light there, shown as a sun), distance (manual, set with the
  volume buttons; infinity is the last step). Manual distances are calibrated
  per device (focus_calibration table). More modes exist (continuous_video,
  macro, edof) and the engine exposes them.

## Volume buttons

- In distance focus mode: change the focus distance, in steps of 0.05
  diopter (the lens's own unit: equal steps are equal lens moves, fine near
  infinity), 0 D = infinity to 10 D = 0.10 m; holding a key auto-repeats.
- Simultaneous double press = screenshot everywhere (overrides the singles).


## Swipe-down menu (inside the viewfinder)

Like RAWfish in look (columns of round buttons, header naming the pressed
value, a band wider than the screen with a position bar). Columns, left to
right: Mode (simple/advanced), Timer, Format, Resolution, Quality, White
balance, Warmth, Tint, Noise and Sharpening (photos), Vf noise and Vf sharpen
(the viewfinder's own processing, optional from the Settings app), Bracket (off; x3 = a burst at one
exposure; +/-1..3 stops), Flash (off/auto/on/torch, when the camera has one),
Grid, Σ denoise (stacks), AF wait, Focus cal. (when enabled in the Settings app;
two taps), Peak, Zebra, Scenes (grouped modes; they grey the atomic controls
they drive). The full list with explanations: docs/USER-GUIDE.md.


## Output folders and names

The RAWfish-indRAW page of the system Settings app (dconf `/apps/rawfish-ir/output`, see
README): `prefix` (default `photo`), `jpgDir` and `dngDir` (default
`~/Pictures/RAWfish-ir` for both, under `~/Pictures` for the sandbox). The gallery merges both folders, newest first; DNG previews
sit in `<dngDir>/.previews/` with a `.nomedia` marker.

## Image types (three)

- `raw`       : RAW16 DNG (brut, linear). Each DNG gets a NAME_vf.jpg preview
                in the hidden .previews/ subfolder, rendered by the engine from
                the DNG's own pixels (half size, as-shot white balance): RAWfish-indRAW's
                gallery shows it in the DNG's place, with a "DNG quick preview"
                banner on its top edge saying that deleting it deletes the DNG
                (share/details/delete act on the DNG); the system gallery never
                sees it, so it cannot be mistaken for a photo.
- `jpg beauty`: the HAL's processed JPEG.
- `jpg yuv`   : our own JPEG from the raw YUV (home denoise) -- NOT yet
  implemented; planned.


## The Σ viewfinder: adaptive ISO (0.47.16)

Before, a Σ preview ran at the 0.5 s ceiling with the ISO raised by the
frame count: 2 frames per second at best, so every correction (one per
~3 frames, the HAL's latency) and every AF step crawled. Now the
controller targets a frame rate (`sigmaPreviewFps`, 10/s) and raises the
ISO to keep the same exposure (time x gain), lengthening the frame only
when the maximum ISO is reached; past even the ceiling the deficit is
multiplied back into the measure. Three states reach the interface
(slowed, darker than the photo, too dark) and are shown under the Σ
banner. The histogram keeps the right position and clipping but is noisier,
hence wider into the shadows -- as it already was at the ceiling. The
measure by equivalence is the only one possible: a 0.5 s ceiling and no
other sensor that sees the scene. The maximum analog gain is a declaration
of the HAL (the sensor), not verified; the test is in ROADMAP.

## Fine white balance and its readouts (0.47.12)

Either side of the zoom's + and − sit the warmth (5 % red/blue steps, the
menu's wbShift) and the tint (5 % green, the menu's tint), each shown for a
moment when tapped. Beside the shutter, the white balance in force in real
terms: the engine takes the HAL's colour correction gains (a neutral surface
is proportional to 1/gains in camera space) and finds its correlated colour
temperature the DNG SDK's way (since 0.47.19, `android/colortemp.h`): the
sensor's two calibration matrices interpolated in 1/T, the temperature
solved for self-consistency by bisection; then the Duv (Ohno's
approximation, + green). The first version used the D65 matrix alone and
read 1000-1300 K too low away from daylight (2400 K for a white at 3700 K,
on JP2 DNGs). Absolute values are only as good as the HAL's matrices: a
clear 40 W filament bulb (~2700 K) reads ~3300 K on the JP2.
Automatic and shifted balances alike, since the result's gains are the ones
applied. The buttons are coloured by what they do (orange / blue for the
warmth, magenta / green for the tint, white for the zoom); a readout is
coloured while its shift is not zero and a double tap on it resets that
shift. The step sizes are hand-edited in rawfish-ir.conf (fineWarmthStep in
5 % units, fineTintStep in % of green): a preference, not a menu item. The
centre column is 46 % of the deck (41 % before), the wheels and the left
buttons slightly narrower. The balance stays a setting the interface
sends (wb shift= tint=), like the scene or the flash: nothing decides on
it, so nothing of it is in the controller; the measurement is the
engine's.

## Photo orientation (0.47.11)

JPEG_ORIENTATION and the DNG's Orientation used to be the sensor mounting
alone: photos came out relative to the phone, not to the ground. The page
reads the phone's physical rotation (QtSensors OrientationSensor, gravity,
independent of the screen's lock) and sends "orient DEG"; the engine adds it
to the mounting at each shot (the other way round for a front camera, the
Camera2 rule). The viewfinder's own rotation stays the page's business.

## Shooting: the page sends a request (0.47.8)

A press of the shutter sends `sh ...` with the settings (type, quality,
processing, bracket, denoise); the controller (`ctl_shoot.c`,
ARCHITECTURE.md step 6, host test `shoot_test.c`) decides the capture lines
from its exposure and focus states. The page keeps the self-timer and the
stack overlay (fed by the engine's progress events).

## Geometry: the page is a view (0.47.7)

The zoom / loupe / frame / display windows, the HAL part of the loupe, the
gesture rule and the taps' mapping are the controller's (`ctl_view.c`,
ARCHITECTURE.md step 5, host test `view_test.c`). The page sends gestures
in display-window units (its view rotation undone) and draws what it is
told: the view window (applied by the item from `vwstate`) and the
reference point for the circle.

## Focus: the page is a view (0.47.5)

The focus sequences described above (a tap: search with the HAL's AE on,
the stale-lock guard, the 3 s limit and the late lock, "focus hold", the
manual distance adopted, the spot after) are the controller's
(`sailfish/ctl_focus.c`, ARCHITECTURE.md step 4, host test
`focus_test.c`). The page keeps the geometry (a tap to frame coordinates,
the circle's place) and draws the circle the controller reports
(`fcstate`: searching, sharp, unsure, failed, sun).

## Exposure: the deck is a view (0.47.3)

The exposure behaviour is the controller's (`sailfish/ctl_exposure.c`,
ARCHITECTURE.md step 3). `ExposureLogic.qml` keeps the names the deck used
(fullAuto, displayIso, fixedPair, touchIso(), setEv()...), but sends the
user's choices as `xp ...` commands and reads the controller's `xpstate`.
The rules described above (spot measure, held pair, loop, offset) are the
same, now testable on a PC (`sailfish/tests/exposure_test.c`).

## Histogram

RGB, with a clipping bar each side whose height shows how many pixels are
clipped. A mix of RAWfish's first and second versions. Two sources, by type:
- **jpg**: the viewfinder frame, i.e. the HAL's processed image (its tone
  curve lifts shadows and midtones; bars = 0 and 255), measured by the
  engine (`vfmeter`, since 0.47.1);
- **raw**: the sensor data, linear, black-subtracted, on a 1/2.2 power scale
  (bars = at or below black, at or above white).
The same scene therefore shows a raw histogram narrower and further left
than the jpg one, with a bigger left bar: in deep shadows the read noise
straddles the black level, so about half of the truly black photosites fall
below it and count as "no information" -- which is what the DNG will hold.
The raw bar is honest, the jpg one lenient; neither axis matches the other.
Both cover what the interface SHOWS (0.47.14): the controller sends the
engine its display window (`viewwin`) with every view change, and both
meters restrict their histogram pass to it -- the RAW frame ignores crops,
and the HAL's frame can be wider than the view (a centre-only HAL crops
little away from the centre, the display enlarging the rest; 0.47.13 used
the HAL's crop and so showed the whole image in a panned loupe). With
a light reference, both meters add the spot's own histogram (`shist=`, its
own scale), drawn as bright outlines over the whole picture's: where the
circle's tones sit.


## Preview and photo processing are separate

The photo's noise reduction and sharpening travel with each JPEG / burst
command (the Noise and Sharpen columns); the `preview` command sets the
viewfinder's only (Vf noise / Vf sharpen, default fast noise reduction and
no sharpening). Earlier one command set both, and a photo choice (no noise
reduction, sharpening on) turned a high-ISO preview into a maze of sharpened
noise.

## Capture flash (the screen's white flash, not the LED)

Like RAWfish, on shutter. It must fire the instant the user can move again =
at END OF CAPTURE (end of exposure / last sub-frame of a stack), NOT at end of
file writing. Implemented via the engine's SF2M "captured" event, emitted before
the file is written.


## Engine <-> UI channels

One stdout stream carries two packet types, told apart by a 4-byte magic and
serialised by an output lock (like RAWfish's SF2P/SF2M):
- **SF2P**: preview image frames (16-byte header + RGB888).
- **SF2M**: metadata/events, one text line (8-byte header + text). The base for
  all live data the UI needs (captured now; iso/shutter, AE, histogram... later).
Commands go to the engine's stdin; real errors stay on stderr; JSON reports go to
the --log file in stream mode.


## vs RAWfish -- what we keep and what we refuse

KEEP (reuse / model on):
- The architecture: a QML UI + a C++ QQuickItem viewfinder + the engine run as a
  separate process streaming frames.
- The SF2P frame protocol and the SF2M metadata/event channel, serialised under
  an output lock and distinguished by magic.
- The layout, proportions and component shapes (caption header, 4-column grid,
  CycleValueButton, ValueCarousel-style wheels, the Jolla shutter icon).
- The Camera2 mode names and the capability-derived wheel values.
- The swipe-down settings menu (Jolla Camera style: dimmed image, header
  naming the pressed value, columns of round items in a band wider than the
  screen with a position bar) and the gallery as a pushed page with the
  system's ImageViewer and GalleryOverlay (share, delete, details).

REFUSE (do differently):
- NO renderExposure / compensated viewfinder: ours is strictly WYSIWYG, showing
  the real frame (RAWfish switched to an always-well-exposed viewfinder judged
  only by the histogram -- not wanted).
- NO cosmetic digital gain anywhere, in the viewfinder or the file; and
  renderExposure must not reach the JPEG either (RAWfish applied it to the file).
- NO silent/combined settings: everything atomic and reported.
- Manual focus distances are CALIBRATED per device (RAWfish's distances did not
  match reality; ours use a measured focus_calibration table).

SAME MECHANISM AS RAWFISH:
- Exposure compensation (the EV wheel, the simple mode's brightness slider):
  Camera2 CONTROL_AE_EXPOSURE_COMPENSATION on the requests, i.e. the HAL's
  AE itself exposes differently; RAWfish sets the same key. (RAWfish ALSO
  had a display gain, renderExposure, which we refuse; our EV is only the
  HAL's.)
- White balance: applied by the HAL's ISP after the sensor, to the preview
  and the JPEG, never to the RAW (as-shot neutral is metadata only). The
  presets are CONTROL_AWB_MODE in both. Manual WB differs in HOW the HAL is
  told: RAWfish sends a Kelvin value through a MediaTek vendor tag
  (MTK_3A_AWB_VALUE, AWB off); RAWfish-indRAW shifts the AWB's own gains through
  the standard colour-correction keys (device-independent, no Kelvin scale,
  "warmer/cooler than the AWB chose"). The vendor Kelvin tag is noted for a
  possible later addition where the HAL offers it.

SHARED LIMITATION (not solved by either):
- Viewfinder "watercolour" denoise at high ISO is the JP2 HAL's own preview
  processing (Jolla Camera and RAWfish show it too); the noise/edge modes do not
  affect the preview. Photos are unaffected (off_noise JPEG and RAW are sharp).
  Possible MediaTek vendor key noted for later.


## Build phases (status)

- Phase 1 (done): preview frame diagnostics (YUV->RGB, framing, 3A-settled grab).
- Phase 2 (done): continuous SF2P streaming (stream mode), immediate start.
- Phase 3 (in progress): the app -- viewfinder displays the stream; deck built
  (col 1 cycle buttons, histogram scaffold, Jolla shutter, ISO/shutter/EV wheels,
  zoom +/-); shutter wired to a real capture command; flash on SF2M captured;
  orientation locked portrait.
- Phase 3, exposure (done, ui 65): auto / priority / manual wheel logic
  (touch = manual), live AE values on the wheels, EV wheel (HAL compensation
  in auto, loop target in priority, measured offset number in manual), notch
  AE button, RGB histogram with clipping bars, RAW16 exposure meter. See
  "Exposure metering" below.
- Phase 3, the rest (done, ui 77): zebra / peaking / grid (thresholds tunable
  per device in override-hal.json), metering/focus circle styled by the AF
  outcome (dotted searching, dashed impossible, solid sharp), swipe-down menu
  (timer, resolution, format, quality, white balance, noise, sharpening, grid,
  bracketing; settings persisted), gallery on an inner left-to-right swipe
  with DNG previews (NAME_vf.jpg), real zoom, focus loupe, volume-key focus
  and double-press screenshot, UI rotation, cameras shown by focal length.
  Packaging: rawfish-ir-cli + rawfish-ir-gui RPMs, one shared version.
- Since then (ui 78-105, see CHANGELOG.md): RAWfish-style menu, simple and
  advanced modes, scenes, manual white balance, stacked exposures (Σ) with
  progress/abort, DNG previews from the raw, gallery as a system page, output
  folders and a Settings page, level, spot metering held by the app, focus
  calibration assistant, burst (x3) and flash.
- Later: the home YUV JPEG (`jpg yuv`).


## Exposure metering (how semi-auto and the EV number work)

Camera2 has no priority AE: AE is all or nothing. So in the priority modes the
preview runs in TRUE manual (AE off, a fixed ISO/shutter pair on the repeating
request -- what the photo will be, no display gain) and the UI closes its own
AE loop on the non-pinned value:

- **Source, raw type**: the engine's exposure meter. It captures a RAW16 frame
  every 200 ms (5 Hz, `meter on rate=5`), measures it in place (linear,
  black-subtracted, centre-weighted green mean normalised to the white level,
  plus a 64-bin RGB histogram and clipping counts) and streams the numbers as an
  SF2M `meter` line; the frame itself is never stored or written. The meter is
  paused while a photo uses the RAW stream. The RAW is the file's own latitude,
  which the viewfinder stream does not have. Cost on the JP2 (preview fps while
  the meter runs) is still to be measured; the rate is adjustable.
- **Source, jpg types**: the viewfinder frames, measured in the app (display
  gamma removed with a nominal 2.2 -- the HAL's exact curve is to be measured
  and the extrapolation refined), since the HAL's JPEG shares the preview's
  processing. The RAW meter is off in jpg, so it costs nothing there.
- **Target ("what auto would choose")**: calibrated while in full auto -- the
  mean the HAL's converged AE settles on (EV compensation undone), per source,
  smoothed; 18% before the first calibration. It is a constant of the HAL's AE,
  not of the scene: when the scene changes, the measured mean moves and the loop
  moves the free value back to the target, like a classic AE. Known limit: the
  loop reproduces the HAL's global brightness, not its scene decisions
  (backlight, highlight protection); to be refined against measurements.
- **Loop**: damped (0.7 of the error per step), 0.1-stop dead band, at most one
  command per 150 ms, correcting from the measured frame's own ISO/exposure so
  latency cannot make it overshoot. The EV wheel shifts the target by 2^EV.
- **Full manual**: the same measurement gives the number shown in place of the
  EV wheel: stops above (+) / below (-) the target.
- **Stacked shutter (Σ) in priority/manual**: the preview cannot show a 16 s
  exposure; it runs at the sub-frame length (the ceiling, 1/2 s) with the ISO
  raised by the frame count, capped at the sensor maximum -- ISO is the only
  amplification we allow, and it is reported. The photo is the real stack at
  the wheel's ISO. (Decision to confirm.)

## The HAL's automatic white balance: only part of the way (measured, 0.47.19)

Measured on the JP2, 20 October 2026, with a neutral diffuser (Lee 216, and
a white polycarbonate within 2 % of it) in front of the lens:
- overcast daylight: the AWB took the light for ~4900 K where the white
  reads ~5230 K; on the bare sky ~4460 K against ~5510 K for the brightest
  cloud: JPEGs come out COLD (blue ~14-40 % too strong);
- a clear filament bulb: the AWB took it for ~3710 K where the white reads
  ~3320 K: JPEGs come out WARM (red ~20 % too strong).
The AWB corrects only part of the way, pulling every light towards the
middle (~4000-4500 K): a deliberate "keep the mood" choice on many phones,
and the reason Jolla Camera's outdoor photos look cold (same HAL). A fixed
default shift cannot fix it (the error changes sign); a white balance set
on a neutral object can (to do: a long press on the kelvin readout). The
DNG is not affected: its "as shot" white is only a starting point.

## The white measured on a neutral object (0.47.20, refined 0.47.21)

The answer to the AWB's partial correction for JPEGs: a 2.1 s press on the
kelvin or tint readout (a ring fills, the measured zone is drawn until the
measure is over; release to cancel; not the shutter, where a slip takes a
photo) sends `wb pick`. The engine measures R, G, B on
one RAW frame -- the spot of a light reference, else the middle 12 % of the
displayed window --, refuses clipping (> 2 % of the quads near white) and
darkness (green under 3 % of the range), and keeps gains making that white
neutral (green 1) as the base of the manual white balance, with the AWB's
last colour transform. The menu's shift and tint reset and apply on top; the
AWB mode waits. The gains are saved (`wbFixed` in rawfish-ir.conf) and re-applied
at start with a button "Keep previous manual WB" and a 6 s bar: without a
click the AWB comes back (0.47.21: a forgotten measured white would colour
every later photo). A double tap on a readout returns to the AWB. Files say
what was applied (0.47.21): the DNG's AsShotNeutral is the inverse of the
applied gains whenever the white is manual (the HAL's NEUTRAL_COLOR_POINT
stays its own estimate), and EXIF WhiteBalance is 1 (manual) in the JPEG and
the DNG. The readouts turn light blue while a
measured white is kept. A setting the interface sends, like the others: the
engine measures, nothing in the controller decides. Off-centre picks carry
a little of the sensor's colour shading (the raw corners are tinted, the
HAL corrects it later): the middle is the most exact.

## The menu, trimmed (0.47.21 / ui 138)

- Warmth and Tint left the menu: the ± buttons around the shutter do it,
  with the readouts (the settings are the same: wbShift, tint).
- AF wait left the menu: since the controller decides the shots, a photo
  never asks for a focus scan (the lens is held, or manual); the wait only
  bounds the flash's AE precapture, and 1.5 s is needed for it to meter
  (shorter: over-exposed). The engine's default is 1.5 s; the command stays
  for scripts.
- Scenes are hidden unless Settings > RAWfish-ir > "Scenes in the camera menu
  (not yet implemented)" is on; then in simple mode too. A scene left on
  with the switch off is ignored (sent as off).
- The swipe that opens the menu needs 20 % of its height (40 % before:
  frequent misses).

## The gallery: one strip with the camera, as Jolla Camera (ui 142)

ui 139 made the gallery a strip that ends, but it was still a separate PAGE
before the camera: leaving it, the strip slid under the finger, then the
page transition started from scratch and caught up -- two movements, slow.
Jolla Camera (com.jolla.camera's CameraPage and GalleryView, BSD-3-Clause,
read on the phone) does it with ONE page: a Silica PagedView "switcher"
holds [ gallery | camera ], opening on the camera, no wrap; the gallery is
itself a PagedView, RightToLeft, no wrap, newest at index 0 next to the
camera. Nested PagedViews hand the gesture over: a swipe past the newest
capture carries on into the camera in the same movement, and back.

RAWfish-indRAW now does the same: the camera page is the root page; its four
visual blocks (notch band, viewfinder zone, settings menu, deck) are the
second item of the switcher, GalleryView.qml (ex GalleryPage.qml) the
first, loaded asynchronously. The switcher is not interactive while a
horizontal drag means something else on the camera (loupe pan, the menu
open, a stack) or the gallery's picture is zoomed. Leaving the camera for
the gallery puts the gallery on the newest capture first. The camera's own
swipe handler no longer opens the gallery: horizontal drags belong to the
switcher. The viewfinder keeps running under the gallery, as before.

## Start in manual (ui 140)

Settings > RAWfish-ir > "Start in manual focus and exposure" (dconf
/apps/rawfish-ir/menu/startManual, off by default). At the first start of the
engine in an app run, 1.5 s after (so that, with nothing saved, the wheels
are pinned where the automatic exposure settled): "fc distance" with the
last distance, focus mode manual, ISO and speed pinned at the last values.
The last values (lastFocusD, lastIso, lastShutterNs in rawfish-ir.conf) are
saved when the app goes to the background or closes, not at every change.
Advanced mode only.

## Commented references; peaking and zebra out of the menu (ui 141)

rawfish-ir.conf cannot hold comments (QSettings rewrites it and drops them),
and JSON has none: the app installs two commented references at each
start, `rawfish-ir.conf.example` (every key: function, values, limits, default;
the dconf settings listed apart) and `override-hal.example.jsonc` (real `//`
comments, so not loadable JSON, never read; checked that it is valid JSON
once the comments are stripped). The real override-hal.json stays plain
values, without comments. Shipped in
/usr/share/rawfish-ir/examples, copied next to the real files, which are never
touched. The Peak and Zebra tweak columns left the menu: peakTweak and
zebraTweak stay in rawfish-ir.conf, the device values in override-hal.json.

## Any screen, any photo shape (ui 144)

The viewfinder used to be full width at 4:3, the deck taking the rest: on
a 16:9 screen (Xperia XA2, 1080x1920) the deck got ~400 px and lost its
shutter row off-screen. Now the viewfinder takes the photo's proportions
(the streamed frame's; the engine picks a preview matching the photo since
0.47.27) and is as large as the deck allows: in portrait full width unless
the deck would get less than its minimum height -- computed from its own
content: caption row, side buttons, and a column holding the histogram
(34 %) above the shutter block -- then narrower and centred, black at the
sides; in landscape full height unless the deck would get less than its
minimum width. On the JP2 (4:3 photos, tall screen) nothing changes.

ui 145: the deck is SCALED first. Sailfish gives its controls the same
physical size on every phone, so on a smaller screen the deck takes a
larger share (on the XA2, more than half the screen, the viewfinder a
400 px strip). The deck is now laid out at its natural size and scaled
uniformly from its top-left corner (text, buttons, spacing and touch areas
together) until the viewfinder gets its full width at the photo's
proportions, down to 50 %; only beyond that does the viewfinder shrink.
The scale comes from the screen's sides (the same in both orientations).
The sides of a narrower viewfinder are black (they showed the window's
white).

## The gallery's folders at creation (ui 146)

Since the gallery became the switcher's first item (ui 142), its Loader set
the camera's item (and so the folders) after creating it: the folder models
were created on an empty folder and only filled when a new file arrived --
"No captures yet" until a photo was taken, then every capture. The Loader
now passes the item at creation (setSource with initial properties). The
list is rebuilt only when a folder's file count changes (the models watch
the folders), not on every visit (costly with many captures); a file whose
details are not read yet is skipped instead of aborting the list, and the
list is then rebuilt once more half a second later.

## Menu or gallery: who gets a drag (ui 148)

Since the gallery became the switcher's first item (ui 142), Silica's
PagedView took any drag leaning a little sideways, before the viewfinder's
swipe handler could see a slanting downward swipe as the menu. Since ui
151 the switcher's own dragThreshold is ~1 cm: a drag going down ~0.5 cm,
at least 3/4 as much down as sideways (~37 deg or steeper), is the menu
first, and from then on the handler keeps it (preventStealing); a flat
drag passes the 1 cm and is the switcher's, which follows the finger to
the gallery. (ui 148 kept every drag and tried to hand a sideways one back
mid-gesture: a PagedView only takes a gesture from its start, and the
gallery no longer opened; ui 149 then slid the switcher by program. Both
removed: PagedView's dragThreshold, documented, does it natively.)

The 16:9 format's grey bands (ui 85) assumed a 4:3 preview; since ui 144 the
preview has the photo's shape, so they hid part of a 16:9 viewfinder. They
now appear only if the photo's format is wider than the streamed frame
(no preview of that shape offered), cutting the actual difference.

## The gallery's list, row by row (ui 150)

Deleting a capture sent the strip back to the newest one: the list was
cleared and refilled at every change of a folder, which a Qt view takes
as a reset (back to the first row, every picture recreated). Jolla
Camera's capture model (C++, libjollacameraplugin) removes and inserts
rows instead (beginRemoveRows / beginInsertRows) and leaves the rest to
the view. The gallery now does the same with its ListModel: a file gone,
its row removed; a new file, a row inserted at its place (both lists are
newest first); a file whose date changed, its row moved. The view stays
on the capture shown; when that is the one deleted it shows the one
taking its place, the next older, so sorting goes on from there. As in
Jolla Camera, a new capture arriving while the newest is shown is shown.
Harness (Qt ListView standing in for Silica's PagedView): delete the
viewed third capture -> the next older one; a new capture while sorting
-> still on the same one; a new capture while on the newest -> the new
one. How Silica's PagedView treats the removal of its CURRENT row is the
one thing to confirm on the phone.

Deleting the capture shown (ui 151): Silica's PagedView, when its CURRENT
page is removed, slides as if towards the newer neighbour while showing
the older one -- an animation in the wrong direction, and PagedView has no
setting for it (documented members: direction, wrapMode, moveDuration,
drag thresholds, moveTo). The gallery therefore never removes the page
shown: it first moves to the older neighbour still there, or the newer
one if the deleted capture was the oldest, and only then removes the gone
row. The move is IMMEDIATE (moveTo, PagedView.Immediate) since ui 152: the
first try (ui 151) animated it with currentIndex, and Silica animated that
programmatic move the wrong way too -- with this RightToLeft strip,
whatever Silica moves by itself slides as if left-to-right; only the
finger's own swipes go the right way. The neighbour now simply replaces
the deleted capture. (Jolla's gallery app slides
a deleted picture down and returns to its grid; Jolla Camera's strip uses
the same PagedView as ours.)

## Focus while zoomed (0.47.31)

Measured on the JP2 (October 2026): zoomed, in automatic focus with no point
tapped, the focus went to an object just OUTSIDE the zoomed frame, and the
sharp zone sat a little low in the image: without a region of ours the HAL
focuses on a default area of the whole sensor, wider than a zoomed view.
The exposure, tested the same way, is right (a light leaving the zoomed
frame stops counting): untouched. Focus by a tap was right too.
Narrowest fix, on purpose: only zoomed (above x1.01) and with no point
tapped for focus, the engine puts an AF region on the requests -- the
middle third of the zoom window, through the same entry a tap uses; at
zoom 1 it REMOVES that entry again (the HAL's own default, exactly as
before); a tap's region replaces it, a "clear" brings it back while zoomed.
The loupe alone, zoom 1 and every tap are unchanged.

## Zoom by holding (ui 153)

The deck's centre + / - : a tap steps by 0.25 (0.5 before: twice as many
steps), a double tap goes to the max / back to x1, and a hold (Qt's
pressAndHold, ~0.4 s) zooms progressively. By proportion -- the zoom is
multiplied by 2^(rate x dt), so x1 -> x2 takes as long as x2 -> x4, as a
magnification is seen -- and faster the longer the hold: rate = 0.5
doublings/s at the start, + 0.75 per second, at most 2 doublings/s (after
2 s). The JP2's x4 (two doublings) takes ~1.75 s of zooming. A value at
most every 66 ms (~15/s) goes to the controller; the hold ends on release,
cancel, or at a stop (x1, max). Harness: x1.09 at 0.26 s, x1.50 at 0.75 s,
x2.23 at 1.25 s, x4 at the stop.

## The zoom's double tap, and the fine white balance's quick taps (ui 154)

The zoom + / - told a double tap with Qt's onDoubleClicked, i.e. the
system's window (~0.4 s): two deliberate taps jumped to the end. The app
now tells it itself (zoomTap): the same button twice within 0.15 s is the
double tap (max / x1), further apart two steps of 0.25. onDoubleClicked
is gone from those buttons, which also cures a hidden loss: with it
connected, Qt swallows the second tap of any quick pair, so two quick
taps on the warmth or tint + / - (same buttons' row) counted as one.

## DNG thumbnails: vignetting and colour (0.47.36)

The gallery's DNG thumbnail (made by the engine from the DNG's own pixels,
NAME_vf.jpg) now applies the SAME gain map as the DNG -- undosed (balance 0,
strength 100), so it shows the corrected brightness, not the pivot's
darkening meant for darktable -- and takes the sensor RGB to sRGB through
ColorMatrix1 (inverted to camera->XYZ, composed with XYZ->sRGB, each row
scaled so the as-shot neutral is white, which carries the white balance).
Before, the thumbnail showed the raw corners (dark) and the camera's own
RGB (flat colours), so measured and HAL shading looked the same in the
gallery. JPEG thumbnails are unchanged (the HAL already rendered them). The
assistant's before/after preview of a flat field builds on this.

## Lens-shading assistant (ui 156)

A page pushed over the camera from a Settings request (dconf
/apps/rawfish-ir/device/shadingCalRequested, cleared as soon as it is served,
so a crash or a back-swipe never reopens it). It only sends 'shading
calibrate ...' and listens to the engine's cal_exposure / cal_progress /
cal_done events (IndrawPreview signals calExposure / calProgress /
calDone); the camera page is untouched. Harness: the page builds the
full and the add-a-light commands, shows progress and the exposure, lands
on the result for success and for failure.

## DNG thumbnails: highlights white, not pink (0.47.49)

The 0.47.36 thumbnail took sensor RGB straight through a matrix normalised
on the as-shot neutral. A pixel the sensor saturated has all channels at
white, (1,1,1) -- which for the sensor is NOT neutral (a grey is about
(0.51, 1, 0.63)): the matrix read it as too much red and blue and rendered
clipped skies and lamps pink ((2.32, 0.72, 2.04) before the 8-bit clip).
Flat fields, never clipped, looked right. Now, as darktable's 'clip
highlights': white balance first, each channel clipped at 1, then the
matrix (folded with the neutral so it takes balanced RGB). A saturated
pixel is white; greys are unchanged (0.50 -> 0.50, 0.88 -> 0.88).

## Screens with no notch, a compact deck, the white-balance readouts (ui 159)

On the Xperia XA2 (1080x1920, 16:9, no cutout) the viewfinder did not take
the screen's width even in 4:3: the black band reserved for the JP2's notch
and the full-size deck left too little height. Now:
- bandOverlay (portrait, screen up to 1.9:1, a heuristic for "no notch"):
  zebra / peak / level / AE lie over the top of the viewfinder, the words
  alone with no background (ui 160), and the viewfinder starts at the top of
  the screen;
- compactDeck (portrait, when the photo at full width leaves less room than
  the normal deck): the histogram takes 20 % of the deck's column instead
  of 34 %, the + / shutter / - rows are tighter, the left buttons' values
  smaller, the wheels shorten with the deck, the ISO wheel moves a little
  right. deckScale still applies on top.
- the colour-temperature and tint readouts sit between their own + and -
  (the warmth's in the left third, the tint's in the right one), no longer
  against the shutter -- on every phone.
Harness: XA2-like screen, 4:3 -> viewfinder 1080x1440 (was 954 wide),
compact deck at full scale; 16:9 -> still narrower (954): a 16:9 photo on a
16:9 screen leaves no room under it -- the deck would have to lie over the
image. JP2-like screen (1080x2400), 4:3: unchanged (no overlay, normal
deck); 16:9: the compact deck lets the viewfinder take the full width.

ui 164 (JP2): the notch gap is 40 % of the width (28 % put peak and level
at the cutout's edges); in the normal deck the histogram takes 44 % of its
column (was 34 %, capped so the + / shutter / - block keeps its room) and
that block spreads over the height left under it. Harness, 1080x2400 4:3:
column 884, histogram 389, block 401..876. The compact deck is unchanged.

ui 165: on the real JP2 in 4:3 the deck is COMPACT (Theme sizes larger
than the harness's), so ui 164 changed nothing visible there. The compact
histogram goes from 20 % to 26 % of its column (+30 %), and the + /
shutter / - block spreads to ~2 mm from the bottom in both decks (minimum
spacing 1.5 mm compact, 3 mm normal; the histogram is capped so the block
keeps its room). Lesson: the harness's Theme is not the phone's -- check a
layout change against a phone screenshot.

## Frame parsing and the aids off the UI thread (ui 167)

parseFrames() walks m_buffer with a read index and removes the consumed
bytes once at the end (it removed every packet from the front of a multi-MB
QByteArray: one memmove per packet); when several frames arrive in one
batch only the newest is copied into m_frame. The zebra / peaking overlay
(computeOverlay, a pure function) is computed by OverlayWorker on its own
QThread: the UI thread hands it the latest frame and the aids' settings,
gets the ARGB overlay back by a queued signal, and a frame arriving while
it works only replaces the one waiting (it skips, never queues). The
overlay therefore lags the frame it was computed for by at most one
worker run; the aids' setters still call buildOverlay().

## Cameras without RAW (ui 169)

The JP2's front and wide cameras offer no RAW16 stream (caps raw=0,0; the
engine opens them with raw16_size [0,0]). page.rawAvailable follows the
caps: without RAW the TYPE button is greyed on "jpg", the Σ speeds leave
the shutter wheel (a stack is a DNG), and a "raw" type in force is set
aside and restored when a camera with RAW comes back. Unknown (no caps
yet) counts as available, so the first camera starts as the user left it.

## One engine start per camera switch; the format remembers its size (ui 170)

The second start on every camera switch (JP2 log: '# settle: format 4:3
wants 6560x4928, engine runs 4096x3072 (restart)' for a camera just opened
at 6560x4928) came from QML's binding order: inside onJpegSizesChanged the
other bindings on the same new caps (runningSize, sensorFormat) are not
re-evaluated yet, so settleSize compared against the previous camera's
size. settleSize now runs through Qt.callLater, once every binding has
settled: one start per switch, as in the harness.
Each format keeps its own last-chosen size (captureSize43 / captureSize169
in rawfish-ir.conf): coming back to 4:3 restores the resolution chosen there
instead of the largest.

## Level colours, log rotation (ui 171)

The level's horizon line turns yellow when the roll is level (within 1
degree), wherever the pitch puts it; the two centre marks turn yellow when
the pitch is straight; both turn green when both are; white otherwise.
The engine's log (~/.cache/com.rawfish/rawfish-ir/rawfish-ir.log) is rotated by
the app before each engine start once it passes 2 MB: the previous .1 is
removed and the log becomes it. Two files, 4 MB at most.
