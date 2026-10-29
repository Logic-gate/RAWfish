# Changelog (AI generated)

Since 0.1.0 one version covers the library and both packages
(`indraw_version.h`, the two specs); before it, the versions were the
engine's, with the app's and its "ui NN" badge alongside. Patch numbers
refer to the patch series the changes were delivered as (kept outside the
repository). Newest first.

## Known issues (as of 0.1.5)
- At high ISO the viewfinder can show a fine "maze" pattern. Not the
  MediaTek 3DNR (tested 21 October 2026: the setting is applied and
  changes nothing visible, docs/VENDOR-TESTS.md); the viewfinder's own
  noise reduction is the next candidate. Photos are not affected.
- The Xperia XA2 runs indRAW as a proof of concept (32-bit, 16:9
  sensor); RAW and Σ are not yet tried there.

## 0.1.5 (ui 185) — the engine restarts after a fault; a camera switch from RAW no longer asks the front camera for the main one's size (patch 0282)
- An engine that stopped was never restarted, although the app said it
  would try three times: "engine stopped" stayed until a camera switch or
  a new start of the app (a JP2 user's "many engine stopped"). It now
  restarts.
- Switching from the main camera in RAW to a camera without RAW could ask
  the new camera for the main camera's size (50 MP on the JP2), which it
  does not list: the engine refused to open. The size is now settled once
  the new camera's sizes are known; and an engine that dies before it has
  opened is restarted at the camera's largest size.
- The log's "# caps" line gives the focal length, its 35 mm equivalent and
  the RAW size.

## docs — the README's RAWfish link (patch 0281)
- The README's technical part linked "RAWfish" to the forum's home page;
  it points at RAWfish's repository, like the credits above it.

## 0.1.4 (ui 184) — no engine restart at the first switch to RAW (patch 0280)
- On a fresh install the first switch to the RAW type showed "Starting
  camera..." (XA2): the type change always set the photo size, and the
  size still unset meant a restart, although the engine already ran at
  that size. A type change now settles the size like a camera change: it
  restarts only when the size really changes (on the JP2, going from the
  50 MP JPEG size, which RAW does not allow, still needs one).

## docs — dconf keys, a fresh install (patch 0278)
- BUILDING lists the dconf keys and says how to check what a new user gets
  ("Starting from scratch", done on the JP2 on 29 Oct 2026: all fine). The
  carry-over of indRAW's files is gone: nobody but its author ever
  installed indRAW.

## CI — the host checks on every push (patch 0276)
- A GitHub Actions workflow (.github/workflows/check-host.yml) runs
  scripts/check-host.sh on every push and pull request (gcc and clang,
  pinned NDK headers, every host test, the sanitizers, the lock checker).
- scripts/check-host.sh now fails when a host test fails: the tests ran
  piped into "tail -1", which hid their exit status (a FAILED test printed
  FAILED and the check still passed).

## 0.1.3 — the lock contract written and checked; the controller under ThreadSanitizer (patch 0275)
- The engine's threads and locks are written down (android/indraw.c's
  header, docs/ARCHITECTURE.md "Threads and locks") and the checker
  enforces three rules: g.lock never re-taken, the order g.lock -> out_lock,
  the output descriptors only under out_lock (it found one place that
  reset them outside the lock, at setup: fixed). The checker tests itself
  on a bad sample.
