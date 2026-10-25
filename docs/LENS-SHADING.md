# (AI generated)
# Lens shading (vignetting): measured, modelled, written into the DNG

## What was measured (Jolla Phone 2, October 2026)

Flat fields through a diffuser, RAW (indRAW 0.47.32), manual focus at known
lens positions, a 12 x 16 grid (edge-anchored), the gain that makes each
point equal to a 6 % patch at the centre. Repeatability between frames of a
position: better than 0.5 %.

**Daylight** (23 Oct, ISO 100, 1/100 s, 3 DNG per position) -- the HAL's lens
value is LENS_FOCUS_DISTANCE, the lens POSITION (its scale: infinity = 2.27):

| HAL lens value (focus) | gain needed, corners (green) | colour in the corners R/G, B/G | the HAL's map, corners | what the HAL's map leaves |
|---|---|---|---|---|
| 2.27 (infinity) | 4.31 | 1.13, 0.98 | 2.71 | 0.63 |
| 2.37 (5 m) | 4.28 | 1.14, 0.98 | 2.71 | 0.63 |
| 2.85 (2 m) | 4.24 | 1.14, 0.98 | 2.71 | 0.64 |
| 4.56 (0.5 m) | 4.10 | 1.14, 0.98 | 2.71 | 0.66 |
| 7.80 (0.25 m) | 3.85 | 1.14, 0.98 | 2.71 | 0.70 |
| 15.18 (0.10 m) | 3.37 | 1.15, 0.97 | 2.71 | 0.81 |

**Tungsten** (22 Oct, ISO 200, 1/25 s, 4 DNG per position): the same
luminance shape within 5 % (4.42 at infinity, 3.49 at 0.10 m), but red in
the corners 23-25 % above green; the HAL's map 2.44, constant, its red 1.10.

Conclusions:
- the LUMINANCE falloff depends on the lens position (22 % less gain at the
  closest focus than at infinity; almost none from infinity to 5 m), hardly
  on the light;
- the COLOUR shading depends on the light (red +13 % in daylight, +23 %
  under tungsten), hardly on the focus;
- the HAL's map follows the light, never the lens position, and corrects
  only part of the falloff (corners left at 55-81 % of the centre) and,
  under tungsten, part of the red.

(A daylight series taken with RAWfish on 30 Sept gave other values -- 3.50
in the corners, red BELOW green -- for a reason not established: its lens
position is unknown (the AF had failed on the diffuser) and RAWfish shot in
the HDR scene mode. Not used.)

## The model and the file

`device-profiles/lens-shading/shading_<maker>-<model>_cam<ID>_<W>x<H>.json`,
named after the phone (ro.product.manufacturer / ro.product.model) so that
users of the same model can exchange it; the engine also ships one per known
phone (`/usr/share/indraw/lens-shading/`, the JP2's built from the two
series above); the user's file wins. Contents (see `android/shading.h`):

- `luminance`: a green gain grid per lens position, keyed by the HAL's own
  lens value (`hal_diopters`) -- not a real distance, so no focus
  calibration is needed and it is the same for every phone of the model;
- `colour`: per light, keyed by its colour temperature as the engine
  estimates it from the HAL's neutral, the factors for R, G and B relative
  to the daylight luminance at infinity.

For each DNG the engine reads the photo's lens value and the HAL's neutral
(its colour temperature), interpolates the luminance linearly in lens value
and the colour in mireds, multiplies them, clamps to [1, max gain], applies
the pivot and strength, and writes the result as the DNG's GainMap instead
of the HAL's. Without a file: the HAL's map, as before. The override's
`shading` key: `measured` (default), `hal`, `none`. Nothing is baked into the
pixels: the developer applies the map.

## The user's dosing (indraw.conf)

From Ric9k's RAWfish calibration script: `shadingPivot` (0..100, default 50;
0 only brightens the edges, 100 only darkens towards the darkest corner --
darktable clips values pushed above white, so a pivot keeps bright edges
from clipping; raise the exposure in darktable), `shadingStrength` (0..100,
100), `shadingMaxGain` (1..8, 5).

## Building a calibration (android/shading_build.h)

`shading_build.h` turns flat-field DNGs into the file above, the way the
on-phone assistant does (and a PC can): `shading_read_dng()` samples a
DNG's four CFA planes on the R x C grid (edge-anchored, a window half a
cell, a 6 % centre reference), turning levels into gains and flagging
clipping and an uneven (left/right) field; `shading_build_assemble()`
averages the captures of each lens position (luminance, green) and each
light (colour, plane / green), laying them out as a `shading_cal_t`;
`shading_write_file()` serialises it. Tested
(`android/tests/shading_build_test.c`, run by check-host.sh) by writing
synthetic flats with the real `dng_write()`, reading them back, assembling
and round-tripping through `shading.h` -- which also checks the DNG
writer's pixels and GainMap.

## The capture command (0.47.38-0.47.45)

`shading calibrate full cct=K lens=D1,D2,...` measures the luminance at the
given HAL lens values and the colour at light K; `shading calibrate light
cct=K` adds a light to the existing file (colour only, one position). It
settles the exposure first (centre near 500 DN, not clipped, starting at
ISO 400 / 1/33 s and scaling time then ISO) and uses that one pair for every
frame; refuses a clipped or too-dark frame, one whose inner corners fall
below 8 % of the centre, a frame whose centre strays more than 5 % from the
first frame of its lens position, or a position whose first frame strays
more than 15 % from the previous position's (shading_level_check: the
frames of a position agree within 0.5 %, the falloff moves the centre 3-7 %
per step); emits `cal_exposure`, `cal_progress done= total=` and `cal_done ok=
path=|why=`. A full calibration moves any existing file into previous/ with
a timestamp first; adding a light edits in place. Validated on the JP2
(24 Oct 2026): a full on-phone run matches the hand-built 23 Oct reference
within 1-2 % in luminance and colour.

Bugs found on the way, all covered by tests now: the CLI's focus mode
number for 'rawD' (8, not 9: the lens never moved); the event channel not
attached in run mode (no progress in a terminal); BlackLevel read as an
inline value when the engine stores four SHORTs at an offset (a black of
570: every corner "too dark"); the colour factors carrying the light's
white balance (normalised to the centre now).

## The assistant (ui 156)

Settings > indRAW > "Calibrate the lens shading" sets a request; at its
next start the camera page pushes `ShadingAssistant.qml` over itself (the
engine keeps running and the page drives it). Step 1 prepares (what is
needed; full calibration or add a light; the light's kind = its colour
temperature, the file's key) and insists that nothing but the lens must
move during the whole measurement. Step 2 runs the command: six lens
positions spread over the HAL's range (0 = infinity to the closest the
caps report), three captures each, a progress bar and the settled
exposure. Step 3 shows the written path or the reason, and returns to the
camera, restarting the engine so the new file is loaded.
