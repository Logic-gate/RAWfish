# (AI generated)
# Porting RAWfish-indRAW to another device

RAWfish-indRAW was built and tested on the Jolla Phone 2 (JP2, MediaTek HAL, Sony
IMX766). Everything device-specific was pushed into data the engine reads at
run time. This page says what to expect, what RAWfish's README lists as porting
work and how RAWfish-indRAW stands on each point, and the steps.

## Prerequisites the device must meet (no way around them)

- A Sailfish OS port whose Android base is **Android 7 (API 24) or newer**
  with the Camera2 NDK libraries (`libcamera2ndk.so`, `libmediandk.so`)
  reachable through libhybris. Devices on older bases (e.g. Xperia X, Android
  6) cannot run the engine at all.
- **64-bit or 32-bit**: the build follows the sfdk target. A 64-bit target
  (`aarch64`) gives an `arm64-v8a` library installed in
  `/usr/libexec/droid-hybris/system/lib64/`; a 32-bit one (`armv7hl`) an
  `armeabi-v7a` library in `.../system/lib/`. Nothing else changes; build
  once per target (`sfdk config target=...` then `build-all.sh`). The
  package's dependencies follow too (a 64-bit library is named with a
  `()(64bit)` suffix by rpm, a 32-bit one without). On 32-bit ARM the
  exported entry points use the hard-float calling convention (`IR_API`
  in android/indraw.c): Android armeabi-v7a passes floats in the core
  registers, Sailfish armv7hl in the VFP ones, and a mismatch crashes any
  call with a float argument (found on the Xperia XA2, October 2026: the
  first 32-bit phone the engine ran on -- it opens, streams, probes).
- For RAW: the HAL must list a `RAW_SENSOR` (RAW16) output size. Without it the
  engine still runs (JPEG only, `raw=max` opens without RAW; the type button
  then has nothing to offer for raw).

## RAWfish's porting list, point by point

| RAWfish's point | RAWfish-indRAW |
|---|---|
| Camera2 through libhybris | same requirement (above) |
| RAW16 output from the HAL | same; absent RAW is handled (JPEG only) |
| complete sensor metadata for DNG | same dependency; the DNG writer leaves out what the HAL does not give (colour matrices, noise profile, shading) instead of inventing it |
| camera IDs differ | the FOCAL button cycles the ids the HAL lists (caps `cameras`) and shows each camera's equivalent focal length; nothing is hard-coded |
| orientation, mirroring, cropping, focus coordinates | the view rotation comes from the HAL's sensor orientation per camera; taps and the loupe go through the same transform; front cameras are not mirrored by design (a camera, not a mirror) |
| lens / ISO / shutter metadata missing or wrong | the engine reports what the HAL applied and checks it against the image (`matches_image`); the Exif it writes carries only what the HAL reported |
| warm-JPEG path depending on a helper staying alive | not applicable: one session, one process, the preview is the session |
| vendor library namespace issues | the engine uses only standard Camera2 keys (no vendor tags); loading problems would show at `rawfish-ir-cli cameras` |
| performance varies | the meter rate, the peaking and the stack latency are measured or adjustable, not assumed |

What RAWfish-indRAW adds that is device-specific and must be measured: the single
exposure ceiling, the focus calibration, the aid thresholds (below).

## Steps

1. Build and install both RPMs (`docs/BUILDING.md`).
2. On the device: `rawfish-ir-cli cameras` lists the cameras; `rawfish-ir-cli caps 0` dumps
   what the HAL claims for one. If the first fails with a libhybris or binder
   error, stop here: the prerequisites are not met.
3. `rawfish-ir-cli probe` writes `generated-hal.json` into
   `~/.local/share/com.rawfish/rawfish-ir/device-profiles/` (read-only reference).
4. Try the command line before the app: `rawfish-ir-cli stream 0 max max raw=max`
   with `jpeg test` / `raw test` on stdin (`docs/DEVICE-PROFILES.md` has
   examples). Read the JSON replies: `applied` must match `requested`, and
   `matches_image` must be true.
5. Measure the **single-exposure ceiling**: the longest `exp=` the HAL really
   honours (on the JP2 it says 16 s and lies above 1/2 s). Put it in
   `override-hal.json` as `observed_shutter_max_ns`. Everything longer is
   stacked.
6. Run the app. Check: the viewfinder is upright on every camera and
   orientation; a tap's circle lands where you tap; the wheels' ranges look
   right (caps); the gallery shows the captures.
7. Tune the aids (`peak_edge`, `peak_width`, `zebra_jpeg`, `zebra_raw`) in
   `override-hal.json` from what the viewfinder shows.