- A two-thread stress test of the controller (the event reader against the
  CLI's commands) runs under ThreadSanitizer, and Helgrind when valgrind is
  installed: no report.

## 0.1.2 — packaging and documentation from the October 2026 audits (patch 0274)
- Both packages ship the licence (%license); the duplicate LICENSES/LICENSE
  is gone (the app's build keeps an identical copy, app/LICENSE).
- CREDITS says plainly what RAWfish-indRAW owes RAWfish: its logic and the
  look of its interface, modelled on it, without copying its code.
- The README's examples work again (file names are prefixes; the engine
  writes DNGs), and it says that the command-line tool is not sandboxed.
- The host check fetches its Android headers at pinned commits and checks
  their SHA-256 (they came from a moving branch).
- References to files that are not in the repository (docs/METHOD.md, the
  patch series) are gone.

## 0.1.1 — bugs from the October 2026 audits (patch 0273)
- The viewfinder's frame reader refuses a header whose size does not add
  up in 64 bits (a 65536 x 65536 header announcing 0 bytes slipped through
  and crashed the app), and no longer makes a short event wait for the
  next frame.
- The engine never writes a frame header larger than any sensor.
- The controller's lock is created once, recursive from its first use.
- A JPEG whose camera Exif had more entries than room says so in its reply
  (exif_hal_dropped) instead of dropping them silently.
- RAW16 and stack sidecars carry the phone's make, model and the photo's
  orientation; scripts/raw16-to-dng.py writes the sensor's own Bayer
  pattern, make, model and orientation (it wrote "Jolla Phone 2", BGGR, no
  rotation for every phone), and a stack shorter than its denoise factor no
  longer gives black and white levels of 0.
- The settings menu's viewfinder Noise / Sharpen columns had kept the ZSL
  entries removed in 0266 (an error at every menu build).

## 0.1.0 — RAWfish-indRAW: indRAW joins RAWfish as a branch; one version for both packages (patch 0271)
- New names, decided with RAWfish's developer: the project is
  **RAWfish-indRAW** (the `indraw` branch of RAWfish's repository); on the
  phone it shows as **RAWfish-ir**; the technical name is `rawfish-ir`:
  packages `rawfish-ir-cli` (the engine: library, controller, command
  line; installable alone) and `rawfish-ir-gui` (the interface, which
  requires the cli), binaries `/usr/bin/rawfish-ir` and
  `/usr/bin/rawfish-ir-cli` (`/usr/libexec/rawfish-ir/rawfish-ir-cli`),
  library `librawfish-ir.so`, shipped data under `/usr/share/rawfish-ir/`,
  user data under `~/.local/share/com.rawfish/rawfish-ir/`, settings in
  `~/.config/com.rawfish/rawfish-ir/rawfish-ir.conf`, dconf under
  `/apps/rawfish-ir`, the log `~/.cache/com.rawfish/rawfish-ir/rawfish-ir.log`,
  photos in `~/Pictures/RAWfish-ir/` by default. The Exif and DNG Software
  tag says "RAWfish-ir 0.1.0".
- Installs next to RAWfish 1.x (nothing shared): both can be used on one
  phone. Nothing is carried over from the indRAW packages (they were only
  ever installed by their author).
- The sources keep the engine's development name, indraw (files,
  functions, QML): nothing internal was renamed.
- One version number from now on, shared by the two packages and the
  library, starting at 0.1.0 (a pre-release; 1.0 when it is judged ready
  to distribute). The history below keeps its old numbers.

## 0.47.74 — app 0.20.57 (ui 181) — the burst gap withdrawn (patch 0270)
- The x3 burst's "Gap" (0.47.73) is withdrawn: the JP2's camera ignores
  the frame duration on the burst's frames, the three always came 34-41 ms
  apart whatever the setting. Measured and recorded in the docs; further
  tries are in the roadmap.

## 0.47.73 — app 0.20.56 (ui 180) — a gap between the frames of the x3 burst (patch 0269)
- With Bracket x3 in JPEG, a new "Gap" column spaces the three frames:
  as fast as possible, or 0.1, 0.2, 0.3 or 0.5 s apart. The sensor itself
  spaces them (exact), up to what the camera delivers; a pose longer than
  the gap sets the pace. The burst's reply in the log gives the spacing
  obtained.

## 0.47.72 — a RAW no longer waits a second before it is taken (patch 0268)
- With the RAW meter on (the RAW type), every RAW photo waited a full
  second before its capture: the meter frame in flight was mistaken for
  the photo's and its slot never freed, so the wait ran to its timeout.
  The RAW now flashes as soon as the JPEG does (stacks and the noise
  profile benefit too).

## build — the packages keep their specs' versions after a git tag (patch 0267)
- With a release tag in the history, sfdk renamed both packages after the
  tag (indraw-app-0.47.69+main...), and the app could not find the
  engine version it requires. The build scripts now tell sfdk to keep the
  specs' versions.

## app 0.20.55 (ui 179) — the ZSL noise and sharpening modes leave the menu (patch 0266)
- The zero-shutter-lag modes are no longer offered in the Noise and
  Sharpen columns: they belong to a capture path indRAW does not use, and
  alone mean whatever the HAL decides. The simple mode's photos use the
  minimal noise reduction and high-quality sharpening. A ZSL capture to
  shorten the shutter delay is in the roadmap.

## 0.47.71 — app 0.20.54 (ui 178) — the shutter's 'denoise' field renamed 'denoise_sigma' (patch 0265)
- In the shutter command (and so in the log), 'denoise' becomes
  'denoise_sigma': it is the Σ stack's poses per frame, not a noise
  reduction mode. The old name is still read.

## 0.47.70 — milliseconds in the log (patch 0264)
- The log's times now have milliseconds: a capture's real duration, and
  where the time goes, can be read from it.

## 0.47.69 — DNG thumbnails in the right colours under warm or LED light (patch 0263)
- The gallery's DNG thumbnail used the sensor's daylight colour matrix
  whatever the light: under a 3500 K LED it came out yellow-green while
  darktable showed the scene neutral. It now follows the DNG standard:
  the light's temperature from the white balance, the two matrices
  interpolated at it. New photos only.

## app 0.20.53 (ui 177) — the version badge clear of the rounded corner; the roadmap rewritten (patch 0259)
- The small "ui" badge moves a few millimetres left, out of the screen's
  rounded corner.
- docs/ROADMAP.md reorganised: open items (before a release, features,
  to find out), ideas, decided against, and what is done.

## 0.47.68 — session_request stops at its list's end (patch 0254)
- The session_request reader also counted the entries of a vendor_request
  list that followed it in the file. Harmless so far, fixed.

## 0.47.67 — a scene mode now applies to the photo, not only to the preview (patch 0253)
- Every capture reset the control mode to the template's, wiping the
  scene: the preview was under the scene, the photo never. With the HAL's
  HDR scene the two photos were identical for that reason. Captures keep
  the scene while one is on.

## 0.47.66 — session parameters from the device profile (patch 0252)
- A device profile can hand the HAL keys it reads only when the session
  is configured ("session_request": the JP2's MediaTek HDR mode, EIS...).
  Applied on a HAL that supports session parameters (Android 9 base and
  up); the open reply says whether they were applied. The first use: the
  HAL's built-in HDR.

## app 0.20.52 (ui 176) — a steadier level (patch 0251)
- The level's readings are filtered more (a third of a second to follow)
  and shown through a 0.05° dead band: no more trembling.

## app 0.20.51 (ui 175) — the level can be zeroed (patch 0250)
- Hold the "level" button with the phone upright: that reading becomes
  the zero (phones' sensors are a degree or two off). The settings menu's
  new "Level" column shows "zeroed" and goes back to "factory".

## app 0.20.50 (ui 174) — the level's cross runs edge to edge, the degrees at the bottom (patch 0249)
- The cross's arms are always longer than the screen; "H" and "V" are
  written at the bottom of the image, each in its axis's colour.

## app 0.20.49 (ui 173) — the level as a cross with the degrees (patch 0248)
- The level is a cross: the horizontal arm turns with the roll ("H" degrees
  at its end), the whole cross moves with the pitch ("V" degrees at its
  top), a small fixed cross marks the centre. Yellow per axis, green when
  both are straight.

## app 0.20.48 (ui 172) — a level diagnostic line in the log (patch 0246)
- Two seconds after the level is switched on, the log says whether the
  sensor module loaded and how many readings arrived (the XA2 shows no
  level; this says why).

## 0.47.65 — app 0.20.47 (ui 171) — the XA2's calibration shipped; level colours per axis; the log rotated (patch 0245)
- The Xperia XA2's lens-shading calibration (daylight and LED) ships
  with indRAW, for every XA2 variant.
- The level: the line turns yellow when the roll is level, the centre
  marks when the pitch is, both green when all is straight.
- The engine's log is rotated past 2 MB (two files at most).

## 0.47.64 — the Exif reader hardened for 32-bit phones (patch 0244)
- The reader of the HAL's Exif (0.47.59) checked its offsets in a way a
  32-bit phone could get wrong (an offset near 4 GB wrapping to a small
  one); a 1-byte thumbnail could make it read one byte too far; and a HAL
  with a full Exif directory could silently lose three of our tags. All
  three fixed and tested.

## 0.47.63 — the last review items: keys named in comments are not keys (patch 0243)
- The profile, override and calibration readers no longer mistake a key
  named inside a comment string for the key itself.

## app 0.20.46 (ui 170) — one engine start per camera switch; each format keeps its resolution (patch 0242)
- A camera switch no longer starts the engine twice (the second start
  compared the new camera against the previous one's size).
- Going 4:3 → 16:9 → 4:3 comes back to the resolution chosen in 4:3,
  not to the largest.

## app 0.20.45 (ui 169) — no RAW type or Σ speeds on a camera that has no RAW (patch 0241)
- The JP2's front and wide cameras give no RAW: on them the TYPE button
  stays on jpg, the Σ speeds are not offered, and a RAW type chosen
  before comes back with the main camera. (They answered "session has
  no raw16 stream" to a RAW shot.)
- A '# settle' line in the log says why a camera switch restarted the
  engine a second time, or kept it.

## 0.47.62 — app 0.20.44 (ui 168) — no wait on the interface when the engine stops; a cut reply stays valid JSON (patch 0240)
- Switching camera or going to sleep no longer waits (up to 2 s) for the
  engine to end: it is asked to quit, deleted when it ends, killed after
  3 s if it does not. Only closing the app waits.
- A reply too long for its buffer is replaced by a short error reply that
  says so, instead of a cut body with a flag.

## app 0.20.43 (ui 167) — lighter frame handling; zebra and peaking off the interface thread (patch 0239)
- The frame stream is parsed with a read index (no more moving the whole
  buffer for every packet), and only the newest frame of a batch is
  decoded.
- The zebra and peaking overlay is computed on a worker thread; the
  interface stays fluid with the aids on, the overlay follows at most one
  frame behind.

## 0.47.61 — the preview conversion, bounds per row (patch 0238)
- The YUV to RGB conversion of every preview frame checks its bounds once
  per row and shares the chroma work between pixel pairs. Same pixels
  exactly (tested against the former loop); somewhat less work on the
  camera thread.

## 0.47.60 — Exif: the HAL's values we know better are ours too (patch 0237)
- Seen on the first merged JPEG: the HAL's Orientation 0 (invalid) was
  kept, its DateTimeDigitized was in UTC next to our local time, its APEX
  shutter and aperture values disagreed with our real exposure. Now: an
  invalid Orientation is dropped; DateTimeDigitized, ShutterSpeedValue,
  ApertureValue, MaxApertureValue and ExposureMode (manual or auto) are
  written from what indRAW applied.

## 0.47.59 — the HAL's Exif is kept and completed, not replaced (patch 0236)
- The JPEG's Exif now keeps everything the HAL wrote (its thumbnail, so
  the system gallery no longer decodes the photo; flash, metering, scene
  type, maker note...) and only replaces what indRAW knows better:
  exposure, ISO, focal length, date, make and model, software,
  orientation, white balance. Up to now the HAL's Exif was dropped for
  eleven tags of ours.

## 0.47.58 — robustness from the code review (patch 0235)
- The engine's events never block a camera thread: the controller's pipe
  is non-blocking and larger; a state line that finds it full is dropped,
  an event waits briefly. A busy focus command no longer freezes the
  viewfinder.
- A dead interface no longer kills the engine mid-write (SIGPIPE ignored).
- Writing a DNG no longer makes a second copy of the image (100 MB less).
- Tighter checks in the DNG and Exif writers and in the calibration
  parser; error texts always valid JSON.

## app 0.20.42 (ui 166) — the deck's left and centre columns lower and spread (patch 0234)
- FOCAL, TYPE and FOCUS start lower and spread down to the bottom.
- The histogram sits a little lower; the +, shutter and − rows reach
  closer to the bottom.

## app 0.20.41 (ui 165) — the taller histogram on the JP2 too (patch 0233)
- On the phone the JP2 in 4:3 shows the compact deck (Sailfish's sizes
  are larger than on the test bench), which 0232 did not touch. Its
  histogram is now 30 % taller too, and in both decks the +, shutter and
  − rows spread down to the bottom.

## app 0.20.40 (ui 164) — a taller histogram, the notch words clear of the cutout (patch 0232)
- The words around the notch (peak, level) moved away from its edges.
- In the normal deck (the JP2 in 4:3) the histogram is 30 % taller, and
  the +, shutter and − rows below it come lower and spread to the bottom.

## app 0.20.39 (ui 163) — the shutter wheel goes past 0.5 s where the phone allows (patch 0230)
- The wheel's speeds stopped at 0.5 s (the JP2's ceiling). 0.6 and 0.8 s
  are added; they appear only on a phone whose single exposure reaches
  them (the XA2: 0.6 s). The JP2 is unchanged.

## 0.47.57 — app 0.20.38 (ui 162) — shipped device profiles; the XA2's notes (patch 0229)
- indRAW now ships a device profile per camera module (Jolla Phone 2,
  Sony Xperia XA2), recognised by make and model or by the camera's RAW
  size, so the XA2's variants share one. Your own override-hal.json only
  needs what differs: every key is read from it first, then from the
  shipped profile.
- A profile can say whether the screen has a notch (the interface stops
  guessing) and name the lens-shading calibration for every variant.
- docs/DEVICE-XA2.md gathers what was measured on the XA2.

## app 0.20.37 (ui 161) — the four words evenly spread without a notch (patch 0228)
- Without a notch, zebra / peak / level / AE are spread evenly over the
  width (no empty centre kept for a cutout).

## app 0.20.36 (ui 160) — the words over the viewfinder without a background (patch 0227)
- On a screen without a notch, zebra / peak / level / AE are now just the
  words on the image, without the grey strip.

## app 0.20.35 (ui 159) — full-width viewfinder on the XA2 in 4:3; WB readouts between their buttons (patch 0226)
- On a screen without a notch (the XA2) zebra, peak, level and AE lie over
  the top of the viewfinder, on a translucent strip.
- When the photo leaves too little room, the deck gets compact (shorter
  histogram, tighter rows, smaller left buttons, the ISO wheel a little to
  the right): on the XA2 the 4:3 viewfinder takes the full width.
- The colour temperature and the tint now sit between their + and -
  instead of against the shutter (every phone).

## docs — the questions that found the problems (patch 0225)
- docs/METHOD.md keeps Ric9k's questions and objections that found
  problems faster than the measurements (read the HAL first, compare with
  the system camera, check every request against the preview, ...), as a
  checklist for the next phone. PORTING points to it.

## 0.47.56 — no stray focus/exposure frame after a photo (patch 0224)
- After every photo the engine sent an "AF cancel" request that still had
  its startup settings (continuous AF, lens at infinity, automatic
  exposure). With the focus manual or held and the exposure manual, the
  XA2's lens left for infinity and came back, and photos came out blurred
  at random. That request now carries the viewfinder's focus and exposure,
  and is only sent when there is a scan or precapture to cancel.

## 0.47.55 — a pause before each capture, per device profile (patch 0223)
- "capture_delay_ms" in a camera's "overrides" (1-3000 ms) makes the
  engine pause before every capture, after the focus is set, for a HAL
  whose lens is still moving (the XA2, 500 ms). Absent: no pause, as
  before (the JP2).

## 0.47.54 — diagnosing a lens that moves at the shutter: template report, optional preview template (patch 0222)
- The log now records what the preview, photo and RAW requests carry (AF
  mode, capture intent, focus distance, every vendor key with its type and
  value). Read-only.
- A device profile may ask for the photo and RAW requests to be built from
  the preview template ("still_template": "preview"), to test a HAL whose
  still template refocuses at the shutter (the XA2). Off unless asked:
  nothing changes on the JP2.

## app 0.20.34 (ui 158) — the format defaults to the RAW's shape; the 4:3 list on a 16:9 RAW (patch 0221)
- The default format is now the shape of the camera's RAW (4:3 on the
  JP2, 16:9 on the XA2) instead of 4:3 everywhere: an untouched install
  takes all the sensor gives.
- On the XA2 with the v16 binaries the menu could not leave 16:9: the 4:3
  sizes were judged against the largest size of all (5984x3392) and all
  rejected. Each format's sizes are now judged against the largest of
  their own ratio.

## 0.47.53 — the probe lists what the HAL takes: level, capabilities, request and result keys (patch 0220)
- generated-hal.json / caps now report the hardware level, the declared
  capabilities, and the standard request and result keys by name, so a
  port starts by reading what the HAL accepts instead of guessing.

## 0.47.52 — the probe no longer crashes on the Xperia XA2 (patch 0219)
- `indraw probe` aborted at exit on the XA2 ("corrupted double-linked
  list") after writing its file: it created and deleted the HAL's camera
  manager three times in one process, which this 32-bit HAL does not
  survive. The inspection commands (cameras, caps, probe) now share one
  manager per process. Nothing changes for the camera session.

## 0.47.51 — a capture keeps the preview's focus for real (patch 0218)
- A capture taken with the focus "kept" (the 'once' mode after a tap, a
  locked focus) still asked the HAL for continuous AF: the capture request
  had never received the preview's focus. On the XA2 the HAL refocused at
  the shutter and undid the lock; the JP2's did not show it. The capture
  (JPEG and RAW) now copies the preview's AF mode and lens distance.

## 0.47.50 — app 0.20.33 (ui 157) — Xperia XA2: focus in RAW, the 4:3 format, one restart per camera switch (patch 0217)
- In RAW the focus hunted back and forth without end on the XA2: the RAW
  meter's frames kept asking for continuous AF while the preview was in
  manual or 'once'. The meter now follows the preview's focus.
- On a 16:9 sensor the 4:3 format gave 16:9 (it asked for "max"). Each
  format now takes the largest size of its own ratio; the menu says which
  one has the RAW's shape ("as the RAW": 4:3 on the JP2, 16:9 on the
  XA2, whose HAL gives a 16:9 readout of its 4:3 sensor).
- Switching camera started the engine twice; once now.

## 0.47.49 — DNG thumbnails: clipped highlights white, not pink (patch 0216)
- Since 0.47.36 the gallery's DNG thumbnails showed burnt-out skies and
  lamps in pink: the colour matrix read a saturated pixel (all channels at
  white) as too much red and blue. The thumbnail now applies the white
  balance, clips each channel, then the matrix: saturated is white, the
  rest of the picture unchanged. The DNG itself was never affected.

## 0.47.48 — a tighter light check, within and between lens positions (patch 0215)
- The calibration's light check (0.47.47: 20 % within a lens position) is
  now 5 % within a position -- its frames agree within 0.5 % on a steady
  setup -- and 15 % from one position's first frame to the next one's,
  which catches a light that changes between positions (the falloff only
  moves the centre 3-7 % per step). The reason says which. Host-tested on
  the levels of a real run.

## 0.47.47 — the calibration's light check is made per lens position (patch 0214)
- shading calibrate compared every frame's centre level with the very first
  frame and stopped at 20 %. The centre level can change legitimately with
  the lens position (a run stopped at the first frame of the fifth position,
  lens 16), and each capture's gains are relative to its own centre anyway. The check now compares the frames of one position with the
  first frame of that position, which still catches a light or a framing that
  moves during the measurement.

## 0.47.46 — app 0.20.32 (ui 156) — the lens-shading assistant (patch 0213)
- Settings > indRAW > "Calibrate the lens shading": at indRAW's next
  start an assistant opens over the camera. It explains what is needed
  (a diffuser, an even light, nothing but the lens moving during the
  whole measurement), lets you choose a full calibration or adding the
  present light, runs the measurement with a progress bar, and reports
  the file written or why it failed. The engine reports the outcome as a
  cal_done event; the temporary cal_meter / cal_cap traces are gone.

## 0.47.45 — the calibration's colour factors are shading only (patch 0212)
- shading_build_assemble left the light's white balance in the colour
  grids (R = 2.04, B = 1.43 at the centre). They are now normalised to
  the centre: 1 there, the corners' relative drift only. A full on-phone
  calibration now matches the 23 Oct reference within 1 % in luminance
  and colour. The on-phone capture path is validated end to end.

## 0.47.44 — the calibration reads the DNG's black level correctly (patch 0211)
- shading_read_dng read BlackLevel's tag field as the value, but the
  engine writes it as four SHORTs stored at an OFFSET: the black came out
  as 570 instead of 64, every corner went to zero and a good flat field
  was refused as "too dark". Fixed; the host test now writes BlackLevel
  the engine's way and fails on the old reader.

## 0.47.43 — focus rawD actually moves the lens (patch 0210)
- The CLI's F_RAW was 9 but the engine's IR_FOCUS_RAW is 8, so 'focus
  rawD' sent an unknown mode and the lens never moved (reached_diopters
  stayed 0); this is why the calibration captured the same position every
  time. F_RAW is now 8. (Also: 0.47.42/43 catch up the version that 0208
  and 0209 left at 0.47.41.)

## 0.47.41 — calibration progress/diagnostics visible in a terminal (patch 0208)
- In `run` mode the engine's events (cal_progress, and the temporary
  cal_meter / cal_exposure / cal_cap diagnostics) now go to stdout, so they
  appear in the terminal; before, the event channel was attached only in
  stream mode and the lines were lost. Helps find why a good flat field is
  refused by shading calibrate. No capture behaviour change.

