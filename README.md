## indRAW (air input, boats, engines...)


## Human description

indRAW is RAWfish, deconstructed, reorganized, cleaned, reconstructed.
Transitional version meant to be reviewed and merged with some RAWfish parts, once all functions and UI are brought in.
Coded by IA under the supervision of and at the request of the creators.
Supervision and all the tests made by humans.
No human code review yet. Audits by other sessions and other AI models.

### Architecture
Camera CLI engine + Silica UI for SailfishOS.
Built on Android's Camera2 through libhybris.
-Command line engine (talks to the HAL) (docs/ENGINE.md)
-Controller (clculations)
-UI (user requests, feedback, settings) (docs/USER-GUIDE.md)
-OS Settings page
-config files (txt+json)

### Features
JPG
RAW (DNG)
Long exposure up to 8 minutes (stacking, experimental)
Manual controls (focus, exposure, white balance, noise, sharpening...)
Hystogram, spot measurement
Loupe for exposure and focus
Bracketting (3x same EV, +1, +2, +3 )
User lens shading calibration tool, color, luminance, focus adapted (DNG exif)
User focus calibration tool (factory uncalibrated)
User click-to-WB (HAL)
Zoom (digital)
Flash
Peaking
Zebra
Spirit level
Gallery DNG previews
Sailjailed

No GPS
No network/radio access


### Actual Stage
Currently tested on the JP2 (more) and the XA2 (less).
Testing are also being conducted on Xperia 10 IV Community port.

Beta version. We still have some bug hunting work + Ui/functions improvements to do.

indRAW is a rebuild based on RAWfish, itself a fork of Jolla Camera.
Nothing would have been possible without Mad_Dev's big work on how to make raw pics with the Jolla Phone 2 and it's IMX766 sensor.

## References, credits
https://github.com/sailfishos/jolla-camera
https://github.com/Logic-gate/RAWfish

Discussion:
https://forum.sailfishos.org/t/next-gen-native-camera-app-rawfish/34150



---------------------------------------------------------------------------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------------------------------------------


## LLM description

indRAW

A camera for Sailfish OS built on Android's Camera2 through libhybris: fast JPEG, true RAW
(DNG) with manual shutter / ISO / focus, a strictly WYSIWYG viewfinder, and an engine that only
ever drives atomic controls and reports what the hardware really applied. Reference device: the
Jolla Phone 2 (2026); also runs, as a proof of concept, on the Sony Xperia XA2 (32-bit); other
devices need porting (see "Device profiles", `docs/PORTING.md`).

