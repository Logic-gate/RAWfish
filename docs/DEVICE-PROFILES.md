# Device profiles (AI generated)

indRAW must run on more than the reference phone (Jolla Phone 2), so anything
that varies between devices lives in data, not in the code. The data is split
by **nature**, not by device -- this is the key difference from RAWfish, where a
single per-device "profile" mixes sensor facts, corrections and UI preferences,
so changing phone loses your UI choices and you can no longer tell what a value
means. indRAW keeps three natures strictly apart:

## 1. What the HAL claims  (read-only, per device)

A snapshot of the sensor's declared capabilities, written by `indraw probe` to
`generated-hal.json`. indRAW generates it and never edits it. It is reference
only: camera ids, sizes, ranges, modes, and the grouped controls (scenes,
effects) the device lists.

## 2. Corrections to those capabilities  (per device)

`override-hal.json` holds **only** corrections to sensor capabilities: fixing
what the HAL mis-declares, or adding facts found by testing (e.g. the real
long-exposure ceiling, which the HAL over-states). Every entry is **atomic** --
one named sensor parameter, one correction -- so you always know exactly what
changed. It never contains UI preferences. Field names follow RAWfish where the
concept is the same (e.g. `observed_shutter_max_ns`), so its author can read it.

Format (hand-written, or started by the probe; the CLI reads it; a commented
reference, `override-hal.example.jsonc`, lies next to it):

```json
{
  "cameras": [
    {
      "camera_id": "0",
      "overrides": {
        "observed_shutter_max_ns": 500000000
      }
    }
  ]
}
```

Corrections wired today (both optional, per camera):
- `observed_shutter_max_ns`: the real single-exposure ceiling, found by testing,
  which replaces the built-in 1/2 s stacking sub-frame maximum. On the JP2 the
  HAL claims 16 s but applies ~1/2 s, so a JP2 profile sets `500000000`.
- `min_frame_ns`: the minimum frame duration for manual exposure (the floor a
  manual shutter is clamped to). Default is 1/30 s (`33333333`); set this if the
  device accepts shorter or needs longer.

A third, richer correction is the **focus calibration table**: `LENS_FOCUS_DISTANCE`
is often uncalibrated (the JP2 reports `uncalibrated`), so a requested distance does
not match reality -- on the JP2, infinity is reached near `focus=0.5m`, not `inf`.
The table maps a real distance to the HAL diopter that actually reaches it; indraw
interpolates in diopters (optics is linear there), extrapolates beyond the ends, and
clamps to the lens's physical range. Measure it by shooting `scripts/focus-calib.sh`
at known distances and picking the sharpest frame. Each user can refine it for their
own device.

```json
{
  "cameras": [
    {
      "camera_id": "0",
      "overrides": {
        "observed_shutter_max_ns": 500000000,
        "min_frame_ns": 33333333,
        "peak_edge": 40,
        "peak_width": 2,
        "zebra_jpeg": 250,
        "zebra_raw": 992
      },
      "focus_calibration": [
        { "real_m": null, "hal_diopters": 2.000 },
        { "real_m": 5.0,  "hal_diopters": 2.299 },
        { "real_m": 2.0,  "hal_diopters": 2.597 },
        { "real_m": 1.0,  "hal_diopters": 3.497 },
        { "real_m": 0.5,  "hal_diopters": 4.808 },
        { "real_m": 0.25, "hal_diopters": 6.494 },
        { "real_m": 0.1,  "hal_diopters": 14.085 }
      ]
    }
  ]
}
```

Each entry is one measured point: `real_m` is the real distance in metres a target
was sharp at (`null` = infinity), and `hal_diopters` is the HAL's lens value
that was sharpest for it. The values above are the JP2's approximate starting
table. Without a table, manual distances use the uncalibrated 1:1 mapping and
the tool warns in the log.

### Calibrating the focus yourself, the simple way

You do not need to know any diopter. The idea: let the autofocus find the
sharp lens position for a target at a distance you know, and record that
position as "this is what D metres means on this lens".

1. Put a contrasted target (a page of text, a ruler) at a measured distance,
   say 0.5 m from the lens. A tape measure is all the equipment there is.
2. In the app, with the focus button on **once**, tap the target. Wait for
   the circle to become **solid** (sharp).