8. Manual focus distances: the HAL's "diopters" are rarely true. Measure a
   few distances against a tape and fill `focus_calibration`
   (`docs/DEVICE-PROFILES.md`); without it the distances are the HAL's,
   labelled as uncalibrated in the log.
9. Stacking: take a Σ4s shot and a plain shot of the same scene and compare
   sharpness (the lens must not move) and exposure; the first stack also
   measures the HAL's per-frame latency for the wheel's time estimates.

## What to send back for the repository

A device profile folder (`generated-hal.json`, `override-hal.json`) and the
notes of steps 5, 7 and 8 are enough for the next person with the same phone.
Device profiles are data: they can be added without touching the code.
- Crops and 3A regions are in ACTIVE-ARRAY coordinates ((0, 0) = the
  active array's top-left, whatever its offset in the pixel array). A HAL
  may tolerate an overflowing crop (the JP2's did), another raise a fatal
  device error (the Xperia XA2's, active array at 8,8: the viewfinder froze
  after two frames). Since 0.47.26 the engine never adds the offset.
- Photo shape: the preview is the largest YUV size no wider than 1280 with
  the photo's proportions (within 2 %), else the largest; the viewfinder
  follows it (a 16:9 sensor gets a 16:9 viewfinder). Short screens: the
  viewfinder shrinks so the deck keeps its minimum height.

## Found on the Xperia XA2 (October 2026, 0.47.50 / ui 157)

- In RAW the lens hunted without end: the RAW meter's request kept the
  setup's CONTINUOUS_PICTURE while the preview was in manual or 'once', and
  this HAL restarted a scan at every meter frame (the JP2's did not mind).
  The meter request now carries the preview's AF mode and lens distance,
  as it already did for the exposure. The same for a CAPTURE that keeps
  the preview's focus (the 'once' mode after a tap, a lock): the still and
  raw requests kept the setup's continuous AF and this HAL started a scan
  at the capture, undoing the lock (0.47.51).
- A 16:9 RAW stream (the HAL's 20 MP 16:9 readout of the 4:3 IMX300:
  RAWfish-indRAW never sees more than the HAL delivers): "4:3" asked for the
  largest size of all, which is 16:9 -- the format did nothing. The format
  now takes the largest size of its own ratio, the binning judged on the
  dimension the format does not crop, and the menu says which format is
  "as the RAW" (the XA2's 4:3, 4208x3120, is another readout: taller than
  the 16:9 JPEG, narrower).
- A camera switch started the engine twice (the switch, then the size
  reset). The size is forgotten without a restart; the new camera's caps
  settle it, restarting only if "max" was not the format's size.

## When a HAL moves the lens at the shutter (XA2, 0.47.54)

On the Xperia XA2 the lens moved at the shutter in 'once' and manual focus,
although every capture asked for AF off at the preview's distance and its
result reported that distance (this HAL echoes the requested value, not the
lens's real position). Two tools to find what the HAL's still template adds:
- the log's '# templates:' line (read-only, every phone): the preview,
  photo and RAW requests' AF mode, capture intent, focus distance and every
  vendor request key with its type and value;
- the camera block's "still_template": "preview" in override-hal.json
  builds the photo and RAW requests from the preview template; if the lens
  then stays put, the culprit is in the still template, and a
  vendor_request entry ("on": "photo") can neutralise the key at fault.
Neither changes anything on a phone whose profile does not ask for it.

The XA2's templates (log of 24 Oct): the preview and the photo requests
differ only by the capture intent (1 preview / 2 still) and Sony's vendor
key 0x801c0000 (i32: 1 in the preview, 0 in the photo and RAW);
0x801b0000 (u8) is 0 in all, 0x801b0001 unset. Tests: "still_template":
"preview" (both differences at once), then 0x801c0000 = 1 on the photo
alone (vendor_request, "on": "photo"). "capture_delay_ms" in the
"overrides" block adds a fixed pause before every capture (0.47.55).

## The real cause: a stale trigger request (0.47.56)

The lens moving at the shutter on the XA2 (only the full-auto mode was
safe; manual and 'once', with manual exposure, came out blurred at random,
#5 blurred / #6 sharp with identical settings) was ours: after every photo
af_release() sent an AF_TRIGGER_CANCEL on g.trigger_request, a request
built once at setup -- AF continuous, no lens distance (infinity), AE on --
and never synchronised with the preview. For one frame between the
preview's, the HAL was told "continuous AF, lens at infinity, auto
exposure"; the lens left and came back, and the photo, a few frames ahead
in the HAL's pipeline, was sometimes exposed while it travelled. The JP2's
HAL never showed it. send_trigger() now copies the preview's focus and
exposure onto the trigger request first, and af_release() sends a cancel
only when a scan or precapture was actually started. The template, Sony
key and control-mode leads were dead ends.
