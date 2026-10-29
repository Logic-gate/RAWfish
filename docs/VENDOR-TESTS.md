#(AI generated)
# Vendor (chipset) keys: what was tried, and what came of it

Every experiment with the chipset's own request keys (`vendor_request` in
`override-hal.json`, see DEVICE-PROFILES.md), with its date, versions,
numbers and conclusion, so that nothing is tried twice blindly. The keys
are named here by their numbers and by what they were observed to do;
the chipset maker's own identifier strings for them are deliberately not
reproduced (their provenance could not be established as public). The
values come from `generated-hal.json` (`vendor_chars`, `session_keys`,
written by the probe since 0.47.23) and from the trials.

How to read a test: the log says, for each key, that the NDK ACCEPTED it
(`# vendor_request 0x...: {"ok":true,...}`) and, 0.7 s later, what the
preview RESULTS report for it (`# vendor_request in the results: {...}`):
the value sent means the HAL echoes it; "absent" means the preview results
do not carry the key (normal for a key acting on photos).

## JP2 (MediaTek), main camera

### 3DNR -- temporal noise reduction of the viewfinder (21 October 2026)

- Key: `0x80010000` (the 3DNR mode), int32, per request (not among the
  session keys). Allowed values (listed by the characteristic
  `0x80010001`): `[0, 1]` -- 0 off, 1 on.
- Engine 0.47.27, `"on": "preview"`.
- Result: accepted; the preview results echo exactly the value sent (0,
  then 1): the HAL takes it into account.
- Seen: no visible difference in the viewfinder between 0 and 1 (nor 2,
  out of the list, tried earlier).