Two parts: the **engine** (`indraw` CLI + `libindraw.so`, this README) and the **app**
(`app/`, Qt/Silica, see `docs/UI-NOTES.md` for its behaviour specification). The app's layout is
modelled on [RAWfish](https://forum.sailfishos.org/) (a Jolla Camera fork), whose SF2P/SF2M
frame protocol it reuses; what it deliberately does differently is listed in `docs/UI-NOTES.md`
("vs RAWfish"). Development relies on AI assistance (code, debugging, research), every change
being reviewed and tested on the device by a person; contributions are welcome on the same terms.
License: BSD-3-Clause (`LICENSE`); lineage and credits in `CREDITS.md`; what changed when in `CHANGELOG.md`.
The project was called **rawbar** until October 2026: the name survives in the history (`patches/`,
old logs) and nowhere else.

## Engine

Complete command reference (every option, range and exit code, with examples): `docs/ENGINE.md`.
Using the app: `docs/USER-GUIDE.md`.

Goals: fast JPEG, quality RAW with manual speed / ISO / focus.

```
sailfish/indraw (glibc)  --libhybris-->  libindraw.so (bionic, NDK Camera2)  -->  HAL
```

The Android library keeps ONE session open: a small YUV preview runs continuously so 3A
converges, and a JPEG output sits on the same session, so a photo is a single request.

## Output files

Captures are written to `~/Pictures/indRAW/` as `NAME_YYYYMMDD_HHMMSS_mmm.EXT`: the command's
first argument is a name prefix, and indraw always appends the capture timestamp (milliseconds
so a burst never collides) and the extension (`.jpg`, `.dng`). DNGs carry the capture time in
EXIF (`DateTimeOriginal`). (`~/Pictures` is outside the Sailjail auto-whitelist: the CLI runs
unsandboxed, the app declares the Pictures permission.)

## Build

New to this? `docs/BUILDING.md` walks through every step, from installing the SDK and NDK to
rebuilding from zero. The short version: `./scripts/build-all.sh` makes both RPMs.

Two RPMs, installed on the phone with `rpm -Uvh` (as root):

```sh
ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-engine-rpm.sh   # RPMS/indraw-engine-X.Y.Z-1.aarch64.rpm
cd app && sfdk build                                           # app/RPMS/indraw-app-X.Y.Z-1.aarch64.rpm
```

`indraw-engine` carries `libindraw.so` (prebuilt with the NDK, installed to
`/usr/libexec/droid-hybris/system/lib64/`) and the `indraw` CLI (`/usr/libexec/indraw/indraw`,
plus `/usr/bin/indraw`); `indraw-app` requires it. The version is `indraw_version.h`, shared by
the library, the CLI and the spec; the CLI refuses to run with a library of another version.

Install by hand (development): copy `build/android/libindraw.so` to
`/usr/libexec/droid-hybris/system/lib64/` (as root) and `sailfish/indraw` to
`/usr/libexec/indraw/indraw` (mode 755) and/or a working directory. Inside the Sailjail
sandbox `/usr/bin` only holds the app binary, so the app launches the engine from `/usr/libexec`.

```sh
ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-android.sh   # build/android/libindraw.so
./scripts/build-sailfish.sh                                # sailfish/indraw (sfdk workspace)
./scripts/check-host.sh                                    # PC: compile check only
```

## Use (on the phone, camera app closed)

```sh
./indraw caps 0       # sizes, noise/edge modes, ISO/speed/focus ranges, per-size timings, shading info
printf 'jpeg /tmp/a.jpg q=95 off_noise\njpeg /tmp/b.jpg q=90 minimal_noise off_edge focus=continuous\nquit\n' \\
    | ./indraw --log ~/indraw.log run 0 8192x6144
```

`run` prints one JSON line when the camera is open (`ms.open`, `ms.session`, `ms.first_frame`),
then one per photo (`3a` = AF lock + AE precapture outcome when `focus=auto`, `ms.3a` its duration,
`ms.image` = request to image, `ms.written` = file on disk, `bytes`,
`applied` = exposure/ISO/AE/AF/noise/edge the camera reports it really used), then one on close. `--log FILE` appends all of it, with the commands and timestamps.

Exit codes: 2 usage, 10 libhybris, 12 bridge library, 14 libdroidmedia (binder thread pool), 20 bridge error.

Mode names always carry their family: `hq_noise` (NOISE_REDUCTION_MODE_HIGH_QUALITY) vs `hq_edge` (EDGE_MODE_HIGH_QUALITY).

## RAW16

```sh
printf 'raw b-raw.raw16\njpeg b-denoise_off-sharp_off.jpg off_noise off_edge\nquit\n' \
    | ./indraw --log indraw.log run 0 8192x6144 raw=4096x3072
python3 scripts/raw16-check.py b-raw.raw16 [--shading]     # on a PC: levels, WB, shading, PNG
```

`raw NAME` writes a ready-to-develop DNG directly on the phone (same format as
`raw16-to-dng.py` below: lens shading GainMap, colour calibration, white balance, noise profile,
EXIF). `raw FILE` with any other extension writes the raw pixels (uint16 little-endian,
width x height, no padding) and `FILE.json`:
layout, static sensor/lens facts (CFA, black/white levels, colour matrices, shading map size,
`SENSOR_INFO_LENS_SHADING_APPLIED`) and the per-frame result (exposure, ISO, WB gains,
neutral point, noise profile, lens shading map when the camera reports it).

File names: fields separated by `-`, each field `name_value` (e.g. `m-denoise_zsl-sharp_hq-q90.jpg`).

## Manual exposure

`jpeg` and `raw` accept `exp=T` (seconds: `16`, `0.5`, `1/125`) and `iso=N` (default: lowest).
AE is turned off for that photo only, the frame duration is stretched to fit the exposure,
and the report shows `requested` next to `applied` (`exposure_ns`, `frame_ns`, `iso`).
`scripts/test-exposure.sh` (run on the phone) checks 1/1000 s to 16 s and that 20 s is refused.

## Long exposure diagnosis

On the JP2 the HAL accepts exposures up to 16 s and echoes them in its results, but the RAW
brightness shows a real exposure under 1 s. So exposure is now **measured**: every `raw`
report has `measured.center_mean` (black-subtracted mean of the central quarter) and
`preview_frames_during` (a real long exposure stalls the preview). `raw -` measures without
writing anything. Variants for `jpeg`/`raw`: `preview=pause` (stop the repeating preview during
the capture), `fps=low` (lowest AE target FPS range), `control=off` (CONTROL_MODE OFF).
`caps` lists `ae_fps_ranges`, `control_modes` and the vendor (MediaTek) request keys.

`scripts/test-exposure-sweep.sh` (phone) runs 1/30 s .. 16 s at constant exposure x ISO for each
variant and prints one summary line per shot: a flat `mean` means the exposure is real.

## Long exposure by stacking

The HAL applies a real single exposure only up to ~1/2 s; longer requests are wrong or never
deliver (measured: 1/2 s exact, >=1 s capped, confirmed in the MediaTek 3A library, which trips
an internal `SpecialLongExp` mode above a fixed threshold with no Camera2 control to drive it).
So long exposures are stacked:

```sh
printf 'stack night-16s.stack total=16 sub=0.5 iso=100\nquit\n' \
    | ./indraw --log indraw.log run 0 4096x3072 raw=4096x3072
python3 scripts/raw16-check.py night-16s.stack     # handles the uint32 sum
```

`stack NAME film=T [sub=T] [stk_denoise=1|2|4] [iso=N]` builds a film-equivalent long exposure by
stacking RAW sub-frames (the HAL caps a real single exposure near 1/2 s). `FILE.dng` writes a DNG;
any other extension writes the raw uint32 sum + `FILE.json`. AF is turned off for the whole burst,
so the frames share one focus.

It is a lossless **brute sum**: N sub-frames are added in 32-bit, and the DNG's black/white levels
are scaled by the frame-equivalent, so the exposure stays truthful and no precision is thrown away.

- **`film=T`** sets the **brightness**, like a classic T-second exposure. The base frame count is
  `T / sub` (so `film=16 sub=0.5` -> 32 frames). The written black/white levels become `black*N`
  and `white*N`.
- **`sub=T`** is each sub-frame's exposure (default and max 0.5 s). Shorter sub-frames clip
  highlights later, at the cost of more frames for the same `film`.
- **`stk_denoise=1|2|4`** stacks that many times the base frames and rescales back to `film`'s
  brightness, cutting noise by a further `sqrt(stk_denoise)` without changing brightness.

Film-equivalent brightness (`sub` 1/2 s), noise falls as sqrt(total frames):

```
film   base frames   stk_denoise=1    stk_denoise=2    stk_denoise=4
                      frames / noise   frames / noise   frames / noise
 1 s       2          2  / x1.4        4  / x2.0        8  / x2.8
 2 s       4          4  / x2.0        8  / x2.8        16 / x4.0
 4 s       8          8  / x2.8        16 / x4.0        32 / x5.7
 8 s      16          16 / x4.0        32 / x5.7        64 / x8.0
16 s      32          32 / x5.7        64 / x8.0        128 / x11.3
```

Known limits: ~0.2 s gaps between frames (bright moving points become dotted trails); without a
tripod the frames are not aligned (no registration yet).

## DNG

```sh
python3 scripts/raw16-to-dng.py m-raw.raw16            # -> m-raw.dng
python3 scripts/raw16-to-dng.py night-16s.stack        # a stack works too
python3 scripts/raw16-to-dng.py m-raw.raw16 --no-shading
```

Wraps a `raw` frame (uint16) or a `stack` (uint32, scaled back to 16 bits) and its `.json` into
a DNG, no libtiff. The phone writes the same DNG directly (`raw NAME` / `stack NAME`);
this script is for reconverting raw16/stack files captured earlier. It carries black/white levels, the BGGR pattern, both colour calibrations
(matrix + forward matrix), the as-shot white balance, the noise profile, EXIF (exposure, ISO,
focal length, aperture), and the lens shading map as an `OpcodeList2` GainMap so the developer
removes the vignetting (the RAW itself is not corrected; `--no-shading` omits it). The GainMap is written byte-for-byte like RAWfish's on-device encoder (its
`writeGainMapOpcode`), the layout darktable actually reads on this phone: in `OpcodeList2`, with
region Top/Left/Bottom/Right as uint32 pixels (Bottom/Right = height/width), MapSpacing/MapOrigin
as float64, and the gains as float32. This mixed int/float64/float32 layout is not obvious from
the spec text and was taken from that source. indraw targets the format the installed tools apply,
not the newest spec (1.7.x), which installed versions would not read.