## 0.47.40 — the calibration exposure search reaches the target (patch 0207)
- cal_find_exposure started at the lowest ISO and 1/50 s and capped its
  steps, so on a dim flat field it never reached the mid-range centre and
  the corners were refused as too dark. It now starts at ISO 400 / 1/33 s
  and scales time (to 1/2 s) then ISO (to the maximum) by the measured
  ratio: it settles the exposure in one or two tries.

## 0.47.39 — the calibration capture fixes the exposure and never overwrites (patch 0206)
- shading calibrate now fixes the exposure once (centre mid-range, not
  clipped) and uses it for every frame: the AE was drifting between
  positions and ruined the first real run (centre clipped 59 %, gains to
  the ceiling). It also refuses frames whose corners are too dark to
  measure, and a full calibration moves any existing file to previous/
  with a timestamp instead of overwriting it.

## 0.47.38 — the calibration capture command (patch 0205)
- `shading calibrate full cct=K lens=...` / `light cct=K`: the engine
  captures flat fields (RAW through a diffuser) at the given HAL lens
  values, builds the calibration and writes it to
  device-profiles/lens-shading/, merging a new light into an existing
  file. Guards against clipping, darkness and a changing field; reports
  progress. The on-phone assistant screen comes next.

## 0.47.37 — place the lens by its raw HAL value (patch 0204)
- A new focus mode `focus rawD` (engine IR_FOCUS_RAW) sets
  LENS_FOCUS_DISTANCE to the HAL's own value D, skipping the focus
  calibration: the same lens position on every phone of a model, for the
  lens-shading assistant to walk positions. Normal focus is unchanged.

## 0.47.36 — DNG thumbnails show the vignetting correction and real colour (patch 0203)
- The gallery's DNG thumbnail now applies the DNG's gain map (undosed, so
  not darkened for darktable) and the colour matrix (sensor RGB -> sRGB,
  normalised on the as-shot neutral): corners corrected, colours close to
  darktable, instead of dark corners and flat camera RGB. The coming
  assistant uses this for its before/after preview. JPEG thumbnails
  unchanged.

## 0.47.35 — the calibration builder (host side of the assistant) (patch 0202)
- `android/shading_build.h`: reads flat-field DNGs and builds the
  lens-shading calibration (luminance per lens position, colour per light),
  the host side of the coming on-phone assistant. Pure C, 16 host tests
  that write real DNGs, read them back and round-trip the file -- also
  covering the DNG writer. No change to the app yet.

## 0.47.34 — the shipped lens-shading calibration found by the app (patch 0201)
- The JP2's calibration now lives in /usr/share/indraw/lens-shading/,
  which the app's sandbox shows (it was not found from the app).

