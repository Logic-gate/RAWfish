# Roadmap and open questions (AI generated)

What is known to be worth doing, and what has been decided against for now.
Each item says why, so it can be picked up later without re-deriving it.

## Open: to do or to decide

### Branches
- **`main`** is the one line of work since 24 Nov 2026 (patch 0528): the
  development branch `img_transport` came into it once tried on the JP2,
  the Xperia 10 III and the XA2. That branch held patches 0431-0435 (the
  viewfinder as YUV converted by the GPU, the aids reading it, the engine
  handover after a capture, the 18 mm's burst ISO, the settings kept
  across patch builds), then `zero_copy` (0436-0443: the zero-copy
  viewfinder, the aids' maps from the engine drawn by the GPU, the stats
  lines, the display kept on), then the ports of main's patches; the
  comments that say "branch zero_copy" date from then. The release before
  it is the tag of 0.3.10.
- **`longexp`** (personal use, not published): main plus the long
  exposures on the JP2 (the sensor's registers through I2C, the patched
  camera HAL), unsandboxed.

### Before a first public release
- **Publication**: a fresh snapshot as the public repository's first commit
  (the history keeps old names and notes that are not for publication; the
  README says the history starts there), then the files split:
  `android/rawfish-ir.c` (~6000 lines) and `app/qml/rawfish-ir.qml` (~2000) by
  responsibility (session, capture, stacking, shading, DNG / page, deck,
  overlays), with the host tests kept green at each step.
- **The engine is sent commands while it stops**: after 'quit' the
  controller still forwards a few (the log shows "camera not open" /
  "setRepeatingRequest failed"). Harmless, untidy: the controller should
  stop forwarding once 'quit' is sent.

### Features

- **A shorter shutter delay with ZSL** (zero shutter lag): the camera keeps
  the last full-resolution frames, unprocessed, in a ring buffer; at the
  press the app takes the frame of that instant and sends it back to the
  HAL to be processed into the JPEG (reprocessing). It would remove most of
  the 0.35-0.45 s between the press and the image on the JP2. Needs the
  HAL's reprocessing capability (the probe's capabilities:
  private_reprocessing or yuv_reprocessing; the XA2 has neither, the JP2
  to check). A RAW variant would keep the RAW meter's last frames. To
  measure first with the log's milliseconds (0.47.70). Meanwhile the ZSL
  noise / sharpening modes are not offered in the menu (ui 179): they are
  meant for frames that will be reprocessed; without that path they only
  mean what the HAL decides (fast or high quality).

- **A gap between the x3 burst's frames** (asked 26 Oct): spacing them by
  SENSOR_FRAME_DURATION does nothing on the JP2 (VENDOR-TESTS: 34-41 ms
  whatever the gap; withdrawn in 0.47.74). To continue if wanted: read the
  frame duration each frame's result reports; try the burst with the
  preview paused, then with CONTROL_MODE OFF; else captures sent one by one
  with a pause (any interval, a few tens of ms of precision, each with the
  usual 0.35 s delay).

- **A JSON library for the profile, override and calibration readers**
  (cJSON, one file, MIT): suggested by two October 2026 audits. The hand
  readers are bounded and tested (no memory fault found); a library would
  remove a class of misreadings of odd but valid JSON. Not urgent.

### Focus (open, November 2026)

- **A touch at the top of the frame focuses farther than at the bottom,
  on the same object.** Seen on the JP2 in manual focus: the same object
  moved from the bottom to the top of the frame reads a farther distance.
  Not optical: with the phone turned 180 degrees (the interface turns
  with it) the effect stays tied to the SCREEN, not to the sensor. Checked
  and in order: the touch area is exactly the viewfinder zone (same
  origin, same size); the viewfinder has the frame's proportions (4:3, no
  cropping on the screen); the screen -> frame -> sensor mapping agrees to
  the pixel in the log, and the two rotation conversions are exact
  inverses; the HAL reports the preview's crop as the whole array. One
  cause left with exactly this signature: the preview covering a little
  LESS of the sensor than the HAL reports (an undeclared crop inside the
  ISP). The zones would then be right at the centre and pushed outwards
  towards the edges: above the object at the top (background: farther),
  below it at the bottom (foreground: nearer), following the screen; the
  square, drawn by the inverse mapping, would still sit under the finger.
  To check: phone on a support, a screenshot of the viewfinder, then a
  photo without moving; compare the edges. Same edges: drop this lead.
  The viewfinder shows less: measure the factor and size the zones on the
  preview's real field. The same effect should then exist left-right.
  Left as is for now (November 2026). Since then (0407): in 16:9 the zones
  WERE misplaced -- that frame shows only the sensor's middle band, which
  the mapping ignored -- and the fix made the touch focus work there. In
  4:3 the frame and the sensor have the same shape, so that cause does
  not apply; the "undeclared crop" lead is weaker but not ruled out: the
  support test above still decides it.

