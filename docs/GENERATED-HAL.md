# (AI generated)
# generated-hal.json, field by field

`rawfish-ir-cli probe` writes this file into `~/.local/share/com.rawfish/rawfish-ir/device-profiles/`.
It is a read-only snapshot of what the camera driver (the Android HAL) claims
for every camera: `{"cameras":[ <caps of camera 0>, <caps of camera 1>, ... ]}`.
RAWfish-indRAW never edits it; corrections go in `override-hal.json` next to it
(`docs/DEVICE-PROFILES.md`). `rawfish-ir-cli caps ID` prints one camera's block.

Sizes are `[width, height]`; times are nanoseconds; "modes" are Camera2 values
with their names. Everything here is **what the HAL says**, not what it does:
the override file exists because the two differ (the JP2 claims 16 s exposures
and honours 1/2 s).

| Field | Meaning | How RAWfish-indRAW uses it |
|---|---|---|
| `camera_id` | the HAL's id ("0" back, "1" front, more on multi-lens phones) | the FOCAL button cycles the ids |
| `jpeg`, `raw16`, `yuv` | every output size per format (JPEG; RAW_SENSOR 16-bit; YUV_420_888, the preview) | `max` picks the largest JPEG/RAW and the widest preview ≤ 1280; the resolution menu lists JPEG sizes that are clean binnings of the largest |
| `noise_modes`, `edge_modes` | NOISE_REDUCTION / EDGE modes available (off, fast, hq, minimal, zsl) | the menu's noise and sharpening columns; "default" keeps the template's |
| `iso_range` | SENSOR_INFO_SENSITIVITY_RANGE `[min, max]` (ISO) | bounds of the ISO wheel and of the app's own AE loop |
| `exposure_ns_range` | SENSOR_INFO_EXPOSURE_TIME_RANGE `[min, max]` | bounds of the shutter wheel; the upper bound is capped by `observed_shutter_max_ns` from the override |
| `min_focus_diopters` | LENS_INFO_MINIMUM_FOCUS_DISTANCE: the nearest focus, as 1/m | the manual focus steps stop there |
| `focus_distance_calibration` | LENS_INFO_FOCUS_DISTANCE_CALIBRATION: UNCALIBRATED / APPROXIMATE / CALIBRATED -- how much the HAL's "diopters" mean | if not CALIBRATED, measure (`docs/DEVICE-PROFILES.md`, the Focus cal. menu) |
| `af_modes` | AF modes (off, auto, macro, continuous video, continuous picture, edof) | the FOCUS button's modes; continuous enables "auto" |
| `min_frame_ns` | shortest frame duration per output (jpeg/raw16/yuv): the fastest the stream can run | frame rate ceilings; the RAW meter's cadence |
| `stall_ns` | extra time a JPEG/RAW16 capture stalls the stream | expected shot latency |
| `max_frame_ns` | SENSOR_INFO_MAX_FRAME_DURATION | longest single frame |
| `shading_map_size`, `shading_map_reportable`, `raw_shading_applied` | lens-shading (vignetting) map size, whether the HAL reports it, whether it is already applied to RAW | the DNG writer stores the map when reported and not applied |
| `ae_fps_ranges` | AE target frame-rate ranges `[min, max]` | the preview's range |
| `control_modes` | CONTROL_MODE values (off, auto, use scene mode, off keep state) | scenes need USE_SCENE_MODE |
| `session_keys` | keys the HAL reads when the session is CONFIGURED (Camera2 session parameters), not per frame | for vendor experiments: a key listed here may do nothing when set in a request |
| `vendor_chars` | the values of the vendor characteristics (tags >= 0x80000000, e.g. MediaTek's `available3dnrmodes` 0x80010001), up to 16 each | for vendor experiments: what a vendor key may take |
| `vendor_request_keys` | request keys outside the Camera2 standard (e.g. MediaTek's) | listed for information; RAWfish-indRAW uses none |
| `raw_capability` | whether the HAL declares the RAW capability | without it no DNG |
| `scene_modes` | CONTROL_AVAILABLE_SCENE_MODES with names | the Scene columns of the menu |
| `effects` | CONTROL_AVAILABLE_EFFECTS (mono, sepia...) | listed only; not offered (grouped processing) |

Vendor and effect lists are informative: RAWfish-indRAW drives the HAL with standard
keys only, so a port never depends on them.
