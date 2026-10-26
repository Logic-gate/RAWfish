# (AI generated)
# The indRAW engine: command reference

`indraw` (the command-line tool, `/usr/bin/indraw`) loads `libindraw.so`
through libhybris and drives the camera with Android's Camera2. The app uses
exactly this interface; everything here can be done by hand in a terminal on
the phone. The app must be closed while you use the tool (one user of the
camera at a time).

Every result is **one line of JSON** on stdout (`"ok":true|false`);
diagnostics go to stderr. With `--log FILE`, the command line, every command
read (`> ...`) and every result are appended to FILE with a timestamp.

## Invocation

    indraw [--log FILE] [--jpgdir DIR] [--dngdir DIR] COMMAND ...

| Option | Meaning | Default |
|---|---|---|
| `--log FILE` | append a timestamped transcript to FILE | none |
| `--jpgdir DIR` | where JPEGs go (`~/` = home); created if needed, refused at start if impossible | `~/Pictures/indRAW` |
| `--dngdir DIR` | where DNGs (and their `.previews/`) go | `~/Pictures/indRAW` |

The options may come in any order, before the command.

| Command | What it does |
|---|---|
| `cameras` | lists the camera ids |
| `caps ID` | what camera ID offers, as JSON (sizes, ranges, modes; see `GENERATED-HAL.md`) |
| `probe` | writes `generated-hal.json` for every camera, and a starter `override-hal.json` if there is none (`DEVICE-PROFILES.md`) |
| `run ID JPEG_WxH [PREVIEW_WxH] [raw=WxH]` | opens camera ID and reads commands on stdin, one per line |
| `stream ID JPEG_WxH [PREVIEW_WxH] [raw=WxH]` | the same, and streams the viewfinder on stdout (the app's mode; SF2P frames + SF2M events, binary) |

Sizes are `WIDTHxHEIGHT` from the camera's lists (`caps`), or `max`: the
largest JPEG / RAW16 size, the widest preview not wider than 1280. The preview
defaults to 1280x960. Without `raw=` there is no RAW stream (no `raw`, `stack`,
`meter`). An unlisted size is refused before the camera opens.

## Commands on stdin (`run` / `stream`)

Names (`NAME`) are letters, digits, `-` and `_` only; the engine writes
`NAME_YYYYMMDD_HHMMSS_mmm.jpg` / `.dng` in the folders above (milliseconds, so
a burst never collides). Exposure times (`exp=`, `film=`, `sub=`) are decimal
seconds (`0.008`, `0.5`, `4`).

### Photos

| Command | Options | Limits |
|---|---|---|
| `jpeg NAME` | `q=N` JPEG quality | 1..100, default 95 |
| | noise mode: `default_noise` `off_noise` `fast_noise` `hq_noise` `minimal_noise` `zsl_noise` | those the camera lists |
| | edge mode: `default_edge` `off_edge` `fast_edge` `hq_edge` `zsl_edge` | those the camera lists |
| | `focus=` (below) | default: keep the lens where it is |
| | `exp=SEC iso=N` manual exposure (AE off) | the sensor's ranges (`caps`); `exp` also capped by the single-exposure ceiling |
| | `easy` | the one combined path: AF + AE before the shot |
| `raw NAME` | `focus=`, `exp=SEC`, `iso=N` | as above; writes a DNG (+ its preview in `.previews/`) |
| `raw -` | same | measure only, nothing written |
| `stack NAME film=T` | `sub=T` sub-frame length | default and maximum: the single-exposure ceiling (1/2 s, or `observed_shutter_max_ns`) |
| | `stk_denoise=1\|2\|4` extra frames divided out | 1 |
| | `iso=N` | the sensor's range |
| | | frames = film/sub x denoise, at most 4096; see `STACKING.md` |
| `burst NAME` | `n=N` frames | 2..8, default 3 |
| | `q=Q`, noise and edge modes | as for `jpeg` |
| | `exp=SEC iso=N` | default: the live automatic pair, frozen for the burst |
| | | JPEG only; one capture call, frames back to back at the sensor's pace; no flash; files `NAME_..._mmm.jpg` one millisecond apart |
| `noiseprofile FILE.json [sub=T]` | sweeps the ISOs and writes a darktable noise profile | developer tool |

Diagnostic variants for `jpeg` / `raw`: `preview=pause` (stop the preview
during the shot), `fps=low` (lowest AE frame-rate range), `control=off`.

A running `stack` stops at its next sub-frame on **SIGUSR1** or when stdin
closes, and writes the frames added so far with the exposure really reached.

### Focus

`focus=MODE` on a photo, or the live command `focus MODE`:

| MODE | Meaning |
|---|---|
| `auto` | one scan, then hold |
| `continuous` / `continuous_video` | the HAL focuses continuously |
| `macro` | like auto, restricted to near distances |
| `edof` | extended depth of field (if listed) |
| `inf` | manual, infinity |
| `hold` (live only) | AF off at the lens position the HAL reports now: the focus stays exactly where it is |
| `rawD` | AF off, the lens set to the HAL's OWN value D (LENS_FOCUS_DISTANCE), with no focus calibration: the same lens position on every phone of a model, used by the lens-shading assistant to walk positions |
| `<metres>m nowait` / `inf nowait` (live only) | the same, returning at once instead of waiting for the lens (the controller's volume-key stepping) |
| `<metres>m` (e.g. `0.5m`) | manual distance; mapped through the focus calibration when there is one |

| Live command | Limits |
|---|---|
| `loupe FACTOR [CX CY]` | focus loupe on the **preview only**: crops it by FACTOR around (CX, CY), the centre in the sensor's active array (0..1, default the middle); limited so zoom x loupe stays within the camera's maximum; a camera whose `caps` says `crop_free=0` (centre-only cropping) always gets the centre; `loupe 1` closes it |
| `tap NX NY [ae] [af] [clear]` | point in the **viewfinder frame** (the preview crop: zoom and loupe), 0..1 each (clamped), mapped to the sensor by the engine; the metering region is 6 % of the frame's short side (so smaller in the loupe), the focus region at least 5 % of the sensor's short side (a contrast AF needs texture); default both ae and af; with `ae` the RAW meter also measures only that spot (a box of 12 % of the frame, so smaller in the loupe, sampled densely); `clear` = back to the HAL's regions and the centre-weighted meter |
| `afwait MS` | how long a photo waits for the scan / precapture it asked for when the flash fires or an AF scan was asked; 0 = 1500 ms (the default; shorter and the flash's precapture has no time to meter). Without flash nor scan, no precapture is sent (0.47.29): the photo only waits, at most 500 ms (or MS if smaller), for the continuous auto exposure to settle, cancelling a precapture left over from earlier |
| `calib D` / `calib inf` / `calib clear` | records the lens position now as distance D metres (> 0), up to 16 points; `clear` forgets them (`DEVICE-PROFILES.md`) |

### Live viewfinder settings

| Command | Limits |
|---|---|
| `exposure auto` / `exposure exp=SEC iso=N` | the manual pair: sensor ranges, `exp` at most the single-exposure ceiling |
| `orient DEG` | the phone's physical rotation (0, 90, 180, 270 clockwise from its natural portrait), read at each shot: the photo's orientation is the sensor's mounting plus it (minus it on a front camera), so photos come out upright on the ground whatever the screen does |
| `wb pick` | measure the white on the spot (a light reference) or the middle of the displayed window, on one RAW frame, and keep it as the base of the manual white balance (AWB off; the shift/tint reset and applying on top); outcome as an event: `wbpick ok=1 gains=R,Ge,Go,B` or `wbpick ok=0 why=clipped\|dark\|timeout\|no_raw\|apply` |
| `shading calibrate full cct=K lens=D1,D2,...` | measure a whole calibration: a flat field through a diffuser, RAW at each HAL lens value D (K = the light's colour temperature); progress as `cal_progress done= total=`; the file goes to device-profiles/lens-shading/ |
| `shading calibrate light cct=K` | add a light (colour at temperature K, one position) to the existing calibration |
| `shading balance=B strength=S maxgain=M` | the measured map's dosing (pivot, strength, maximum gain) |
| `wb fixed R,GE,GO,B` / `wb fixed off` | keep a white measured before (restored by the interface after a restart) / back to the AWB as the base |
| `viewwin X Y W H` | the window the interface shows, normalised in the active array (the controller sends it with every view change): the histograms and the viewfinder's centre-weighted mean cover it, whatever the HAL's own frame |
| `aelock on` / `aelock off` | holds / frees the automatic exposure (a new `tap` with ae, `tap ... clear` and `exposure auto` free it too) |
| `ev STEPS` | the HAL's compensation steps (`caps` ev_min..ev_max, step ev_step) |
| `preview NOISE EDGE` | the noise / edge mode names above, for the **viewfinder only**: a photo uses the modes given with its own command, or the camera's still-capture defaults |
| `wb MODE` | `auto` `incandescent` `fluorescent` `warm_fluorescent` `daylight` `cloudy` `twilight` `shade`, if listed |
| `wb shift=S [tint=T]` | S -20..20 (5 % steps red/blue, + warmer), T -100..100 (+ magenta); `shift=0 tint=0` = back to AWB |
| `flash off\|auto\|on\|torch` | only if the camera has a flash (`caps` flash=1); auto/on in automatic exposure add a pre-flash precapture (~0.5-1 s); with a manual exposure (or a pair the app holds) "on" fires a single flash and "auto" does not fire (an automatic flash needs the camera's metering) |
| `scene off` / `scene N` | a Camera2 scene number the camera lists (`caps` scene_modes) |
| `zoom FACTOR` | 1..the camera's maximum (clamped); the RAW stays full frame |
| `meter on [rate=HZ]` / `meter off` | 0 < HZ <= 20 (default 5); needs `raw=` |
| `preview-frame FILE.ppm` | saves one viewfinder frame (diagnostic) |
| `# TEXT` | a comment: logged, ignored |
| `quit` | closes the camera and exits (end of stdin does too) |

## Events on the stream (`stream` mode)

The RAW meter (`meter on`) measures only while a stream is attached
(`indraw_stream_start`): it serves an interface that is looking; with no
stream the command is accepted but no frame is measured.

`caps` carries `proto=N`, the version of this event/command protocol (1
since 0.47.15); an interface built against another version can tell.

Besides the frames (SF2P), the stream carries one-line events (SF2M): `caps`,
`ae` (the preview's live values; also `cct=` and `duv=`, the white balance in force as a correlated colour temperature and its Duv, estimated from the colour gains through the sensor's calibration matrix), `meter` (the RAW meter; its histogram covers the displayed window (`viewwin`), and `shist=` is the spot's own while a reference is set -- `vfmeter` likewise), `vfmeter` (the viewfinder meter: the linear
mean -- centre-weighted, or the tap's spot --, clipping percentages, the
pair the frame was exposed with when known (`iso=`, `exp_ns=`, else 0) and
a 64-bin RGB histogram of the frame just sent, at most every 100 ms),
`captured`, `written`, `stack_start n=` (a stack began), `progress`, `stack_done`, and `fcrop
x= y= w= h=` -- sent right before a frame whose crop differs from the
previous one: the crop that frame was really taken with (from its capture
result), normalised in the active array. A HAL applies a new crop a few
frames after it is asked to; a display that maps frames with `fcrop` never
jumps.

Internals: with a controller attached (`indraw_set_event_fd`, done by the
`indraw` process at stream start), these events go to the controller through
a pipe and reach the stream through `indraw_emit`; the behaviour seen by an
interface is the same.

## Controller commands (`stream` mode)

In `stream` mode the `indraw` process also runs the controller
(docs/ARCHITECTURE.md): the camera's behaviour, which the interface drives
with high-level commands on the same stdin. The controller turns them into
the engine commands above (logged with a `c> ` prefix) and reports its state
as events.

| Command | Meaning |
|---|---|
| `xp auto` | automatic exposure (the AE button) |
| `xp touch iso` / `xp touch speed` | that wheel is pinned where it is (a priority mode; both: manual) |
| `xp iso N` / `xp speed NS` | that wheel pinned to a value (NS in nanoseconds; beyond the ceiling: a stack, previewed as one frame of the same exposure, as short as `xp pvfps` allows, the ISO raised to match) |
| `xp ev STEPS` | the EV wheel (HAL steps): the HAL's compensation in automatic, the loop's target otherwise |
| `xp spot on` / `xp spot off` | a light reference in automatic: the spot measure (bounded, then held) / back to the HAL's AE |
| `xp pause on` / `xp pause off` | the exposure left alone (the loupe moving without a reference, a focus search) |
| `xp source raw` / `xp source vf` | which meter drives the loop (the RAW meter in raw, the viewfinder's otherwise) |
| `xp pvfps N` | the target frame rate of a Σ preview (1..30, 10 by default): one frame of the stack's exposure, as short as that, ISO permitting |
| `xp state` | the state once |

| `fc mode cont\|once\|manual` | the focus mode (applied to the preview; drops the reference) |
| `fc step N` | manual focus: N steps of 0.05 D (+ farther, - nearer) |
| `fc distance D` | manual focus set to D diopters (0 = infinity) |
| `fc ref FX FY` | a tap at a point of the viewfinder frame: the reference (search with the HAL's AE on, then hold / adopt the distance, then the spot measure); light only in continuous mode |
| `fc clear` | the reference removed (regions reset, exposure freed, mode restated) |
| `fc scene on\|off` | a scene drives the exposure: no spot measure after a tap |
| `fc state` | the state once |

| `vw zoom Z` / `vw loupe L` | the digital zoom (photo crop) / the focus loupe factor (1 closes it) |
| `vw pan DU DV` | moves the loupe by a fraction of the display window (the interface undoes its view rotation first) |
| `vw gesture on\|off` | a finger moves the loupe: the HAL crop stays at the zoom window meanwhile |
| `vw tap U V` | a tap at a display-window point: the controller maps it to the viewfinder frame and runs the focus sequence (as `fc ref`) |
| `vw clear` / `vw state` | the reference point forgotten / the state once |

| `sh name=N type=jpg\|raw q=Q noise=M edge=M bracket=off\|0\|1\|2\|3 denoise_sigma=D` | a press of the shutter: the controller turns it into the engine's `jpeg` / `raw` / `stack` / `burst` lines (the held pair, the manual distance, a stack for a Σ speed, a burst or three shots for a bracket), run as if typed; the self-timer is the interface's |

Event: `vwstate zoom= loupe= view=x,y,w,h ref=has,u,v` -- the view window to
show (array-normalised) and the reference point in display-window
coordinates.

Event: `fcstate mode= d= circle= searching= light=` -- the focus mode, the
manual distance (diopters), the circle to draw (0 none, 1 searching, 4 sharp,
5 locked but unsure, 6 failed, 7 sun), whether a search runs, whether a
reference measures the light.

Event: `xpstate iso= exp_ns= iso_manual= shutter_manual= full_auto= full_manual= fixed= spot= ev= offset= offset_valid= pv_ms= pv_slow= deficit= too_dark= ev100=` --
(the `pv_*`, `deficit` and `too_dark` fields describe a Σ preview: its frame
time, slowed by the ISO limit, how many stops darker than the photo, and
whether its signal is too low to measure)
what the wheels and indicators show (`fixed`: a pair the controller holds,
which photos carry; `offset`: manual stops from automatic; `ev100`: the
metered light level, `nan` without an aperture). Sent whenever it changes.

## Device profile fields (`override-hal.json`)

Checked at every open; out-of-range values are ignored with a log line.

| Field | Range | Default |
|---|---|---|
| `observed_shutter_max_ns` | 1 ms .. 60 s | 500000000 (1/2 s) |
| `min_frame_ns` | 1 ms .. 1 s | 33333333 |
| `peak_edge` | 1 .. 255 | 40 |
| `peak_width` | 1 .. 6 | 2 |
| `zebra_jpeg` | 1 .. 255 | 250 |
| `zebra_raw` | 1 .. 1000 (per mille of white) | 992 |
| `focus_calibration` | up to 16 `{ "real_m", "hal_diopters" }` | none (1:1) |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | done |
| 2 | bad command line |
| 10 | libhybris / android_dlopen unavailable |
| 12 | `libindraw.so` not loaded, a symbol missing, or library/tool version mismatch |
| 14 | binder thread pool could not start |
| 20 | the camera layer reported an error |

## Examples

List the cameras, then what the back camera offers:

    indraw cameras
    indraw caps 0

One JPEG and one DNG, automatic exposure, transcript in a log:

    printf 'jpeg test\nraw test\nquit\n' | indraw --log /tmp/t.log run 0 max max raw=max

A manual 1/60 s at ISO 400, focused at 1.5 m, quality 90, no noise reduction:

    printf 'jpeg street q=90 off_noise focus=1.5m exp=0.0167 iso=400\nquit\n' | indraw run 0 max

A Σ8s stack at ISO 400 with the denoise x2 (32 sub-frames, about 22 s), into
its own folder:

    printf 'stack night film=8 iso=400 stk_denoise=2\nquit\n' | indraw --dngdir ~/Pictures/night run 0 max max raw=max

Three JPEGs as fast as the sensor allows (a short moment), at a fixed 1/250 s:

    printf 'burst moment n=3 exp=0.004 iso=400\nquit\n' | indraw run 0 4096x3072

Measure a scene's exposure without writing anything:

    printf 'raw - exp=0.01 iso=100\nquit\n' | indraw run 0 max max raw=max

Dark frames for a noise measurement (lens covered; see `STACKING.md`):

    for iso in 100 400 1600; do for n in 1 2; do echo "raw dark-iso_${iso}-n_${n} focus=inf exp=0.01 iso=${iso}"; done; done > /tmp/d.txt
    echo quit >> /tmp/d.txt
    indraw --dngdir ~/Pictures/darks run 0 max max raw=max < /tmp/d.txt

Write the device profile files:

    indraw probe

## The preview's YUV -> RGB (0.47.61)

yuv420_to_rgb (android/yuv.h) converts the Camera2 YUV_420_888 planes with
the bounds checked once per row (a row whose last sample would fall outside
a plane is black) and the three chroma terms computed once per pair of
pixels. It is pixel-exact with the per-pixel loop it replaced
(android/tests/yuv_test.c: 40 random layouts, planar and semi-planar,
padded rows, odd widths, truncated planes). On an x86 host at -O2 it is
1.3x faster than the per-pixel loop (the compiler already hoisted much of
the per-pixel cost); the gain on the phones' ARM cores is to be measured,
not assumed.

## The DNG thumbnail's colour (0.47.69)

The thumbnail written next to each DNG is rendered the way a raw converter
reads the file: the light's temperature from the as-shot neutral through
ColorMatrix1/2 (ct_estimate, the same as the viewfinder's kelvin), the
ForwardMatrix1/2 interpolated at it (white-balanced camera RGB -> XYZ D50),
then XYZ D50 -> sRGB (Bradford). Until 0.47.68 it used ColorMatrix1 (D65)
alone, right in daylight only: under a 3500 K LED a cream wall came out
B/G 0.23 (yellow-green), now 0.65 -- as darktable shows it. Without the
forward matrices the one-matrix path remains as a fallback.