## 0.47.33 — app 0.20.31 (ui 155) — measured lens shading in the DNG (patch 0200)
- The HAL's vignetting map corrects about half the falloff, ignores the
  lens position and, under tungsten, part of the red. Flat-field series
  (two lights, seven lens positions) showed the falloff follows the lens
  position and the colour follows the light. A measured calibration per
  phone (shipped for the JP2) now gives each DNG its own map, at its lens
  position and light; pivot, strength and maximum gain in indraw.conf,
  as in Ric9k's RAWfish script. docs/LENS-SHADING.md.

## app 0.20.30 (ui 154) — release housekeeping; the zoom's double tap (patch 0199)
- The zoom's double tap (to the max / back to x1) has its own 0.15 s
  window: two deliberate taps are two steps. Quick taps on the warmth and
  tint buttons no longer lose the second one.
- Docs for the release, after an external audit: the patch series
  described as what it is (a documentary record, not replayable from an
  empty tree; patches/README.md explains the numbering), no more "future
  app" in README and DEVICE-PROFILES, this changelog's order fixed and
  its known issues listed, CREDITS typos; .mailmap gives every commit one
  identity; the app requires engine 0.47.32.

## 0.47.32 — the white flash as soon as the pose is over (patch 0198)
- "captured" (the interface's white flash, "you may move") waited for the
  photo's image: for a JPEG, after its encoding, 0.3-0.4 s after a short
  pose. It now goes at the capture result (the frame is read, before any
  encoding), or at the image should the result come late or never; once.

## app 0.20.29 (ui 153) — zoom by holding (patch 0197)
- Holding the zoom + or - zooms progressively: by proportion (x1 to x2
  as long as x2 to x4), slowly at first, faster the longer the hold. A
  tap now steps by 0.25 (twice as many steps); double tap unchanged.

## 0.47.31 — automatic focus stays inside a zoomed frame (patch 0196)
- Zoomed, the automatic focus used the HAL's default area of the whole
  sensor: it took an object just outside the frame and its sharp zone sat
  low. Only when zoomed and no point is tapped, the focus area is now the
  middle third of the zoomed frame; nothing else changes.

## app 0.20.28 (ui 152) — deleting in the gallery: the neighbour replaces it at once (patch 0195)
- The slide to the older neighbour (ui 151) went the wrong way as well:
  with this right-to-left strip, every move Silica animates by itself
  does. The older neighbour now replaces the deleted capture at once,
  without a slide; the deferred removal is gone.

## app 0.20.27 (ui 151) — the gallery strip's own drag threshold; deletion slides the right way (patch 0194)
- The menu/gallery decision now uses the strip's own drag threshold (~1
  cm): a slanting downward swipe reaches the menu first; a flat one is
  the strip's and the gallery follows the finger again. The code that
  handled sideways drags itself (ui 148-149) is gone.
- Deleting the capture shown: the strip first slides to the older
  neighbour (as a swipe), then the deleted one is removed off screen --
  no more slide in the wrong direction.

## app 0.20.26 (ui 150) — deleting in the gallery stays in place (patch 0193)
- After a deletion the gallery went back to the newest capture: its list
  was cleared and refilled. It is now updated row by row, as Jolla
  Camera's own model does: the next older capture takes the deleted one's
  place, sorting goes on; a new capture shows only if the newest was shown.

## app 0.20.25 (ui 149) — the gallery opens again from the viewfinder (patch 0192)
- 0191 broke the gallery swipe: it handed a clearly sideways drag back to
  the gallery strip mid-gesture, which the strip does not take. The
  viewfinder now slides the strip to the gallery itself (animated) on a
  clearly sideways swipe to the right; the menu's diagonal swipe stays.

## app 0.20.24 (ui 148) — menu or gallery: clearer gestures; the 16:9 bands fixed (patch 0191)
- On the viewfinder, a slanting downward swipe opened the gallery: the
  gallery strip now gets a drag only once it is clearly sideways (~0.7 cm,
  under ~27 deg); a downward drag (~0.5 cm, over ~37 deg) opens the menu.
- The 16:9 format's grey bands hid part of a viewfinder that is already
  16:9 since ui 144; now only when the preview could not take that shape.

## 0.47.30 — app 0.20.23 (ui 147) — code and comments reviewed (patch 0190)
- No behaviour change. Comments brought up to date with the last weeks'
  changes (one page with a gallery/camera strip, a viewfinder at the
  photo's proportions, the 2.1 s white-balance press, the capture's 3A
  waits, the Σ preview); unused leftovers removed (a page alias, two QML
  properties, an unused gallery function, an unused CLI type); the
  gallery's id no longer says "page"; the CLI's afwait help, README,
  USER-GUIDE and ENGINE.md corrected.

## 0.47.29 — no precapture without flash (patch 0189)
- A photo taken while the auto exposure was settling sent a precapture,
  the flash's metering sequence: on the JP2 it outlasts 0.5 s and its state
  lingered, so the next shots each sent another and waited 0.5 s (five in
  a row in a log, ~0.87 s each). Without flash there is no precapture any
  more: the photo waits at most 0.5 s for the continuous exposure to
  settle, after cancelling a lingering precapture. With flash, unchanged.

## 0.47.28 — the 1.5 s wait only for the flash (patch 0188)
- Since 0.47.21 every photo taken while the automatic exposure was still
  settling (just after moving) could wait up to 1.5 s (1.2 s seen). The
  1.5 s wait is now only for a flash shot (its metering needs it) or an
  AF scan asked for; otherwise at most 0.5 s.

## app 0.20.22 (ui 146) — the gallery from the start; the vendor tests written down (patch 0187)
- The gallery said "No captures yet" until a photo was taken: its folders
  were given after its creation. Given at creation now; a file still
  being read no longer aborts the list (retried a moment later).
- docs/VENDOR-TESTS.md: every chipset-key test with numbers (3DNR: echoed,
  no effect, the maze is elsewhere; MFNR: works, small gain, off in good
  light in auto) and the override line to keep the MFNR in auto.

## app 0.20.21 (ui 145) — the deck scales to the screen (patch 0186)
- On a small screen the deck is scaled down as a whole (to 50 % at most)
  so the viewfinder keeps its full width; only beyond that does the
  viewfinder shrink. Black, not white, beside a narrower viewfinder.

## 0.47.27 — app 0.20.20 (ui 144) — any screen, any photo shape (patch 0185)
- The preview takes the photo's proportions (the XA2's 16:9 photos got a
  square preview, stretched), and the viewfinder follows them.
- On short screens the viewfinder shrinks (centred, black at the sides)
  so the deck keeps its minimum height: on the XA2 the shutter row was off
  the screen and the captions overlapped.

## 0.47.26 — crops in active-array coordinates (patch 0184)
- The engine added the active array's offset to its crops and regions;
  Camera2 counts them from the active array's corner. The full-frame crop
  overflowed by the offset: harmless on the JP2, a fatal device error on
  the Xperia XA2 (offset 8,8), whose viewfinder froze after two frames.

## 0.47.25 — 32-bit: float arguments reach the engine right (patch 0183)
- On 32-bit ARM, Android (armeabi-v7a) and Sailfish (armv7hl) pass float
  arguments in different registers: every engine call with a float
  (focus, zoom, loupe, tap, view window, vendor keys...) read garbage, and
  "focus continuous" crashed on the Xperia XA2. The engine's entry points
  now use the hard-float convention on 32-bit ARM.

## build: the app always builds from clean; binaries checked (patch 0182)
- 0181 relied on `sfdk build --clean`, which did not remove the app's
  in-tree objects and binary: the armv7hl package still carried the
  aarch64 binary. The app's spec now deletes them before every build (two
  C++ files: seconds); build-all.sh checks with file(1) that the CLI and
  the app are binaries of the target's architecture, and stops otherwise
  (no check possible = stop). The architecture memory of 0181 is gone.

## build: switching architecture is safe (patch 0181)
- Building for armv7hl after aarch64 packaged the app's stale aarch64
  binary (its in-tree build was not cleaned): the package asked for
  ld-linux-aarch64. build-all.sh remembers the last architecture and
  rebuilds the app from scratch when it changes, then checks that an
  armv7hl package asks for no 64-bit library.

## 0.47.24 — the 32-bit engine package installs (patch 0180)
- The armv7hl engine package required `libhybris-common.so.1()(64bit)`,
  the 64-bit name, and could not be installed (Xperia XA2). The
  dependency now takes the right name per architecture; check-host.sh
  refuses a "(64bit)" dependency outside an `%ifarch aarch64` block.

## 0.47.23 — app 0.20.18 (ui 143) — vendor-key diagnostics; the flash white withdrawn (patch 0179)
- The probe's generated-hal.json gains `session_keys` and `vendor_chars`
  (the values of the vendor characteristics); after applying
  `vendor_request` entries, the log says what the preview results report
  for each key -- to tell "accepted" from "applied".
- The flash's own white (`flash_neutral`) is removed, code and docs: not
  mature enough, not a priority. An override still containing
  flash_neutral is now refused as an unknown key: remove the line.
- The gallery's "n / N" counter sits below the notch.

## app 0.20.17 (ui 142) — gallery and camera in one strip, as Jolla Camera (patch 0178)
- Leaving the gallery was two movements (the strip, then a page
  transition catching up). As in Jolla Camera, the camera page now holds a
  PagedView [ gallery | camera ]; the gallery is a nested PagedView, right
  to left, newest next to the camera: one continuous movement either way.