- **The 18 mm (camera 2): closed for now.** Its autofocus searches one
  way and passes the sharp position; our touches could not focus it
  (bottom third of the screen only, log of 14 Nov), nor could Jolla
  Camera's. Since 0403 it takes no focus search of ours: automatic is the
  camera's own, manual the volume keys; the experiments were removed in
  0414. Open idea, for a branch of its own: our own contrast autofocus
  (lens steps measured on the viewfinder) for that camera.

- **The manual distance shown is not reliable away from the centre.** It
  is the HAL's lens position converted through a focus table measured at
  the centre; with the effect above, and with the steep scale in metres
  (one 0.05 D step is 20 m to infinity, 180 of the 200 steps lie within
  the last 90 cm), the number misleads. Replacements considered: the step
  number (0 = infinity .. 200 = 10 cm on the JP2: honest, repeatable, the
  volume keys move it one by one); a scale between a mountain (infinity)
  and a flower (close-up) with a sliding mark; coarse bands (close-up,
  near, middle, far, infinity: still the table, with jumps at the bounds);
  the distance kept only for a point near the centre and marked "≈"
  elsewhere. Leaning: the step number with the small scale. To decide.

### From the November 2026 review (open)

- **Two very long files**: `android/rawfish-ir.c` (about 7300 lines) and
  `app/qml/rawfish-ir.qml` (about 2300) are the main obstacle to a human
  review. To split by subject without changing behaviour: the engine into
  session / capture / metering / white balance / calibration units, the
  page into components (deck, viewfinder overlays, white-balance controls).
- **A private Silica import**: the gallery uses `Sailfish.Silica.private`
  for its dismiss button (the same one as Jolla's gallery). It may change
  with a Sailfish update: to replace by a button of our own.


### To find out
- **The viewfinder's "maze" at high ISO** (JP2; RAWfish shows the same):
  after about thirty seconds on a still scene, maze-like patterns and an
  afterimage on small moves; a scene change resets it; photos unaffected;
  the standard viewfinder noise and sharpening keys change nothing. Not the
  HAL's 3DNR (key `0x80010000`: accepted, echoed, no effect -- 21 October,
  VENDOR-TESTS). Next candidates: the viewfinder's own processing chain,
  then a YUV preview taken before the HAL's processing (see "Viewfinder
  transport").
- **The HAL's multi-frame noise reduction by default** (JP2, key
  `0x80040000`): does the HAL blend frames on a plain photo when we say
  nothing? Compare a photo with the key at 0 and without it, in low light.
- **A noisy Σ at a low ISO** (JP2, November 2026): 15 s at ISO 400 is
  clean, 60 s at ISO 100 very noisy, though both gather the same light.
  Candidates: the read noise of four times as many sub-frames adding up
  (each sub-frame's ceiling the same, so 60 s at ISO 100 is 4x the
  frames), and the sensor's heat over a minute (dark current). To measure:
  a dark Σ at both pairs, the noise per sub-frame and the sum. If it is
  the read noise, Σ could choose a higher sub-frame ISO itself (fewer,
  shorter frames for the same light), as the tile merge's split did
  (removed in 0.3.11-553: at the sensor's highest ISO it could not make up
  for the shorter frames, and gained nothing measurable).
- **The tile merge (0445/0453) against the classic one**: hand-held,
  4 and 8 frames; the noise and the detail
  after the sharpening, against the same shot classic.
- **The XA2's RAW noise and linearity**: a dark-frame and flat series as
  for the JP2 (docs/measurements).