## Noise profile (darktable)

```sh
printf 'noiseprofile jolla.json\nquit\n' | ./indraw --log indraw.log run 0 4096x3072 raw=4096x3072
```

`noiseprofile FILE.json [sub=T]` sweeps ISO (100, 200, 400 ... up to the sensor maximum), reads
the HAL's `SENSOR_NOISE_PROFILE` at each step and writes a darktable `noiseprofiles.json` for this
phone (maker/model from `ro.product.*`, so it matches the DNG). The scene does not matter -- the
sensor measures its own noise. The file's `comment` says where to install it (on Linux,
`~/.config/darktable/`); after a restart, darktable's *denoise (profiled)* module offers this
camera's per-ISO profile. The report lists the a/b coefficients per ISO, so you can see whether the
HAL returns real per-ISO values or a fixed model. The gain-channel-to-colour mapping follows the
sensor's CFA, so it is correct on any phone. (A later indraw settings page will have a
"generate noise profiles" button for this.)

## Exif in the JPEGs

The HAL's JPEG carries an Exif segment without ISO or exposure (JP2). The engine replaces it with
its own (`android/exifwrite.c`): Make/Model/Software, DateTime and DateTimeOriginal, ExposureTime
and ISO **as the HAL reported having applied them** (the capture result matched to the image; a
missing result leaves the tags out rather than guessing), FNumber, FocalLength and the 35 mm
equivalent, and the Orientation tag copied from the HAL's segment (the HAL may rotate pixels or set
the tag; we keep its choice). No thumbnail, no maker notes.