## app 0.20.16 (ui 141) — commented references; peaking and zebra out of the menu (patch 0177)
- `indraw.conf.example` and `override-hal.example.jsonc`, every key with its
  function, values and limits, installed next to the real files at each
  start (the real files untouched).
- The Peak and Zebra tweak columns left the menu (indraw.conf keeps them).

## 0.47.22 — (the flash's own white: withdrawn in 0.47.23)
- The DNG's AsShotNeutral follows the applied gains whenever the shot's
  AWB is off (kept). The flash white itself was removed in 0.47.23.

## app 0.20.15 (ui 140) — start in manual focus and exposure (patch 0175)
- Settings > indRAW > "Start in manual focus and exposure": the app opens
  with the focus manual at the last distance and both wheels pinned at the
  last ISO and speed (advanced mode; off by default).

## app 0.20.14 (ui 139) — the gallery as in Jolla Camera (patch 0174)
- No more loop: a strip that opens on the newest capture, older ones to the
  left, ending at the oldest; from the newest, a right-to-left swipe returns
  to the camera; "3 / 27" says where you are.

## app 0.20.13 (ui 138) — the menu trimmed (patch 0173)
- Warmth, Tint (now on the deck) and AF wait (the engine's 1.5 s default)
  left the menu, with their code.
- Scenes only when Settings > indRAW > "Scenes in the camera menu (not yet
  implemented)" is on (off by default), then in simple mode too.
- The menu opens on a 20 % swipe (40 % before).

## 0.47.21 — app 0.20.12 (ui 137) — the measured white, refined (patch 0172)
- The press is 2.1 s; the zone being measured is drawn until the measure
  is over.
- At start a measured white is kept only if "Keep previous manual WB" is
  tapped within 6 s (a bar runs out); otherwise automatic again.
- The DNG records the white actually applied (AsShotNeutral from the
  applied gains when the white is manual), and EXIF WhiteBalance says
  manual, in the JPEG and the DNG.
- The capture's 3A wait defaults to 1.5 s (flash metering needs it).

## 0.47.20 — app 0.20.11 (ui 136) — measure the white on a neutral object (patch 0171)
- A 3 s press on the kelvin or tint readout measures the white on the
  circle's spot (or the middle) from the RAW data and keeps it: neutral
  JPEGs whatever the HAL's AWB thinks. The ± steps apply on top; a double
  tap returns to automatic; kept across restarts with a banner (Keep /
  Auto). Engine: `wb pick`, `wb fixed`, event `wbpick`.

## 0.47.19 — the kelvin readout the DNG way (patch 0170)
- The colour temperature beside the shutter used one calibration matrix
  and read 1000-1300 K too low away from daylight. It now interpolates the
  sensor's two matrices in 1/T and solves for the self-consistent
  temperature, as the DNG SDK and darktable do (`android/colortemp.h`,
  tested on PC against five JP2 DNGs).
- UI-NOTES and USER-GUIDE: the HAL's automatic white balance measured --
  it corrects only part of the way (daylight cold, lamps warm).

## 0.47.18 — the red Σ message only at the real limit (patch 0169)
- "too dark to measure" appeared whenever the Σ preview frame was dark, so
  also in manual when the photo itself was just under-exposed (Σ4s at
  ISO 100 in a dim room). It now appears only at the limit: the frame at
  0.5 s and the maximum ISO, its signal still in the noise. Below that,
  a dark frame is a dark photo (the ± number, or the loop, handle it).
- USER-GUIDE: the message's wording, and two words (DN, RAW16). ROADMAP:
  the boosted-preview idea in reserve (blue frame, trust the histogram
  only), with the analog-gain test as its prerequisite.

## 0.47.17 — fix: the camera hung at start (patch 0168)
- 0.47.15 locked g.lock around the colour-temperature update inside a
  block that already holds it; the lock is not reentrant, so the preview
  callback deadlocked at the first frame, and everything else with it
  ("Starting camera..." forever). The extra lock is removed;
  `check-host.sh` now refuses a g.lock taken while held
  (`scripts/lockcheck.awk`).

## 0.47.16 — app 0.20.10 (ui 135) — the Σ viewfinder at a usable rate (patch 0167)
- A Σ preview ran at the 0.5 s ceiling: 2 frames/s at best, so metering
  and focusing crawled. The controller now previews one frame of the same
  exposure as short as a target rate (10/s, `sigmaPreviewFps` in
  indraw.conf), raising the ISO; lengthened only at the maximum ISO; past
  the ceiling the deficit is compensated in the measure. Messages under the
  Σ banner: preview slowed (ISO limit), darker than the photo (histogram too
  dark), too dark to measure. User guide: how and why, and the hardware's
  limits (0.5 s, the sensor as the only meter, the analog gain unverified).

## docs (patch 0166)
- ROADMAP: the lens-shading question -- the HAL's map half-corrects the
  JP2's vignetting (measured), the earlier RAWfish calibration format to
  reuse, and the measurements at several focus distances to make before
  deciding whether to replace the HAL's map, combine with it, or not.

## 0.47.15 — engine audit fixes (patch 0165)
- From an external review of `indraw.c`: the preview-frame save used the
  caller's path buffer after the lock (a timeout could free it) -- the
  engine keeps its own copy; `stream_fd` is protected by one lock
  (`out_lock`) everywhere, so a `stream stop` cannot race a writer; a DNG
  stack now emits `written kind=dng` like any photo; the command-side
  writes of the spot, zoom, loupe, EV and the colour temperature are under
  the lock the readers take; the locks are statically initialised; the
  focus-calibration table is validated (increasing distances, non-
  decreasing lens values) instead of interpolated blindly; a stack aborted
  before its first frame fails instead of taking one; stale "no libm"
  comments corrected; `caps proto=1` versions the protocol; the meter's
  need for a stream is documented.

## 0.47.14 — the histogram covers the displayed window exactly (patch 0164)
- 0.47.13 restricted the raw histogram to the HAL's crop, which on the JP2
  (centre-only cropping) is the whole image once the loupe is off centre.
  The controller now tells the engine the window actually displayed
  (`viewwin`); both meters cover that.

## 0.47.13 — app 0.20.9 (ui 134) — histogram of what is shown, the spot highlighted (patch 0163)
- The raw histogram covers the area the viewfinder shows (zoom, loupe),
  like the jpg one. With a reference circle, both meters add the spot's
  own histogram, drawn as bright outlines over the whole picture's.

## 0.47.12 — app 0.20.8 (ui 133) — fine white balance, kelvin and tint readouts (patch 0162)
- Three + and three − across the centre column, coloured by what they do:
  warmth (orange / blue, 5 % red/blue), zoom (white), tint (magenta /
  green, 5 % green); a tap shows what it added. Beside the shutter, the
  white balance in force in real terms (kelvin, Duv), estimated by the
  engine from the camera's colour gains and the sensor's calibration
  (`cct=` / `duv=` in the `ae` line); a readout is coloured while its shift
  is not zero, a double tap on it resets the shift. Step sizes in
  `indraw.conf` (`fineWarmthStep`, `fineTintStep`).

## 0.47.11 — app 0.20.7 (ui 132) — photos upright on the ground (patch 0161)
- Photos were oriented relative to the phone (the sensor mounting only).
  The app reads the phone's physical rotation from its tilt sensor,
  independent of the screen's orientation lock, and the engine tags each
  JPEG and DNG with the mounting plus that rotation.

## build: 32-bit targets (patch 0160)
- The engine's library is built for the ABI of the sfdk target (arm64-v8a
  for aarch64, armeabi-v7a for armv7hl) and installed in droid-hybris'
  lib64 or lib accordingly; the app spec no longer pins an architecture
  (`BuildArch`), which refused the armv7hl target.

## 0.47.10 — app 0.20.6 (ui 131) (patch 0159)
- The Σ progress no longer showed: the app marked "stacking" when it sent
  the stack command, which the controller now issues. The engine announces
  the start (`stack_start n=`) and the app follows that.
- "jpg yuv" is hidden from the TYPE button until it exists.
- Settings: "Mode at start" (as left / simple / advanced).

## 0.47.9 — app 0.20.5 (ui 130) — clean-up, the architecture complete (patch 0158)
- Step 7 of ARCHITECTURE.md: the interface no longer reads the live AE /
  AF states or the lens position (unused since steps 3-4); the page's
  header describes the split. The controller layer is complete: the
  engine talks to the sensor, the controller behaves, the interface shows.

## 0.47.8 — app 0.20.4 (ui 129) — the shooting decisions in the controller (patch 0157)
- Step 6 of ARCHITECTURE.md: a shutter press is a request (`sh ...`); the
  controller decides the capture lines (held pair, manual distance, stack,
  burst, bracket, JPEG processing), host-tested; the CLI runs them. Same
  behaviour for the user.

## 0.47.7 — app 0.20.3 (ui 128) — the geometry in the controller (patch 0156)
- Step 5 of ARCHITECTURE.md: zoom, loupe, windows, the gesture rule, the
  loupe throttle and the taps' mapping run in the controller (`ctl_view.c`,
  host-tested); commands `vw ...`, event `vwstate`. The page keeps the view
  rotation and the drawing. Same behaviour for the user.

## 0.47.6 — volume-key focus steps without lag (patch 0155)
- Each volume-key step waited for the lens before the next (the engine's
  live focus command waits for a manual distance), so steps piled up: the
  display jumped by groups and ran on after the key was released. The
  controller now updates its state first and sends the distance with
  `nowait`; the engine returns at once.