- **Level**: the sign of the roll in landscape (portrait is verified).
- **JP2 flash**: availability and the pre-flash delay in automatic
  exposure (the XA2's works).

### The sensor's analog gain: a declaration to verify

`caps` reports `iso_analog_max` from `SENSOR_MAX_ANALOG_SENSITIVITY`: a
declaration of the HAL (the sensor), never verified -- and this HAL also
declared a 16 s maximum exposure where 0.5 s was measured. The dark-frame
series (`docs/measurements/jp2-dark-noise.csv`) stops at ISO 3200 and does
not show where the analog gain ends (read noise at the floor up to ISO 400,
~0.5 DN, then rising: 1.1 DN at 1600, 1.7 at 3200). To do: extend the
dark frames (and a few flats) from 3200 to the maximum ISO and look for the
two signatures of a digital gain in the RAW: a **comb** in the histogram
(only every k-th code value occurs) and a noise in DN growing exactly as the
ISO. It matters for the Σ viewfinder, which pushes the ISO high: a digital
gain stays proportional but quantises the shadows.


## Ideas for later

### Viewfinder transport: cheaper and sharper

Up to 0.3.9 the engine converted each preview frame to RGB and sent it
through a pipe (SF2P), 1280x960 at most -- the same as RAWfish's bridge.
Steps, each falling back on the previous one where it does not work:
1. YUV frames converted to colour by the GPU (a shader). **First half done
   in 0.3.11** (patch 0431): SF2Y, I420 through the same pipe (half the
   bytes, no conversion on the camera thread nor in the app), the shader in
   the item. ProCam (a RAWfish fork) does the same. Left: measure it on the
   phone (viewfinderYuv=0 / 1), 15 Nov 2026: the engine's own time per
   frame ~5 ms shorter (wall time, waits on the pipe included), the CPU
   that `top` shows for the two processes about the same within its noise
   (a first comparison that looked like -39 % mixed 19 and 29 frames a
   second; the runs whose mode the log confirms show no clear CPU change).
   Kept for the halved pipe traffic, a viewfinder larger than 1280x960
   later, and no cost. The aids read the YUV frame directly since 0432.
   Shared memory skipped: the copy through the pipe is not what costs.
2. Zero-copy: `AImageReader` producing `AHardwareBuffer`s, handed to the app
   (`AHardwareBuffer_sendHandleToUnixSocket`) and imported as EGL images via
   libhybris -- what GStreamer's `droideglsink` does. Best performance; depends
   on each device's graphics driver: test on the JP2 first.
   **Branch `zero_copy`, step 0: a probe (patch 0436).** rawfish-ir.conf
   `hwProbe=5` (and `hwProbeGpuUsage=1` for buffers allocated for GPU
   sampling too) has the engine send its next 5 viewfinder buffers' gralloc
   handles (fds over a unix socket, SCM_RIGHTS) to the app, which imports
   each with libhybris's `eglHybrisCreateRemoteBuffer` (what a Wayland
   compositor does with a client's buffer), binds it to an external texture,
   draws it off screen and reads it back. The log says at each step what
   worked ("hwprobe ..." from the engine, "# hwprobe app: ..." from the app,
   RESULT=picture when the GPU saw the image); the first good one is saved
   as ~/.cache/com.rawfish/rawfish-ir/hwprobe-*.png. The viewfinder itself
   is unchanged. If it works: the frames' handles instead of their pixels,
   the shader on the external texture, a few buffers in flight, released
   when drawn. If not: the YUV pipe (0431) stays.
   Result on the JP2 (Mali-G610 MC2): RESULT=picture, 5/5, three runs;
   format 0x23, usage 0x20033, an import takes 2-8 ms (once per buffer).
   **Step 1: the transport (patch 0437).** SF2H packets (a buffer id and a
   sequence number) in the pipe, each buffer's handle once over the socket
   (HWB2), the app's releases back (REL1); 6 viewfinder buffers by default
   (`vfBuffers`, 4-10): at most `vfBuffers - 2` held by the app, the
   oldest taken back beyond that ("forced=" in the "viewfinder pace" line).
   The zebra and the peaking switch the engine to the YUV pipe while on
   (they read pixels); any failure falls back to it. To measure on the
   phone: CPU and VmRSS of both processes against `viewfinderZeroCopy=0`,
   at the same frame rate (same light), and how often "forced" grows.
   **Measured on the JP2 (15 Nov 2026, patch 0438's "# stats" lines).**
   Zero copy works on the three cameras, `held=2 forced=0` throughout,
   the picture unchanged to the eye. Same scene, same frame rate (29.5
   fps), 5 lines of 5 s each way (one switch, zero copy then YUV):
   app CPU 22.5 % against 40.4 % (-18 points, -44 %), engine 14.0 %
   against 23.8 % (-10 points, -41 %) -- together -28 % of a core; the
   system -5 points of 8 cores (some of it the compositor and the GPU
   driver, no longer fed 1.8 MB a frame); engine VmRSS 45 MB against
   48-49; latency 101 ms against 111; the engine's time per frame 3.0 ms
   against 11-12 (19.5 fps: 5.0 against 13.7). The battery current
   (~860 against ~915 mA) is within its noise (790-1020 mA from one line
   to the next): the screen dominates; minutes each way would be needed.
   **Step 2: the aids without leaving zero copy (patch 0439).** The zebra
   and the peaking no longer switch the viewfinder to the YUV pipe: the
   engine works out what they compare with their thresholds on the full
   frame, with the app's own arithmetic, and sends one value per 2x2 cell
   -- the one that decides (SF2A, `vf aids N`): the peaking's |Laplacian|
   coded so that `q >= edge` exactly where a pixel passes, the luma or
   brightest-channel maximum for the zebra, the 2x2 mean the RAW zebra's
   histogram already read. Exact in value, coarser in place (the user's
   choice). Checked: the engine's maps against the app's full-resolution
   decisions, every cell, three frame sizes. Costs ~0.3 MB a frame per
   aid instead of 1.8 MB, and the peaking's maps ~5-6 ms a frame on the
   engine's camera thread (measured on a desktop; the app no longer
   computes the full-resolution Laplacian). An engine without SF2A: the
   aids from the YUV pipe, as before, after 2 s.
   Measured on the JP2 (15 Nov 2026, 13:24, 29.5 fps, app + engine CPU):
   no aid 36 %, zebra on the luma 72 %, peaking 96 %, both 102 % -- the
   YUV pipe with both 110 % (the system: 43.9 against 45.1 % of 8 cores;
   the battery ~945 against ~1050 mA, the ranges apart). A smaller gain
   than hoped: the engine read the camera's buffer byte by byte, which is
   not cached for the CPU (the luma maximum 5.5 ms a frame against 0.7 on
   a desktop), and the frame waited for the maps (latency 110 ms against
   93). **Patch 0440:** the planes copied in one block per row first, the
   maps worked out after the frame went (the overlay follows a few ms
   later), and on one frame in `aidsEvery` (default 2: 15 a second; the
   app redraws the overlay at that pace). The "viewfinder pace" line says
   `aids_ms` (each map set) and `aids_fps`.
   Measured (15 Nov 2026, 13:50-13:53, both aids, ~29.3 fps): every frame
   102 % (app 57 + engine 45), one in two 75 % (42 + 33), the YUV pipe
   111 % (89 + 22); the system 43.2 / 39.8 / 45.2 %; the battery ~930 /
   ~840 / ~1050 mA; the frame no longer waits (`ours_ms` 1.7-2.0 against
   15; latency 116 against 127 ms for the YUV pipe in the same minute).
   One in two looks fine to the eye: the default. The copy changed
   nothing: a map set still took ~14 ms -- the work itself, likely on a
   little core (Cortex-A55). **Patch 0441:** the peaking's Laplacian row
   by row (a few rows in the cache instead of two whole planes) in loops
   the compiler vectorises (checked in the aarch64 assembly): 6.6 to
   2.7 ms on a desktop with clang (the NDK's compiler), the same values.
   Measured (14:07, both aids, one map set in two): a map set ~13 ms
   against ~17.5 (-25 %, less than on the desktop: the core runs slowly
   at this load -- at 14:00:04 the CPU sped up and a set took 7.8 ms),
   the engine 27 % against 33 %.
   **Patch 0442: the aids drawn by the GPU.** The maps uploaded as two
   one-byte textures (0.3 MB each) when they change, a shader making
   overlayCore's decisions per fragment (border, local maximum, line
   thickening, stripes every 4 cells, the same colours); no ARGB image
   built and uploaded per update. Checked: the GPU's overlay and the
   CPU's identical pixel for pixel (peaking, both zebras, line widths,
   the magnifier). The RAW zebra (levels from the histogram, stripes
   kept from frame to frame) stays on the CPU. `aidsGpu=0` to compare.
   Measured (14:23-14:26, three sessions 0 / 1 / 0, both aids, one map
   set in two): the app 31.9 % against 44.5 and 45.6, the system 38.9 %
   against 40.2 and 40.3 of 8 cores (the app's saving, not moved to the
   compositor), the battery unchanged (~840 mA); the same look.
   **Where it ended, 15 Nov 2026** (both aids, 29.5 fps): app + engine
   59 % against 111 % for the YUV pipe, the system 38.9 against 45.2 %,
   the battery ~840 against ~1050 mA, latency ~101 against ~110 ms; with
   no aid: 36 %, 34.4 %, ~830 mA.
   **Later, small gain: a viewfinder larger than 1280x960.** The JP2's
   screen is ~1032-1080 px wide (a forum readout: 1032x2272): 960 px of
   viewfinder are enlarged 8-12 %. 1440x1088 would cover it. Worth it
   with a full-resolution peaking (the cells would halve it again).