3. Pull the menu down and, in the **Focus cal.** column (shown when "Focus
   calibration in the camera menu" is on in Settings > Apps > indRAW), tap
   **0.5m**, then tap it again while it blinks red (the second tap confirms:
   a stray touch never overwrites a point). The
   engine records the lens value it has at that moment for 0.5 m, in
   `focus-calibration.json` next to the override file, and applies it at once.
4. Repeat for the other distances you care about (0.25 m, 1 m, 2 m, 5 m, and
   `∞` on something far away). Three or four points are enough; the manual
   focus wheel then interpolates between them. One point alone already
   shifts the whole scale by a constant; with two or more, straight lines in
   diopters between the points, extended past the end ones, so every
   distance is covered.
5. A wrong point is replaced by recording the same distance again; **clear**
   forgets the assistant's points (those in `override-hal.json` stay).

Points in `focus-calibration.json` override the same distances in
`override-hal.json`; the merged table is what the engine uses. The command
line equivalent is `calib D` (or `calib inf`, `calib clear`) on the running
engine's stdin. The old sweep method (`scripts/focus-calib.sh`, shooting a
series of DNGs around each distance and judging sharpness) is still there for
the demanding.

`indraw probe` writes a starter `override-hal.json` when there is none: one
block per camera the phone has, every field the engine reads with its
built-in default, and next to the fields that correct a HAL claim the HAL's
own value as a reference (`_hal_exposure_max_ns`, `_hal_min_frame_ns`; keys
starting with `_` are never read). It is deliberately not a copy of
`generated-hal.json`: the two files hold different things (what the HAL
claims, what you measured), and copied claims would silently mask the real
ones after a system update. Delete a field or a camera block to go back to
the defaults.

The engine checks the file at every camera open and writes its findings to the
log (`~/.cache/com.indraw/indraw/indraw-ui.log`, lines starting with `#`):
an unknown key (a typo such as `peak_egde`) is reported and ignored; a value
outside its range (e.g. `peak_edge` 1..255, `observed_shutter_max_ns`
1 ms..60 s) is reported and the default kept; unbalanced braces or quotes make
the whole file ignored, so a half-edited file never applies half its values;
a camera without a block is reported (defaults for it).

**A commented reference.** The app writes `override-hal.example.jsonc`
next to `override-hal.json` at each start: every key with what it does,
its values and limits, as `//` comments. It is a reference only: with the
comments it is not valid JSON and the engine never reads it. Copy the lines
you need into your own file, without the comments: the real file stays
plain values.