## Live exposure for the UI (stream mode)

```sh
printf 'exposure exp=0.008 iso=400\nev 2\nmeter on rate=5\nexposure auto\nmeter off\nquit\n' \
    | ./indraw --log indraw.log stream 0 4096x3072 raw=4096x3072 > /dev/null
```

- `exposure auto | exp=SEC iso=N` sets the viewfinder's exposure live: the HAL's AE, or a
  fixed pair (AE off) applied to the repeating request. Priority modes are composed by the UI
  (it pins one value and computes the other from the meter); the engine only takes fixed
  values. A pair above the single-exposure ceiling is refused (the UI stacks instead).
- `ev STEPS` sets the AE compensation, in the HAL's steps (`caps` line below), for the preview
  and for every automatic photo.
- `zoom FACTOR` sets the digital zoom (SCALER_CROP_REGION, centred) on the preview and every
  photo request, up to `caps zoom_max`. The HAL never crops the RAW16 output, so a DNG keeps the
  full frame (a DefaultCrop tag for the zoomed window is a later addition).
- Any size on the command line may be `max`: the engine picks the largest listed JPEG / RAW16
  size and, for the preview, the widest YUV size no wider than 1280 -- so a front camera with
  its own size list opens without the caller knowing it.
- `tap NX NY [ae] [af] [clear]` sets a metering and/or focus region at the point (NX, NY) of the
  viewfinder frame (0..1, the preview crop: zoom and loupe); with `ae` the RAW meter also measures
  only that spot until `clear`.
