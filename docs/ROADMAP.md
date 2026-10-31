# Roadmap and open questions (AI generated)

What is known to be worth doing, and what has been decided against for now.
Each item says why, so it can be picked up later without re-deriving it.

## Open: to do or to decide

### Before a first public release
- **Publication**: a fresh snapshot as the public repository's first commit
  (the history keeps old names and notes that are not for publication; the
  README says the history starts there), then the files split:
  `android/indraw.c` (~6000 lines) and `app/qml/indraw.qml` (~2000) by
  responsibility (session, capture, stacking, shading, DNG / page, deck,
  overlays), with the host tests kept green at each step.
- **The engine is sent commands while it stops**: after 'quit' the
  controller still forwards a few (the log shows "camera not open" /
  "setRepeatingRequest failed"). Harmless, untidy: the controller should
  stop forwarding once 'quit' is sent.

### Features
- **A "stock" JPEG type**: the HAL's own processing at 8 MP, quality 85,
  for small everyday files. To decide: reopen the session at that size
  (about 1 s to switch, ~1.5 MB files) or keep the current size (instant,
  3-4 MB).
- **An HDR of RAWfish-indRAW's own**: the JP2 HAL's own is not reachable from
  Camera2 (VENDOR-TESTS). Bracketed RAWs merged in linear space on the
  stacking code (tripod or steady hand: no alignment), written as one DNG;
  the bracketing setting already takes the exposures.
- **XA2, 16:9 photos**: the deck would have to lie over the image for the
  viewfinder to take the full width (a 16:9 image on a 16:9 screen leaves
  no room under it).

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

Today the engine converts each preview frame to RGB and sends it through a
pipe (SF2P), 1280x960 at most -- the same as RAWfish's bridge. Steps, each
falling back on the previous one where it does not work:
1. YUV frames in shared memory, converted to colour by the GPU (a shader):
   removes most of the CPU cost, allows a larger viewfinder; portable.
2. Zero-copy: `AImageReader` producing `AHardwareBuffer`s, handed to the app
   (`AHardwareBuffer_sendHandleToUnixSocket`) and imported as EGL images via
   libhybris -- what GStreamer's `droideglsink` does. Best performance; depends
   on each device's graphics driver: test on the JP2 first.


### Merge with RAWfish
Possible shape: RAWfish-indRAW's engine (one process, one Camera2 session for
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