- Conclusion: the viewfinder's "maze" at high ISO does NOT come from the
  3DNR. Left to check, the same way: the viewfinder's own noise reduction
  (the menu's "Vf noise"; the app sets it to off), then other candidates.

### MFNR -- multi-frame noise reduction of the photo (21 October 2026)

- Key: `0x80040000` (the multi-frame blending mode), int32, per request.
  Allowed values (listed by the characteristic `0x80040002`):
  `[0, 1, 2, 255]` -- off, MFNR, the anti-shake variant, auto. Its
  outcome would be in `0x80040001`, on the PHOTO's result (the engine
  reads the preview's only: "absent", as expected).
- MediaTek's MFNR (also called MFLL): several RAW frames, a best-shot
  selection, conversion to YUV, a fusion into one YUV, then the JPEG. In
  auto, the HAL runs it only above an ISO threshold of its tuning.
- Engine 0.47.27, `"on": "photo"`, JPEG 4096x3072, minimal_noise /
  hq_edge.

Low light, ISO 9325, 1/20 s, the same scene (dark cloth):

| mfbmode | processing time | JPEG size | fine grain, dark areas |
|---|---|---|---|
| 0 (off) | 448 ms | 8.0 MB | 14.0 |
| 1 (MFNR) | 832 ms | 7.2 MB | 13.2 |
| 255 (auto) | 839 ms | 7.1 MB | 13.2 |

("fine grain": the standard deviation of the image minus its 3 px blur,
8-bit units -- noise plus fine texture; the bright, out-of-focus areas did
not change.)

Daylight, ISO 100, mfbmode 255: 383 and 359 ms -- the fusion did not run.

Conclusions:
- the MFNR can be driven by RAWfish-indRAW: 1 and 255 run it at high ISO (+0.4 s,
  a lighter JPEG); in auto (255) the HAL leaves it off when the light is
  enough;
- the gain is small: ~6 % less fine grain in the dark, nothing like the
  halving a real four-frame fusion would give; Σ in RAW does much better,
  under control;
- it acts on the JPEG only: not on the viewfinder (so not the maze), not
  on the RAW.
- Not known yet: what the HAL does WITHOUT the key (a JPEG at high ISO
  without vendor_request: ~450 ms means no MFNR by default, ~830 ms that
  the default already runs it).

To keep it (it does nothing in good light), in the camera block of
`override-hal.json`:

```json
"vendor_request": [
  { "tag": "0x80040000", "type": "i32", "value": 255, "on": "photo" }
]
```

To switch it off without losing the line, `"value": 0`. (JSON has no
comments: an entry at 0 is the switch off.)

### Session keys (read only when the session is configured)

From `generated-hal.json` (0.47.27), main camera: among others
`0x80030000` (HDR mode), `0x80070000` / `0x80070003` (EIS, preview EIS),
`0x00010011` (Android's standard video stabilisation mode), `0x800b001f`,
`0x800b0005`, `0x800b0012`, `0x800d0000` / `0x800d0002` (MediaTek's
configuration requests), `0x80130001` / `0x80130003`, `0x800f0000`. Setting
these in a request does nothing: they are read at session creation. Since
0.47.66 the camera block's `"session_request": [ {tag, type, value} ]`
hands them to the HAL as session parameters (the API 28 call, resolved at
run time; the open reply says `"session_params": "applied"`, `"none"` or
`"unsupported"` -- the XA2's Android 8.1 base has no session parameters).
(The HDR mode was tried this way on 25 Oct: see "HDR -- the HAL's own",
below: not reachable.)

### HDR -- the HAL's own (25 October 2026, engine 0.47.66-68): not reachable

What was tried, on a backlit scene, each time a photo with and one
without, compared in exposure (the capture's applied values) and by eye:
1. The standard scene mode HDR (control.sceneMode 18, control.mode
   USE_SCENE_MODE) -- first on the preview only (a bug: set_variant reset
   the capture's control mode, fixed in 0.47.67), then on the photo too
   ("control":2 in the reply).
2. The session key `0x80030000` = 1 (i32) as a session parameter
   (session_request, 0.47.66; "session_params":"applied").
3. The three request keys of the group, `0x80030000`, `0x80030001`,
   `0x8003000b` = 1 (i32), on every request ("on":"all"): all ACCEPTED by
   the NDK, none echoed in the preview results.
Result, every time: the photo exposed exactly as without (same exposure
and ISO), taken in one frame time (350-450 ms): no multi-frame
processing, no visible difference.

What the phone's libraries say (grep -a on /vendor/lib64; busybox's tr
has no [:print:]): `libmtkcam_metastore.so` names
`availableStaggerHdrModes`, `vhdrExpOrder`, `vhdrResult`, `isHdrSensor`,
`hdrDetectionResult` and a debug property `debug.camera.hal3.hdrDetection`;
`libmtkcam_featurepolicy.so` has `createHDRPolicyHelper`. The HDR these
keys concern is the SENSOR's staggered / video HDR, and the photo HDR is a
decision of the HAL's own feature policy, taken on its side, which the
vendor keys we can reach do not command. The debug property could not be
tried (no setprop under /usr/libexec/droid-hybris/system/bin).
The probe's `0x80030003: [0, 1]` is most likely the stagger modes list.

Conclusion: the HAL's HDR is not reachable from Camera2 on this phone
with what we know. Not to be retried blindly. An HDR of RAWfish-indRAW's own
(bracketed RAWs merged in linear space, on the stacking code) is a
possible chantier, a different one. Remove the HDR entries from the
override after testing: they do nothing.

### Burst frame spacing by SENSOR_FRAME_DURATION: ignored (26 Oct 2026)

Tried in 0.47.73 (withdrawn in 0.47.74): the x3 burst (one capture() call
with three still requests) with SENSOR_FRAME_DURATION set on the requests
to space the frames, exposure in auto (the live pair frozen) and in
manual. The spacing measured from the images' timestamps:

| Gap asked | exposure | spacing measured |
|---|---|---|
| none | 30 ms | 33.7 ms |
| 0.1 s | 40 ms | 41.0 ms |
| 0.2 s | 40 ms | 41.0 ms |
| 0.3 s | 30 ms | 34.0 ms |
| 0.5 s | 30 ms | 34.0 ms |
| 0.5 s (manual, 1/200 s) | 5 ms | 33.7 ms |
| 0.1 s (manual, 1/200 s) | 5 ms | 33.7 ms |

The frames always follow at the sensor's pace (the exposure or 1/30 s):
the HAL ignores the requested frame duration on these captures. Not
tried: the frame duration the HAL reports applied in each frame's result;
the preview's repeating request paused during the burst; CONTROL_MODE OFF
on the burst's requests. See ROADMAP.

## Xperia XA2 (Sony / Qualcomm), main camera

- Only three vendor request keys (`0x801b0000`, `0x801b0001`,
  `0x801c0000`), Sony's, no MediaTek ones; no session keys. Nothing tried.