## 0.47.5 — app 0.20.2 (ui 127) — the focus in the controller (patch 0154)
- Step 4 of ARCHITECTURE.md: the focus behaviour (modes, the tap's search
  and its guards, hold, manual distance and steps, late lock, the exposure
  couplings) runs in the controller (`ctl_focus.c`, host-tested); commands
  `fc ...`, event `fcstate`. The page keeps the geometry and draws the
  circle it is told. Same behaviour for the user.

## app 0.20.1 (ui 126) (patch 0153)
- In the loupe the exposure was held for as long as it was open: the wheels
  looked blocked, AE included. It is held only while a finger moves the
  loupe; still, everything works as outside it.

## 0.47.4 — controller: captures without the lock; vfmeter tagged; vendor keys (patch 0152)
- A photo, a burst or a stack ran under the controller's lock: the engine's
  events (a stack's progress) would have stalled behind it, and the engine's
  event writes with them. Long commands now run with the lock released; the
  controller defers its own engine commands meanwhile and re-applies after.
- `vfmeter` carries the pair its frame was exposed with (from the frame's
  result, by timestamp); the exposure loop's settle check uses it instead of
  the last live values, which could lag by a frame.
- `override-hal.json` gains `vendor_request`: chipset keys (tag, type,
  value, on preview/photo/all) set at open and logged; the engine does not
  interpret them (`indraw_vendor_set`). ROADMAP: the JP2's keys, the 3DNR
  mode (`0x80010000`) first to try on the viewfinder maze.

## 0.47.3 — app 0.20.0 (ui 125) — the exposure in the controller (patch 0151)
- Step 3b of ARCHITECTURE.md: the controller drives the exposure (from
  `caps`, `ae`, `meter`, `vfmeter`; commands `xp ...`; state `xpstate`),
  issuing the engine's exposure / ev / meter lines itself (logged `c> `).
  `ExposureLogic.qml` is a view. Same behaviour as before for the user.

## 0.47.2 — the controller's exposure logic, with tests (patch 0150)
- Step 3a of ARCHITECTURE.md: `sailfish/ctl_exposure.c`, the exposure
  behaviour ported to C as pure logic, and its host test (seven rules on a
  simulated scene with HAL latency), run by `check-host.sh`. Compiled into
  `indraw`, not wired yet: nothing changes on the phone.

## app 0.19.14 (ui 124) (patch 0149)
- Back to focus first: the search runs with the HAL's own AE on (its AF
  relies on its 3A; under an app-imposed pair it was slow and "not sure"),
  the app's spot measure follows the search. Fast and sure again; the
  "light first" order of ui 118-123 is dropped, with its machinery.

## app 0.19.13 (ui 123) (patch 0148)
- A tap was slower than before: the focus search waited for the exact
  exposure. It now starts as soon as the point is within one stop of its
  target (usually at once); the fine corrections follow the search.

## app 0.19.12 (ui 122) (patch 0147)
- The circle went dashed after one second and never turned solid when the
  focus search took longer (dark scenes): the search limit followed the
  photo's "AF wait" setting (300 ms). It is 3 s now, and a lock reported
  after it still counts (lens held, circle solid).

## build fix (patch 0146)
- The NDK link of `libindraw.so` lacked `-lm` (pow() in the new viewfinder
  meter): `build-android.sh` links it; `check-host.sh` now refuses a libm
  call without it.

## 0.47.1 — app 0.19.11 (ui 121) — the viewfinder meter in the engine (patch 0145)
- Step 2 of ARCHITECTURE.md: the engine measures the viewfinder frame it
  sends (mean, clipping, histogram, the tap's spot through the frame's real
  crop) and sends `vfmeter`; the app's item stops measuring. One spot for
  both meters, set by the tap. Nothing changes for the user.

## docs (patch 0144)
- Why the raw histogram sits left of the jpg one with a bigger low bar
  (USER-GUIDE, UI-NOTES).

## app 0.19.10 (ui 120) (patch 0143)
- The camera starts in continuous focus ("auto"), no longer in "once".

## 0.47.0 — the controller layer, step 1 (patch 0142)
- A controller layer in the `indraw` process (`sailfish/controller.c`,
  docs/ARCHITECTURE.md): the engine's state events go to it through a pipe
  (`indraw_set_event_fd`) and it forwards them to the stream (`indraw_emit`,
  in order with the frames). Pass-through for now; the camera's behaviour
  will move there piece by piece. Host test in `check-host.sh`. Nothing
  changes for the interface.

## 0.46.7 — focus window in the loupe (patch 0141)
- The focus region shrank with the loupe like the exposure spot (1.5 % of
  the field at 4x): too little texture for the contrast AF, which locked 2
  taps out of 20 in the loupe (nearly every one outside it). The AF region
  now keeps at least 5 % of the sensor's short side; the exposure spot stays
  as small as the loupe shows.

## 0.46.6 — app 0.19.9 (ui 119) — no exposure after the search starts; raw viewfinder beat (patch 0140)
- The circle still failed to settle at times: the light phase's time limit
  could let a last exposure correction land during the focus search. The
  light phase is now ended for good before the search; a search without an
  outcome shows the circle dashed.
- In raw, the viewfinder beat slightly: the meter's preview frames came with
  the photo crop and no "fcrop". They now carry the preview crop and are
  announced like the others.

## 0.46.5 — app 0.19.8 (ui 118) — light then focus; the spot on the scene (patch 0139)
- A tap: light first (spot measure / loop settles), then the focus search,
  with no exposure change during it; a stale "locked" from the previous tap
  is no longer taken for the result.
- The spot is a box on the scene (12 % of the frame: small in the loupe),
  mapped into each viewfinder frame through its real crop; the RAW meter's
  box follows the loupe too; both sampled densely (about a thousand points).
- The loupe's sharp crop was never sent when the finger lifted (a handler
  read a value not yet updated).

## 0.46.4 — app 0.19.7 (ui 117) — focus first, then light; spot EV; smooth loupe (patch 0138)
- In "once" the circle seldom turned solid: the spot exposure's corrections
  replaced the preview request during the focus search and interrupted it.
  The light is now measured when the search has ended (or after the AF wait).
- Manual and semi-automatic exposure: the circle shows the point's light as
  an absolute EV (ISO 100) above it. The engine's caps line gives the lens
  `aperture`.
- The loupe pan was jerky: the HAL crop changed on every move (each change
  replaces the preview request). During a pinch or a pan the view is now a
  display enlargement; the HAL crops for real detail when the finger lifts.

## 0.46.3 — app 0.19.6 (ui 116) — circle and loupe pan (patch 0137)
- The circle no longer turned solid: it followed the live AF state, which
  goes back to 0 when "focus hold" turns the AF off right after the lock.
  It now shows the outcome of its own search, latched.
- Zoom/unzoom jumps while panning the loupe: the display magnification was
  computed from the crop requested, which the HAL applies a few frames late.
  The engine now sends each frame's real crop ("fcrop"), and the viewfinder
  item maps the requested view window into it.

## docs (patch 0136)
- ARCHITECTURE.md: one term, "interface", made precise as "interface (QML)"
  or "interface (C++ item)" where it matters. ROADMAP.md: the RAWfish merge
  is a "possible shape".