### A hand-held RAW merge
Σ adds its sub-frames as they are, without aligning them: it needs a
tripod, and whatever moves leaves a trail. A RAW merge would take a burst
of short RAW16 frames, align each part of each frame on a reference frame
(a tile search, coarse to fine, to a fraction of a pixel), leave out the
tiles that do not match (a moving subject), and write the sum as one DNG,
as Σ does. For hand-held night shots, and a Σ that forgives a small move.
Open: aligning on the Bayer mosaic (2x2 blocks, or a half-size grey
image), the time per full-size frame on the phone, and keeping the DNG's
brightness exact where tiles are left out (each pixel divided by its own
count of frames).

### Merge with RAWfish
Possible shape: RAWfish-ir's engine (one process, one Camera2 session for
viewfinder and photos) under RAWfish's interface in photo mode, GStreamer
kept for video. The controller layer (docs/ARCHITECTURE.md, done in 0.47.9)
already gives any interface the same camera behaviour.

### In reserve: a boosted Σ preview at the limit (not to do unless needed)

If the adaptive Σ viewfinder (0.47.16) proves insufficient in very dark
scenes, a last resort, agreed in principle and kept in reserve:
- **only at the limit**: the preview frame already at 0.5 s and the maximum
  ISO, its signal still in the noise -- never before;
