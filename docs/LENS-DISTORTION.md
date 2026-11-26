# (AI generated)
# Lens distortion: measured, shipped, corrected in the viewfinder and the JPEGs

## What it does

With the setting on (the pull-down menu's **Lens fix**, or `--lensfix on` /
`lensfix on` on the command line), straight lines stay straight: the
viewfinder and every JPEG are remapped to what a distortion-free lens would
have drawn. It matters most on the JP2's wide camera (strong "mustache"
distortion: a slight pincushion in the middle, a strong barrel in the
corners); the main and front cameras bend much less. DNGs are never
touched: RAW stays the sensor's.

The corrected picture is a pinhole camera with square pixels and the same
optical centre, at the narrowest focal length that leaves no empty corner
(OpenCV's `alpha = 0`): it crops 1 to 2 % on the JP2's cameras. The crop
is fitted on the format's whole frame, so the viewfinder shows exactly what
the photo will, and a zoom or the loupe shows the same corrected geometry.

## The file: one per phone model and camera

`distortion_<maker>-<model>_cam<ID>.json`, named after the phone
(`ro.product.manufacturer` / `ro.product.model`, slugged as the lens
shading files are) so that one file serves every phone of the model. Looked
for in the user's `device-profiles/lens-distortion/` first, then in the one
shipped with the engine (`/usr/share/rawfish-ir/lens-distortion/`, from
`android/data/lens-distortion/`).

A camera module sold under several model names (the XA2's H3113, H4113...,
the 10 III's XQ-BT52, XQ-BT41) would need one file per name: instead, its
device profile names one file for all of them, per camera, as it does for
the lens shading (`"distortion_file"`, `DEVICE-PROFILES.md`). The order, as
for the shading: the user's file under the phone's own name, the user's
under the profile's name, the shipped one under the profile's name, the
shipped one under the phone's own name. `--lensfix FILE` / `lensfix file=`
bypass all of it. Without one, nothing is corrected and the
CLI warns (`lensfix state=unavailable`; the menu's header says so).

ONE model per camera serves every output size: the 4:3 and 16:9 formats,
12 MP and 50 MP, the viewfinder at any zoom. The model refers to the
camera's full frame (the largest 4:3 size, which covers the active array);
every frame the camera produces shows a known rectangle of that array (its
crop and its shape, `viewfield.h`), so its correction follows. Checked on
the JP2 (25 Nov 2026): each 16:9 folder of samples, fitted alone, gives the
same focal length as the joint model within 0.4 %, the 3280x2464 binned
selfie photos within 0.1 %.

Contents (see `android/lensdist.h`):

| Key | |
|---|---|
| `format` | `"rawfish-ir lens distortion 1"` |
| `width`, `height` | the frame the matrix refers to (the camera's full frame) |
| `fx`, `fy`, `cx`, `cy` | OpenCV's camera matrix for that frame (pixel centres at integers) |
| `dist` | OpenCV's coefficients `[k1, k2, p1, p2, k3, k4, k5, k6]` (4, 5 or 8) |
| `make`, `model`, `camera_id`, `name`, `fit`, `generated` | for people: the phone, the fit's error and coverage, the samples |

The model is OpenCV's rational one: for an ideal ray (x, y), r² = x² + y²,

    a  = (1 + k1 r² + k2 r⁴ + k3 r⁶) / (1 + k4 r² + k5 r⁴ + k6 r⁶)
    xd = x a + 2 p1 x y + p2 (r² + 2 x²),   yd = y a + p1 (r² + 2 y²) + 2 p2 x y

A file whose radial curve folds back inside the frame, or that would need
more than 25 % of crop, is refused (a bad fit).

## Measuring a camera (maintainers, on a PC)

Step by step, with examples: [LENS-CALIBRATION.md](LENS-CALIBRATION.md). In short:

`scripts/lens-calibrate.py` (Python 3, `opencv-python`, `numpy`; OpenCV is
never needed on the phone):

1. Print a chessboard (the default: 7 x 10 squares of 23 mm; `--board`,
   `--square-mm`), glue it flat on something rigid.
2. With RAWfish-ir, the lens fix OFF, take 15 to 30 photos per format of
   the camera: the board filling much of the frame, in the corners and
   along the edges above all, at various angles; portrait or landscape. One
   folder per format is handy (photos of several sizes in one folder are
   grouped by size).
3. Run, with the camera's id (`rawfish-ir-cli cameras`; on the JP2:
   0 main, 1 front, 2 wide):

       scripts/lens-calibrate.py --camera-id 2 --name wide samples/wide/43 samples/wide/169

   It writes `android/data/lens-distortion/distortion_jolla-jp2601_cam2.json`
   (maker and model from the photos' Exif) and reports the fit: the error
   per folder in its own pixels, how far into the corners the samples
   reach (aim at 90 % or more), each folder fitted alone (a few tenths of
   a per cent off the joint model is normal; more than 2 % is flagged).
4. Check it on a photo, on the PC or the phone:

       rawfish-ir-cli lensfix android/data/lens-distortion/distortion_jolla-jp2601_cam2.json IN.jpg OUT.jpg

5. Commit the file: the package installs every file of the folder.

Photos whose fit is more than twice the median error (a bent board, motion
blur) are dropped and the fit repeated.

## Where the correction runs

| What | Where | How |
|---|---|---|
| the viewfinder | engine (`android/rawfish-ir.c`, `lens_fix_view`) | each frame SENT is remapped through a per-pixel table (bilinear), rebuilt when its crop changes (zoom, loupe). The viewfinder meter and the white pick measure the frame as taken, before the remap (their boxes map from the sensor into it); a tap on the corrected picture is mapped back to the lens' point (`indraw_tap`) |
| our JPG (from the YUV stream) | CLI (`cmd_yuv`) | the frame is corrected after the merge and the noise filters, before the sharpening (which then works on the final geometry; the noise it must exceed is measured on the frame as taken): Y bicubic, U and V bilinear, on every core; compressed once. Exif Software says "lens corrected" |
| the camera's JPEG (HAL: `jpeg`, `burst`) | CLI (`lensfix_camera_jpeg`) | after the engine wrote it: decoded (4:2:0 as it is), corrected, compressed again at the photo's quality; its Exif (every tag) and other metadata segments kept, a new Exif thumbnail made from the corrected picture. Its `written` event is held by the controller until the file is final, so the gallery never shows the uncorrected one |
| DNG | -- | never: the developer has the RAW as the sensor gave it |

The model is loaded by the engine at each camera start
(`indraw_lensdist_load`, bound to the active array) and handed to the CLI
(`indraw_lensdist_get`); the photo's field comes with the YUV frame
(`ir_yuv_t.field`) or from `indraw_photo_field` for the camera's JPEG.

Timings on the JP2 (aarch64, all cores): a 12 MP camera JPEG decoded,
corrected, compressed and written in 0.2 s, a 50 MP one in 1.3 s; our JPG
saves the decode.

## Commands and events

| | |
|---|---|
| `--lensfix on\|off\|FILE` | the setting at start; FILE: this file instead of the phone's (testing a new calibration) |
| `lensfix on\|off` | the setting, at any time (the app sends it at every start and from the menu) |
| `lensfix file=PATH` | load another file now (and turn the fix on); `file=` back to the phone's |
| `lensfix status` | `{"lensfix":"on\|off\|unavailable","profile":true\|false}` |
| event `lensfix state=on\|off\|unavailable` | after each change; `unavailable`: asked, but no profile for this camera |
| `rawfish-ir-cli lensfix PROFILE IN.jpg OUT.jpg [q=95] [rot=0\|90\|180\|270]` | a JPEG corrected offline, no camera, also on a PC (the JPEG taken as the camera's full frame or a centred crop of it). `rot=`: the pixels were turned clockwise by it and carry no Exif orientation -- a camera driver's JPEG, or a photo whose Exif was stripped (sent through a forum): `rot=90` for a back camera held upright |

## Tests

`android/tests/lensdist_test.c` (the file, the binding, no empty corner,
the inverse, 16:9 and zoom, the viewfinder table, every shipped file) and
`sailfish/tests/lensfix_test.c` (a dot lands where the model says, in a
YUV frame and through a JPEG; the Exif, its orientation and manual tags
kept; a new thumbnail; turned pixels refused) run in `scripts/check-host.sh`,
under AddressSanitizer and UBSan.

## Limits

- A camera JPEG whose pixels the driver turned upright (instead of an Exif
  orientation) is not corrected: the model is the sensor's (logged).
- The camera JPEG is compressed twice (decode, correct, encode at the
  photo's quality); our JPG, the app's default, once.
- The correction is the one at the lens' focus when the samples were
  taken; distortion barely changes with focus on phone lenses.