## docs (patch 0135)
- `docs/ARCHITECTURE.md` (proposal): engine (sensor commands and returns),
  a new controller layer in C (the camera's behaviour, testable on a PC),
  the interface (display and gestures); where each function goes, the
  protocol that follows, the migration steps.

## app 0.19.5 (ui 115) — no gallery from the loupe (patch 0134)
- A left-to-right drag in the loupe opened the gallery: our handler ignored
  it, but Silica's own back gesture (to the gallery page) took it. The
  page's back navigation is off while the loupe is open or a stack runs,
  and the loupe's pan cannot be stolen.

## app 0.19.4 (ui 114) — level in every orientation (patch 0133)
- The level's line turned green only in normal portrait: the roll was
  wrapped to -180..180, so a level phone in the three other orientations
  read +-180. A horizon has no direction: the roll is folded to -90..90.

## 0.46.2 — app 0.19.3 (ui 113) — consistency audit (patch 0132)
- A spot held in automatic is now what photos and bursts are taken with
  (they were shot in the HAL's AE, unlike the viewfinder).
- Flash with a held or manual pair: "auto" does not fire (it used to count
  as "on"), "on" fires a single flash.
- Scenes: a tap does not start the spot measure under a scene; a scene
  switched on stops it.
- A (re)started engine clears the circle and the loupe (the new session
  knows neither); simple mode returns to automatic exposure; the landscape
  AE button removes the circle like the portrait one.

## 0.46.1 — app 0.19.2 (ui 112) — loupe pan on centre-only HALs (patch 0131)
- The pan in the loupe did nothing when the HAL only crops around the centre
  (SCALER_CROPPING_TYPE CENTER_ONLY: it re-centres every crop). The engine
  reports `crop_free` in caps; on such a HAL the app keeps the HAL crop just
  wide enough to contain the panned view and pans in the display.
- Docs: the circle never disappears by itself (user guide, notes).

## app 0.19.1 (ui 111) — the circle measures the light again (patch 0130)
- In automatic exposure the exposure was set on the whole frame, not the
  circle: the HAL's AE region + lock (0125) did not work on the JP2 (the
  lock came before the region had acted). The app now measures the spot and
  sets the exposure itself, in at most four corrections, then holds the
  pair until the circle goes. Every circle measures the light (in full
  manual, the +/- indicator reads the spot).

## 0.46.0 — app 0.19.0 (ui 110) — the loupe shows real detail (patch 0129)
- Engine: `loupe FACTOR CX CY` crops the PREVIEW request only (photos, RAW,
  meter keep the photo crop); the zoom now sets the photo crop on photos and
  the zoom+loupe crop on the preview. `tap` takes a point of the viewfinder
  frame and maps it to the sensor (it ignored the zoom before); the region
  is 6 % of the frame, so smaller in the loupe.
- App: one coordinate model (zoom, frame and display windows); the loupe is
  a real crop up to the HAL's maximum, display enlargement beyond; while it
  is open the exposure does not move unless a tap meters a spot in it.
  Pv noise / Pv sharpen renamed Vf noise / Vf sharpen.
- `docs/ROADMAP.md`: the viewfinder maze (temporal noise reduction, a
  hypothesis to verify), viewfinder transport steps, RAW viewfinder set aside,
  merge with RAWfish, measurements to make.

## 0.45.3 — app 0.18.6 — complete Requires (patch 0128)
- App: one Requires per QML import (Silica, Gallery, QtQuick, QtQuick.Window,
  folderlistmodel, QtSensors for the level, Nemo Policy / Configuration /
  KeepAlive).
- Engine: libhybris-common and droidmedia, both loaded with dlopen (rpm could
  not see them); droidmedia only starts the binder thread pool.

## app 0.18.5 (ui 109) (patch 0127)
- The FOCUS mode "circle" is now called "once" (one scan where you tap, then
  held); the reference circle itself keeps its name.

## 0.45.2 — app 0.18.4 (ui 108) — preview processing separate (patch 0126)
- Engine: `preview NOISE EDGE` sets the viewfinder only; photos never inherit
  it (their modes come with each command, else the still-capture template).
  A photo choice (no NR, sharpening) had made high-ISO previews a maze.
- App: Noise / Sharpen are the photos'; new optional columns Pv noise / Pv
  sharpen (Settings > Apps > indRAW > "Preview processing in the camera
  menu"), default fast NR and no sharpening. "Denoise" renamed "Σ denoise".

## 0.45.1 — app 0.18.3 (ui 107) — a tap holds focus and exposure (patch 0125)
- The 0119 "program AE" on the spot is removed: it replaced the preview
  request several times a second, which made the preview jerky and let the
  focus wander after a tap. Now a tap fixes both, once each: when the scan
  has locked the lens is held at its reported position (`focus hold`, AF
  off -- the same rule now freezes the lens for stacks), and when the HAL's
  AE has converged on the point the exposure is held (`aelock on`). Freed
  by a tap on the circle, AE, or a focus-mode change.

## app 0.18.2 (ui 106) (patch 0124)
- The icon at every launcher size (86, 108, 128, 172 px), generated by
  `scripts/make-icon.py` (only 86 px was installed: the launcher kept showing
  the old icon at its own size).
- Flash: the first column of the menu, kept in simple mode.

## icon (patch 0123)
- New app icon: the dark Sailfish-style tile filled with coarse sensor noise,
  crossed by three analog interference lines.

## docs (patch 0122)
- UI-NOTES (menu columns as they are now, status since ui 78), README (meter
  spot, caps fields), ENGINE (tap and the spot meter), USER-GUIDE (exposure
  following the circle) brought up to date.

## 0.45.0 — app 0.18.0 (ui 105) — burst and flash (patch 0121)
- Engine: `burst NAME [n=2..8]`: the frames in ONE capture call, back to
  back at the sensor's pace, one exposure (manual, or the live automatic pair
  frozen), images queued by the JPEG callback and copied out at once, Exif
  per file. The Bracket x3 uses it in JPEG.
- Engine: `flash off|auto|on|torch`: in automatic exposure the flash-aware AE
  modes and a precapture (pre-flash metering, not skipped when a flash may
  fire); with a manual exposure a single flash on the shot's request only;
  torch on the preview. caps reports `flash`. The app has a Flash column
  when the camera has a flash.

## app 0.17.2 (ui 104) (patch 0120)
- Focus cal.: a distance or "clear" is armed by a first tap (blinking red)
  and carried out by a second tap on it within 4 s.
- Bracket: a "x3" value -- three shots in a row at the same exposure,
  queued at once.

## 0.44.6 — app 0.17.1 (ui 103) — spot metering that stays (patch 0119)
- The light measured at the tapped circle used to drift back to the average
  (the HAL drops its AE region): the meters now measure only the spot while
  the circle stays, and in automatic mode the app keeps the exposure on it
  itself (program AE). Removing the circle hands back to the HAL.
- Grid: the menu owns the state; it no longer stays on.
- Level: the horizon line turned the wrong way (roll sign).

## 0.44.5 — app 0.17.0 (ui 102) — level, manual focus from a tap, user guide (patch 0118)
- Engine: the `ae` line carries `focus_real`, the lens position as a real
  distance (the focus calibration inverted); a single calibration point now
  shifts the whole 1:1 scale instead of fixing every distance to one value.
- Manual focus: a tap focuses once on the point, then the distance found
  becomes the manual distance (shown under the circle); the volume keys
  continue from it.
- The "grid" notch button became "level" (horizon line from the
  accelerometer, roll and tilt, green when level); the grid is a menu column.
- `docs/USER-GUIDE.md`: the app screen by screen, the menu, the gallery, the
  Settings page, focus calibration, files.

## docs (patch 0117)
- `docs/ENGINE.md`: the engine's complete command reference -- invocation,
  every stdin command with its options and limits, override fields with
  ranges, exit codes, examples. The CLI's stale header comment (old option
  syntax) now points to it.

## 0.44.4 — override file help (patch 0116)
- `indraw probe` writes the starter `override-hal.json` with one block per
  camera and the HAL's own values beside the fields that correct them
  (`_hal_...`, reference only); not a copy of `generated-hal.json`, on purpose.
- The engine checks the override file at every open: unknown keys and
  out-of-range values are logged and ignored, a structurally broken file is
  ignored as a whole, a camera without a block is reported.

## app 0.16.5 (ui 101) — menu swipe up (patch 0115)
- The upward swipe that should close the settings menu never worked (its
  MouseArea gave the press away). It now closes the menu from anywhere,
  the column band included.

## docs (patch 0114)
- `docs/STACKING.md`: dark frames at 0.5 s, cold and right after a Σ4m: within
  2 % of the 1/100 s readout noise, no thermal offset -- dark current is
  negligible on the JP2 at Σ durations; data and plot in `docs/measurements/`.

## docs (patch 0112)
- `docs/STACKING.md`: the JP2's dark-frame measurement (no dual conversion
  gain; ISO 100-400 at the readout floor) and what it means for Σ shots;
  the plot and data in `docs/measurements/`.

## 0.44.3 — stacking reviewed (patch 0111)
- Formulas checked against the literature and a simulation (`docs/STACKING.md`):
  brightness exact; a stack is a sum, as it must be; its noise is sqrt(n)
  better than one pushed frame and worse than a true long exposure in dim
  light (one read noise per sub-frame -- the 1/2 s ceiling is the limit).
- Each frame's own black level (the HAL's dynamic black) is removed, not n
  times the first one: shadows stay neutral when the black drifts.
- 16-bit headroom: data and DNG levels scaled by 64 on a 10-bit sensor, so the
  fractions from the per-frame blacks and the denoise division are kept.

## app 0.16.4 (ui 100) (patch 0110)
- "Σ stacking: experimental feature" on the viewfinder while a Σ speed is
  selected.

## 0.44.2 — app 0.16.3 (ui 99) — stacks as one long exposure (patch 0109)
- 0.44.1 kept the light of all sub-frames but left the DNG's black and white
  at single-frame values over pixels holding base pedestals and up to base x
  white: lifted, noisy, magenta images. The stack is now written as one long
  exposure: the extra pedestals removed per CFA phase, single-frame black and
  white, highlights clipped at white; noise profile adjusted.
- Gallery: "DNG quick preview" banner on the top edge of the picture, with
  "Deleting this preview deletes the original DNG file."

## build fix (patch 0108)
- `app/rpm/indraw.spec`: the Settings page's two `%files` lines had landed
  in `%install` (the build ran them as commands). `scripts/check-host.sh`
  now refuses a bare path line in any spec's `%install`.

## 0.44.1 — build fix (patch 0107)
- `sailfish/indraw.c` lacked `<unistd.h>` (access, unlink): the build
  engine's strict C11 failed. `scripts/check-host.sh` now compiles the CLI
  with the same flags as the real build, so this cannot slip through again.

## 0.44.1 — app 0.16.2 (ui 98) — stacks really accumulate (patch 0106)
- Stacks divided the sum by the number of frames: every Σ duration came
  out as bright as one 1/2 s sub-frame (only the DNG's white level said
  otherwise). The sum is now kept -- divided only by the denoise factor and
  by the power of two that fits 16 bits, with black/white scaled to match --
  so brightness follows the duration, and the DNG preview shows it.
- A Σ speed forces raw and greys TYPE; the previous type returns after.
- Shutter wheel: the Σ is its own small label and the shot time a tight
  second line, both fitted to the column (the Σ and the end of the brackets
  were cut off).

## app 0.16.1 (ui 97) — fine focus steps, device tuning in Settings (patch 0105)
- Manual focus in 0.05-diopter steps (200 over the range, auto-repeat when
  the key is held) so a calibration point can be placed precisely.
- Settings > Apps > indRAW: "Device tuning" -- a switch showing the Focus
  cal. column, and a button that asks the app to write the device profile
  files (probe) at its next start, with the date once done.