- the preview raises its gain BEYOND the photo's equivalent, so the meter
  has signal, and the measure divides the extra gain back out;
- a **blue frame** around the viewfinder and a message: the image is
  brighter than the photo, trust **only the histogram**;
- the histogram must then be shifted back by the extra gain (by
  calculation), or it would show the brightened frame instead of the photo.

Prerequisite: the analog-gain test above. An extra gain helps the
measure only while it is ANALOG (applied in the sensor before the
conversion, it lifts a faint signal above the read noise and the
converter's step); a digital gain is a multiplication of numbers already
read and adds no information -- it would only brighten the display. So the
reserve makes sense only while there is analog headroom left.

Also to measure first: how low the "too dark" threshold can go. A mean over
thousands of pixels stays measurable down to about 1 DN, the noise acting
as a dither, provided the black level is exact: the provisional 4 DN is
likely cautious.


## Decided against (for now)

### Our own tone curves (tried in October 2026, branch `saturation`)
In manual white balance the ISP takes our colour matrix and our tone
curves (JP2, tested from the terminal). The matrix works well:
saturation and toning (sepia, blue...) keep the camera's own tone
processing. Our curves do not: the camera's curve adapts to each scene
(it darkened the shadows of a flat indoor scene and opened those of a
contrasty outdoor one), and a fixed curve of ours blocked the shadows
where the camera's kept them. Kept for later: effects on top of the
camera's images (the matrix), not instead of its tone curve.

### A viewfinder built from the RAW stream

Demosaicing the RAW16 stream (already read for the meter) on the GPU would
free the viewfinder from the HAL's preview processing (no maze, true to the
DNG). Decided against for now: a 4096x3072 16-bit stream is 25 MB per frame,
the HAL often delivers RAW at a reduced rate, and the cost in CPU/GPU and
battery is high. If ever, as an optional mode, not as the default viewfinder.


## Done (pointers)

- Lens shading: measured per lens position and light, applied per photo,
  an on-phone assistant, calibrations shipped for the JP2 and the XA2
  (docs/LENS-SHADING.md).
- Engine / controller / interface split (docs/ARCHITECTURE.md, 0.47.9).
- Device profiles shipped per camera module, matched by model or RAW size
  (docs/DEVICE-PROFILES.md); the Xperia XA2 as a second target
  (docs/DEVICE-XA2.md).
- The October 2026 code review and its second pass (docs/ROBUSTNESS.md).
- The level: a cross with the degrees, a recorded zero, a steadier filter.
- Stacking (Σ), flash and the front camera checked on the XA2.