- `loupe FACTOR [CX CY]` crops the preview only (the focus loupe): real sensor detail, photos untouched.
  Before each viewfinder frame whose crop changed, the stream carries `fcrop x= y= w= h=` (the crop
  that frame was really taken with, normalised in the active array), so a display can map it exactly. Default is both; `clear` removes the regions (full-frame AE, the focus
  button's mode). The app maps a touch through its display rotation and loupe before calling, and
  sends `af` only in full manual exposure.
- `wb auto|incandescent|fluorescent|warm_fluorescent|daylight|cloudy|twilight|shade` sets the
  white balance of the preview and the JPEG (the RAW stays linear). caps lists `awb_modes`
  (Camera2 values) and `jpeg_sizes` (every listed JPEG size, for the resolution/format menu).
- `wb shift=S [tint=T]` sets a manual white balance as a shift from the HAL's AWB: AWB off, the
  AWB's last transform and gains with red/blue moved by 5 % per step (S -20..20, + = warmer) and
  the greens by the tint (T percent, + = magenta); `shift=0 tint=0` returns to AWB (colour
  correction back to the HAL's). Applied in the ISP: preview and JPEG agree, the RAW is untouched.
- `burst NAME [n=2..8]` takes JPEGs back to back in one capture call (fixed exposure: the given pair,
  or the live automatic one frozen); `flash off|auto|on|torch` drives the flash (`docs/ENGINE.md`).
- `preview NOISE EDGE` sets the viewfinder's processing only; photos take the modes given with them.
- `xp ...`: the controller's exposure commands (modes, EV, spot, pause), answered by `xpstate` events
  (`docs/ENGINE.md`, "Controller commands").
- The `ae` line also carries `cct=` and `duv=`: the white balance in force as a colour temperature and
  tint, estimated from the HAL's colour gains through the sensor's calibration matrix.
- `focus hold` keeps the lens exactly where it is (AF off at its reported position); `aelock on|off`
  holds / frees the automatic exposure. The app uses both once after a tap.
- `afwait MS` sets how long a photo waits for the 3A when the flash fires or an AF scan was asked
  (0 = 1.5 s); otherwise a photo waits at most 0.5 s for the exposure to settle.
- `calib D|inf|clear` records the lens's current position as the value distance D (metres) maps
  to, for the manual focus wheel (the app's "Focus cal." menu; `docs/DEVICE-PROFILES.md`).
- `scene off|N` sets a Camera2 scene mode (caps lists `scene_modes`): a grouped control under
  which the HAL drives AE/AWB/AF itself; the app greys the atomic controls while one is on.
- `stack NAME film=T [sub=T] [stk_denoise=1|2|4] [iso=N]` stacks `film/sub` sub-frames (times
  `stk_denoise`, divided out) into one DNG of exposure `film`; up to 4096 frames. The focus is
  frozen at the lens position the HAL last reported (no scan: focus before the shot). While it
  runs the SF2M channel carries `progress kind=stack frame=i n=N elapsed_ms=...` per frame and
  ends with `stack_done frames=F n=N aborted=0|1 ms=...`. **SIGUSR1** to the `indraw` process, or
  the end of its stdin (the app gone), aborts at the next sub-frame and writes what was added,
  with the exposure really reached. Formulas, noise and how to lower it: `docs/STACKING.md`.
- `meter on|off [rate=HZ]` runs the exposure meter: every period a RAW16 frame is captured and
  measured in place (never stored), giving the UI linear sensor data for its own AE loop and
  histogram. Paused while a photo uses the RAW stream. Needs `raw=WxH`.

The SF2M channel (text lines next to the SF2P frames) carries `captured kind=jpeg|raw|stack`
(the exposure is done: the UI flashes; for a JPEG or a RAW it goes at the first of the capture
result -- the frame is read, before any encoding -- or the image, once), `written kind=jpeg|dng|raw16|stack path=...` (the file
is on disk) and `written kind=preview path=NAME_vf.ppm orientation=N` (a half-size RGB render of
the DNG's own pixels -- black-subtracted, white-balanced with the as-shot neutral, gamma 1/2.2 --
written in the hidden `.previews/` subfolder, which the app converts to `NAME_vf.jpg` rotated
upright; indRAW's gallery pairs it with its DNG, the system's indexer skips the folder, so it
never shows up as a photo of its own), and besides:
`caps` once at stream start (ISO range and analog ceiling, exposure range, the single-exposure
ceiling after `override-hal.json`, EV range/step, orientation, black/white levels), `ae` about
10 times a second (the preview's real ISO/exposure, AE/AF states, EV, lens position, and
`focus_real`: that position as a real distance through the focus calibration) and
`vfmeter` about 10 times a second (the viewfinder frame's linear mean, centre-weighted or the tap's spot,
clipping and a 64-bin RGB histogram), `meter` at the meter rate (`mean` = centre-weighted green -- or only the spot of the last
`tap ... ae` until `clear` -- black-subtracted, 0..1 of the white level; clipping percentages;
`hist` = 64 bins x RGB on a 1/2.2 power scale, peak 1000).

## The app and Sailjail

The app runs sandboxed from the app grid (`[X-Sailjail]` in `app/indraw.desktop`:
`ApplicationName=indraw`, permissions Camera, Pictures, MediaIndexing -- the least it needs: no
other user directories, no network, no location). The sandbox only shows
`/usr/share/indraw` (so the spec installs the QML there, not under the binary's name),
`~/.local/share/com.indraw/indraw` (the engine's device profiles, see below) and
`~/.cache/com.indraw/indraw`, where the app writes the engine log `indraw-ui.log`. Photos go to
`~/Pictures/indRAW/` through the Pictures permission.

The caps line also carries `peak_edge`, `peak_width`, `zebra_jpeg` and `zebra_raw` (the focus
peaking's sensitivity and line width, the zebra clipping level for jpg, 0..255, and raw, per-mille
of the white level, since the RAW has its own latitude), all tunable per device in
`override-hal.json`, `flash` (1 when the camera has a flash unit), `crop_free` (1 when the HAL
crops anywhere, 0 when it only crops around the centre -- the loupe adapts) and `aperture` (the
lens's f-number).

## Where captures go, and their names

By default everything goes to `~/Pictures/indRAW/` as `photo_YYYYMMDD_HHMMSS_mmm.EXT`. The system
Settings app has an indRAW page (Settings > Apps > indRAW, like RAWfish) with the file name prefix
and the JPEG and DNG folders; they are dconf keys under `/apps/indraw/output` (`prefix`, `jpgDir`,
`dngDir`), read by the app at every engine start. The prefix is letters, digits, `-` and `_` (up to
32); folders are absolute (`~/` = home) and must be under `~/Pictures` for the sandboxed app to be
allowed to write there (the page refuses others). The engine gets them as `--jpgdir` / `--dngdir`
options (`indraw [--log FILE] [--jpgdir DIR] [--dngdir DIR] stream ...`); the DNG previews live in
`<dngDir>/.previews/` with a `.nomedia` file, so the system's indexer leaves them alone. The
gallery and the cover follow the configured folders.

The same page has a "Device tuning" section: a switch that shows the focus calibration column
in the camera's menu, and a button asking the app to write the device profile files at its next
start (`/apps/indraw/device`).

Porting to another phone: `docs/PORTING.md`.

## Simple mode layout

`~/.config/com.indraw/indraw/simple.json` defines what the app's simple mode hides and fixes
(defaults when absent):

```json
{ "hide": ["wheels", "type", "histogram", "headers"],
  "quality": 95, "noise": "zsl_noise", "edge": "hq_edge",
  "menu": ["timer", "format", "wb"] }
```

## Device profiles (multi-device)

indRAW keeps per-device data out of the code, split by nature (see
`docs/DEVICE-PROFILES.md`): what the HAL claims (`generated-hal.json`, read-only),
corrections to it (`override-hal.json`, atomic sensor fixes only -- today
`observed_shutter_max_ns`, the tested long-exposure ceiling, replacing the hard-coded 1/2 s), and UI
preferences (device-independent, kept by the app in its own settings). Files live under
`~/.local/share/com.indraw/indraw/device-profiles/` (Sailjail path;
`INDRAW_PROFILE_DIR` overrides for tests).

```sh
./indraw cameras          # list camera ids
./indraw probe            # write generated-hal.json (a snapshot of every camera's caps)
```

Everything asked of the HAL is **atomic** by default: each control is one
explicit request (exposure, ISO, focus, white balance, zoom, regions...),
nothing combined, and every capture reports exactly what was applied. Two
things are deliberately composed on top of that, in the open:
- the priority modes (one wheel pinned) are the app's own AE loop on the
  RAW16 meter sending fixed pairs -- the engine never sees a "priority";
- **scene modes** are the one grouped control offered (Scene column of the
  menu): while one is on the HAL drives AE/AWB/AF itself and the app greys
  the atomic controls it took over. Effects are listed by `caps` but not
  offered.