## 0.44.0 — app 0.16.0 (ui 96) — focus calibration assistant, housekeeping (patch 0104)
- "Focus cal." menu column: focus on a target at a known distance, tap the
  distance, the lens value is recorded (`calib D` command,
  `focus-calibration.json` merged over the override file, applied at once).
  No diopters to know.
- `indraw probe` writes a starter `override-hal.json` when there is none;
  `docs/GENERATED-HAL.md` explains every field of the generated file.
- README: the "atomic" principle stated with its two deliberate exceptions
  (the app's own priority loop, scene modes); the former name recorded.
- Author Ric9k in LICENSE, README, CREDITS, specs; scripts reviewed (dark
  analysis script translated to English, the sweep calibration marked as
  the advanced method).

## app 0.15.1 (ui 95) — gestures (patch 0103)
- With the loupe on, one finger pans the window (the swipes being off, the
  finger is free); two fingers are only needed to zoom.
- A swipe interrupted by a second finger (or the system) left the menu
  half-way: `open = false` on an already closed menu moved nothing. The menu
  now always settles through one function. No gallery/menu swipes while the
  loupe is on; a pinch start cancels a swipe in progress.
- Two-finger gestures are classified once per gesture (spread > 15 % =
  zoom about the centre, else centre moved = pan): the per-update 1 % test
  had made pans look like zooms.

## 0.43.0 — app 0.15.0 (ui 93) — DNG previews from the raw, output settings (patch 0102)
- The DNG's NAME_vf.jpg preview is rendered by the engine from the DNG's own
  pixels (half size, as-shot white balance, gamma), via a PPM the app turns
  into the JPEG rotated upright -- a stack's preview now shows the stack's
  exposure, not the last viewfinder frame. Output settings (prefix, JPEG
  and DNG folders) on indRAW's page of the system Settings app (dconf
  `/apps/indraw/output`; engine options `--jpgdir` / `--dngdir`); the
  gallery merges the two folders. `docs/PORTING.md`. Previews live in the hidden
  `.previews/` subfolder (with a `.nomedia` marker): the system gallery never
  shows them as photos;
  share/details/delete in indRAW's gallery act on the DNG itself.
- Gallery: a DNG page without a preview could not bring the overlay back
  (no viewer to tap), so the page could not be left; fixed.

## 0.42.0 — app 0.14.0 (ui 88) — stacking (patch 0101)
- Shutter wheel: Σ positions 1 s .. 8 min (photo-equivalent) with the real
  shot time in brackets, measured per device; the Σ drawn smaller.
- Stacks freeze the lens at its reported position (AF OFF used to send it
  to infinity: the focus changed mid-stack) and no longer scan.
- Progress events, black viewfinder with frames/time left, abort that keeps
  the frames so far (SIGUSR1 or the app's stdin closing); engine kept alive
  through inactivity only while a stack runs. "Denoise" setting (2x/4x
  frames divided out). Frame cap 4096. The screen stays on during a stack
  (Nemo.KeepAlive) and every control but the abort is locked.

## 0.41.0 — app 0.13.0 (ui 87) — robustness review (patches 0097–0099)
- Exif Make/Model are the device's own Android product properties.
- The HAL's JPEG is handled as untrusted bytes (segment lengths bounded);
  AWB gains copied under lock; settings and `simple.json` parsed and clamped;
  engine auto-restart (three tries, growing pause); sandbox permissions
  reduced to Camera, Pictures, MediaIndexing.
- `docs/ROBUSTNESS.md`, `docs/BUILDING.md`, `scripts/build-all.sh`.

## 0.40.x — Exif in the JPEGs (patches 0095–0096)
- The engine writes its own Exif APP1 into the HAL's JPEG: exposure and ISO
  as the HAL reported applying them, focal length and 35 mm equivalent,
  f-number, dates, orientation copied from the HAL's tag, Exif/Flashpix
  versions, colour space; placed after the JFIF segment.
- The app declares MediaIndexing so captures are indexed without delay.

## 0.39.0 — app 0.12.0 (ui 86) — fast shots (patch 0094)
- A photo no longer asks for a new AF scan (the live focus already set the
  lens) and skips the AE precapture when the preview's AE is converged:
  ~0.35 s per shot instead of ~1.7 s. "AF wait" setting for the cases that
  do scan.
- RAW stream always "max"; a camera switch resets the JPEG size: starting
  the front camera or a 16:9 size no longer fails.
- Loupe: zoom about the centre, two-finger pan, the reference circle follows;
  peak line width in the device file; AWB presets reset warmth/tint.
- `docs/UI-NOTES.md` records the EV and white-balance mechanisms vs RAWfish.

## 0.38.0 — app 0.11.0 (ui 85) (patch 0093)
- Peaking by Laplacian (blurred edges no longer light up), 1-pixel red lines.
- Manual white balance as a shift from the HAL's AWB (`wb shift=S tint=T`),
  with a correct return to AWB.
- Simple-mode brightness slider rewritten (own component, -3..+3 EV);
  loupe diagonal; gallery keeps its index across the details page; "off"
  for scene 0; FACE_PRIORITY keeps the atomic controls.

## 0.37.0 — app 0.10.0 (ui 84) (patch 0092)
- Scenes over several columns with names and theme icons; temperature/tint
  columns (replaced in 0.38 by the shift); simple mode defined by
  `~/.config/com.indraw/indraw/simple.json`.
- First Laplacian-free peaking rewrite (superseded).

## 0.36.0 — app 0.9.0 (ui 83) (patch 0091)
- `scene off|N`; caps lists scene modes.
- Camera-switch "engine stopped" fixed (the old engine's exit was taken for
  the new one's); cover; no page dots; simple/advanced mode; scenes with
  greying; peak/zebra tweaks; resolution rules (clean binnings only, no
  size wider than the RAW16 in raw); 16:9 frame; menu gestures (shorter
  swipe, swipe up to close); volume auto-repeat; loupe to 12x; "jpg yuv"
  notice.

## 0.35.0 — app 0.8.x (ui 80–82) (patches 0088–0090)
- Gallery as a Silica page with the system's ImageViewer and GalleryOverlay
  (share, delete, details); the gallery is the root page and the camera an
  attached page, so the inner left-to-right swipe slides it in from the left.
- Reference circle stays until removed; continuous focus follows the point
  (engine `tap` respects the focus mode); focus labels "circle"/"auto"; a sun
  marks light-only taps.
- Settings menu as a band wider than the screen with a position bar.

## 0.34.0 — app 0.7.0 (ui 79) (patch 0087)
- Tap regions on every request with AF cancel/start (only the first tap
  worked); CLI `wb` (had been missing); `probe`/`cameras` without an ID.
- Settings overlay in the Jolla Camera / RAWfish style; gallery close fix.

## 0.33.0 — app 0.6.0 (ui 78) (patch 0086)
- Tap in auto (no AE precapture with the AF trigger); menu on a downward
  drag inside the image; gallery folder created at start, ImageViewer;
  left-handed deck in inverted landscape; screenshot flash; new icon.

## 0.32.0 — app 0.5.x (ui 76–77) (patches 0080–0085)
- `wb MODE`; caps lists JPEG sizes and AWB modes; the per-device aid
  thresholds announced in 0.29 really land (the earlier edit never did).
- Menu: self-timer, resolution, format, quality, white balance, noise,
  sharpening, grid, bracketing; settings persisted in an INI file.
- UI rotation in every orientation (deck on the right in landscape).
- Credits and lineage (`CREDITS.md`, Jolla Camera license text); the spec
  icon line fixed; build binaries untracked.

## 0.31.0 — app 0.4.0 (ui 75) (patch 0079)
- `captured` for raw/stack and `written` events; `NAME_vf.jpg` preview next
  to each DNG, shown by the gallery and the system's.

## Rename to indRAW (patches 0077–0078)
- Everything that was "rawbar" renamed to "indraw"; personal paths removed;
LICENSE (BSD-3-Clause), README introduction, app icon; focus circle styled
by the AF outcome (dotted / dashed / solid).

## 0.30.0 — app 0.3.1 (ui 73) (patch 0076)
- AE metering circle in full auto; front-camera orientation from caps;
  in-app screenshot (lipstick's service is unreachable from the sandbox).

## 0.29.0 — app 0.3.0 (ui 72) (patch 0075)
- `tap` (metering/focus region at a sensor point); swipe-down menu; focal
  "(=35)"; zebra level by image type.

## 0.28.0 — app 0.2.0 (ui 71) (patches 0073–0074)
- `indraw-engine` RPM with a version shared by library, CLI and spec (the
  CLI refuses a mismatch); camera button shows the equivalent focal length;
  gallery on an inner swipe; rpm's dependency scanner kept off the Android
  library.

## 0.27.0 — (ui 69–70) (patches 0071–0072)
- Real digital zoom (crop region), `max` sizes for any camera, focus loupe,
  volume double-press screenshot; zebra, peaking, grid; CLI comment lines.

## 0.26.0 — (ui 65–68) (patches 0065–0070)
- Auto / priority / manual exposure with the app's own AE loop on a RAW16
  meter; live `ae` and `caps` lines; EV; AE notch button; histogram with
  clipping bars; volume-key manual focus; camera switch; the app made to
  work inside Sailjail (install path, log location, engine in
  /usr/libexec).

## Before 0.26.0 (patches 0001–0064)
- The engine as a command-line tool: Camera2 session, JPEG, RAW16 DNG,
  stacked exposures, noise profiles, device profiles with focus calibration;
  the SF2P frame stream; the first viewfinder app (ui 64).