**Where the files come from.** `override-hal.json` (with a starter block
per camera, the HAL's claims as `_hal_` references) and
`generated-hal.json` are written by the probe: Settings > Apps > indRAW >
"Create the device profile files" (run at the next start of the app), or
`indraw probe` in a terminal. The starter is written only when no
override-hal.json exists, so deleting it and running the probe gives it
back; the app's normal start does not.

**Shipped device profiles (0.47.57).** indRAW ships one profile per camera
module in `/usr/share/indraw/device-profiles/` (source:
`android/data/device-profiles/`), the same format as `override-hal.json` plus:
- `"match": { "make": "Sony", "models": [...], "raw": [W, H] }`: the phone is
  matched by make and model (any case); if no model matches, by make and the
  camera's RAW size -- one camera module, several model names;
- `"notch": true | false`: the screen's top cutout, for the interface (else
  it guesses from the screen's proportions);
- per camera, `"shading_file"`: the lens-shading calibration's file name for
  every variant of the module (looked for after the name built from this
  phone's make and model).
Every key is read from the user's `override-hal.json` first, then from the
shipped profile, then the built-in default: the user's file only needs what
differs. The log says which profile was matched ('# device profile: ...').
check-host.sh validates the shipped profiles (JSON, match block, known keys).

**What the probe reports (0.47.53).** Besides the sizes, ranges and modes,
`generated-hal.json` now gives, per camera: `hardware_level` (legacy /
limited / full / 3), `capabilities` (manual_sensor, manual_post_processing,
raw, read_sensor_settings, burst_capture...), `request_keys` (the standard
controls a request may carry, named: control.afMode, sensor.exposureTime,
noiseReduction.mode...) and `result_keys` (what a capture result reports).
These three say what the HAL will take and tell back, and are the starting
point of a port: a control absent from request_keys is ignored or refused.

**Lens shading (vignetting).** A measured calibration per phone model and
camera, `shading_<maker>-<model>_cam<ID>_<W>x<H>.json`, in
`device-profiles/lens-shading/` (or shipped with the engine in
`/usr/share/indraw/lens-shading/`; the JP2's is):
each DNG then gets a gain map for its own lens position and light instead of
the HAL's partial one. The camera block's `shading` key: `measured`
(default), `hal`, `none`. Measurements, model and file: `docs/LENS-SHADING.md`.

**Vendor (chipset) request keys.** A camera block may carry a
`vendor_request` list: keys outside the Camera2 standard (the ones
`generated-hal.json` lists as `vendor_request_keys`, by hex id) to set on
the requests at open. Each entry: `tag` (hex id, `0x8000xxxx`), `type`
(`u8`, `i32`, `float` or `i64` -- the key's type, from the chipset's
documentation), `value`, and `on` (`preview`: the viewfinder requests;
`photo`: the capture requests; `all`). The engine applies them without
interpreting them, and the log says whether the NDK accepted each one (a
key the camera service has not published is refused). Accepted is not
applied: a moment later the log adds what the preview RESULTS report for
each key ("# vendor_request in the results"): the value sent means the HAL
echoes it; another value, or "absent", means it ignores or overrides it.
`generated-hal.json` lists `session_keys` (keys read only when the session
is configured: setting them per request may do nothing) and `vendor_chars`
(the values of the vendor characteristics, e.g. `available3dnrmodes`).
What was tried, with numbers and conclusions (3DNR, MFNR on the JP2):
`docs/VENDOR-TESTS.md`, which also gives the override line to keep the
MFNR in auto. Such keys belong to
one chipset: they live here, never in the code. Example, the first thing
to try on the JP2's viewfinder "maze" (ROADMAP.md), MediaTek's temporal
noise reduction switch:

```json
"vendor_request": [ { "tag": "0x80010000", "type": "i32", "value": 0, "on": "preview" } ]
```

Both files can also be created from the phone: the "Create the device profile
files" button of Settings > Apps > indRAW asks the app to run `probe` at its
next start. The override is applied right after the camera opens; the tool
prints what it applied. Field reference for the generated file:
`docs/GENERATED-HAL.md`.

```json
{
  "cameras": [
    {
      "camera_id": "0",
      "overrides": {
        "observed_shutter_max_ns": 500000000,
        "min_frame_ns": 33333333
      }
    }
  ]
}
```

## 3. UI / UX preferences  (device-independent)

Menus, toggles, display choices. These belong to the Qt app, in its own file,
and are **never** mixed with sensor data. Changing phone must not change your UI
preferences, and changing UI preferences must not touch the hardware data. The
app keeps them in `~/.config/com.indraw/indraw/indraw.conf` (and a few in dconf,
from its Settings page; `indraw.conf.example` describes every key); the CLI
tool has no UI.

## Everything asked of the HAL is atomic

indRAW never drives a *grouped* control that reconfigures several things at once:
no scene mode, no effect, no global "auto" that overrides the atomic settings.
On open, every request is set to `CONTROL_MODE = AUTO` with `SCENE_MODE =
DISABLED` and `EFFECT_MODE = OFF`, so a scene or effect can never silently change
noise, edge, colour or exposure behind our back. Only atomic controls (noise
reduction, edge, white balance, exposure, ISO, focus) are driven and reported,
so every test says exactly what was applied. Scenes and effects are *listed* by
`caps`/`probe`; the app shows the scenes only if its Settings page asks
(off by default), and never uses the effects.

## Location

Following the Sailjail sandbox (which whitelists
`$HOME/.local/share/<OrganizationName>/<ApplicationName>` and sets
`XDG_DATA_HOME` inside the jail), the files live in:

    $XDG_DATA_HOME/com.indraw/indraw/device-profiles/      (inside the sandbox)
    $HOME/.local/share/com.indraw/indraw/device-profiles/  (fallback)

`INDRAW_PROFILE_DIR` overrides both, for tests. The CLI tool reads
`XDG_DATA_HOME` first, which is what Qt's `QStandardPaths::AppLocalDataLocation`
resolves to, so the CLI and the app agree on the path.
