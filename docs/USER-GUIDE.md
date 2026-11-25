# (AI written, human written user-guite should be made)
# RAWfish-ir user guide

How to use the camera, screen by screen. For installing it, see
`BUILDING.md`; for the reasons behind each behaviour, `UI-NOTES.md`.

## The camera screen (portrait)

From top to bottom: a black band around the front camera with four text
buttons, the viewfinder, and the control deck.

### The four buttons around the front camera

| Button | What it does |
|---|---|
| **zebra** | stripes over what is about to clip (bold = on). In **jpg** and snap: the pixels that come out white (their luminance at the limit); in **RAW**: any one colour channel at the limit, since a lost channel is lost data. Zebra and peak are remembered from one start to the next in advanced mode |
| **peak** | thin red lines on the edges that are in focus |
| **level** | a cross on the true horizon, with the degrees (see "The level") |
| **AE** | back to full automatic exposure on the whole image; also removes the exposure point (the sun) |

### The viewfinder

It shows the picture as it will be: no brightening for display, what you see
is the exposure you get.

| Gesture | Effect |
|---|---|
| touch | **the focus** at that point (see "Focus"); the exposure is not touched. In **auto** focus the touch takes the focus from the camera |
| touch held 1 s | **the exposure** at that point: a small sun marks a spot of 5 % of the frame, measured from then on (see "Exposure") |
| touch on the circle / on the sun | removes it: the focus back to the mode's own, or the exposure back to the whole image |
| two fingers apart / together | focus loupe, up to 12x, for the viewfinder only (the photo is not zoomed): up to the camera's limit (about 4x on the JP2) the sensor's real detail, beyond that an enlargement; a red frame and diagonal say it is on |
| one finger, loupe on | moves around in the magnified image; while a finger moves, the exposure does not change and the view is an enlargement (smooth); when it stops, the sensor's real detail comes back and the exposure, the wheels and AE work as usual. On cameras that can only magnify the centre, the real detail is largest in the middle and the view is enlarged more the further you move from it |
| touch or hold, loupe on | the same, on a spot as small as the loupe shows: a precise focus point, a precise spot measurement. The circle and the sun follow the loupe and the zoom |
| double tap, loupe on | closes the loupe (back to the zoom in force). A single touch in the loupe acts 0.3 s later, so that a double tap does not move the focus point |
| swipe down | opens the settings menu |
| swipe left to right | opens the gallery (a clearly sideways swipe: a slanting downward one opens the menu instead) |

While the loupe is on, the menu and gallery swipes are off. After each
touch or hold, a line recalls the rule for two seconds: "touch: focus · hold:
exposure here".

**Photo not saved.** A red box in the middle, "Photo NOT saved", says
that a photo could not be taken or saved: with "storage full" the memory
(phone or card) has no room left; otherwise the engine's reason is under
it. A touch closes it. At the top, "Storage almost full: N MB left" shows
while less than 300 MB are left where the photos go (a DNG takes about
25 MB, a JPG 3 MB), in red under 30 MB.

### The deck

| Part | What it is |
|---|---|
| **FOCAL** | the camera, shown by its 35 mm-equivalent focal length; tap to switch camera |
| **TYPE** | **DNG (RAW)**; **jpg**, RAWfish-ir's JPEG, made from the camera's image before its noise reduction and sharpening ("JPG +RAW" when Settings saves a DNG with each JPG); **snap**, a light photo for utility shots such as a receipt or a sign: our own processing at its defaults, the image averaged down to about 2048 px wide (2048 × 1536, 3 MP, on the JP2), marked SNAP on the viewfinder. TYPE never restarts the camera between these three |

| **FOCUS** | auto, or the distance held; a touch on a distance gives the focus back to auto (see "Focus") |
| histogram | the tones of **what the viewfinder shows** (zoomed or in the loupe: that area), per colour, with clipping bars at each end. In **jpg** it is measured on the processed image; in **raw** on the sensor data itself, so it sits further left and narrower (no tone curve lifting the shadows), and its left bar is bigger: it counts photosites at or below black, where noise puts about half of the truly dark ones -- shadows with no information, as the DNG will hold them. With a reference circle, the circle's own tones are drawn as bright outlines over it |
| **EV** wheel | brightness correction, in the camera's steps |
| shutter | takes the picture |
| **+ / −** (three of each, coloured) | left: warmth, by steps of 5 % red/blue (orange + warmer, blue − cooler); middle: digital zoom, white (the JPEG is cropped; a DNG stays full frame): a tap steps by 0.25, a quick double tap (within 0.15 s) goes to the max / back to x1, and **holding** zooms progressively -- slowly at first, faster the longer you hold, at the same pace whatever the magnification (x1 to x2 takes as long as x2 to x4); right: tint, by steps of 5 % green (magenta +, green −). A tap shows for a moment what it added |
| readouts beside the shutter | the white balance in force, estimated from the camera's own colour gains and its colour calibration: the colour temperature in kelvin, and the tint as Duv in thousandths (+ green side, − magenta) -- more reliable as a difference than as an absolute value (the camera's calibration reads a 2700 K bulb as ~3300 K); note that the camera's automatic balance only corrects part of the way: daylight photos come out a little cold, lamp-lit ones a little warm; they follow the automatic white balance and your steps alike; a readout is coloured while its shift is not zero, and a double tap on it resets that shift. **Measuring the white**: aim at something white or grey (in the circle if you set one, else in the middle) and hold a finger 2 s on either readout -- a ring fills around the shutter and a light-blue circle shows the zone being measured; releasing early cancels. The white is measured on the sensor's raw data (on a camera without RAW, the JP2's front and wide ones: on the viewfinder's image, a little less exact; a second measure on the same white comes closer) and kept -- for that camera: each camera keeps its own measured white (a sensor's colours are its own); a camera without one gets the last measured white as an offset from its own automatic white, so a white measured on purpose on a coloured card carries its colour to every camera, approximately. A double tap (back to automatic) drops the measured whites of all the cameras (both readouts turn light blue): JPEGs and the viewfinder are then neutral for that light, and the DNG records that white as its "as shot" balance (EXIF: manual white balance); the ± buttons still apply on top. When the app restarts, the measured white is applied for 6 s with a button **Keep previous manual WB** and a bar running out: a tap keeps it, otherwise the automatic balance comes back. A double tap on a readout returns to automatic. It refuses a white that is clipped (too bright) or too dark |
| **SPEED** and **ISO** wheels | exposure time and sensitivity |
| `ui NN` | the app's build number |

## Exposure

The SPEED and ISO wheels decide the mode:

| Mode | How to get there | What is automatic | With an exposure point (the sun) |
|---|---|---|---|
| **automatic** | no wheel touched, or the AE button | speed and ISO, by the camera; the EV wheel corrects | the app measures the point and sets the exposure from it (a few quick corrections), then **holds** it until you remove the sun |
| **ISO priority** | touch the ISO wheel | the speed, by the app, at most 1/15 s (longer: set it on the wheel); EV corrects | the app measures the point instead of the whole image; the point's light level shows above the sun |
| **speed priority** | touch the SPEED wheel | the ISO, by the app; EV corrects; past 1/2 s: Σ (stacked, raw) | same |
| **manual** | touch both | nothing; the EV wheel becomes a "+/−" number: stops brighter or darker than automatic would be | the number tells how **the point** is exposed; the same number shows above the sun |

The exposure point is set by a touch **held one second**: a small sun, a spot
of 5 % of the frame. Without one, the exposure is measured on the whole image.
In a manual or priority exposure the number above the sun says how many stops
the point sits above (+) or below (−) a neutral grey correctly exposed, with
the settings in force ("+1.3"): one more ISO stop adds one. "≥ +2.5": the point
is clipped, at least that much; "≤ −4.0": crushed black, at most that much. The histogram draws the spot's own tones as
brighter outlines over the whole picture's. A touch on the sun, or the AE
button, removes it.

The AE button goes back to automatic from any mode, and removes the sun.
A photo is always exposed as the viewfinder shows: when a pair is held (a
wheel, or a sun in automatic) the photo takes that pair. Switching to
simple mode returns to automatic exposure and automatic focus; another
camera, another resolution or coming back to the app keeps the exposure
mode and its values, and the focus point where it still makes sense.
Under a scene, the camera drives the exposure.
"auto" under a wheel means the camera (or the app) is turning it.

## Focus

One mode: the camera starts in **auto**, and you take the focus over
whenever you like.

| FOCUS shows | Without a touch | A touch | The volume keys |
|---|---|---|---|
| **auto** | the camera keeps focusing by itself, following the scene, where it chooses | takes the focus: one search on the point, from where the lens is; the distance found is shown and held | take the focus from where the lens is (no jump), then move it by small steps (up = farther; hold a key to move faster) |
| **a distance** (e.g. "1.50 m") | the lens stays there, however you reframe | one search on the point; the distance found replaces it | small steps from there |

A touch on FOCUS showing a distance hands the focus back to the camera
(auto); showing auto, it takes the focus with one search in the middle of
the frame. The distance stays after a photo, until you hand it back. On a
camera whose profile says a touch cannot focus it (the JP2's 18 mm), only
the volume keys take it.

A touch while a search is still running starts a new one on the new
point (the first is cancelled properly before).

A photo never starts a new search: it is taken where the focus is. A touch
never changes the exposure: that is the held touch's work (see "Exposure").
From 1/4 s (and for a Σ) the camera's own focusing is stopped for the photo,
the lens held where it is at the release, then given back: it cannot move
during the exposure. After the photo the auto focus may search again: that
is after, not during. The log says where the lens was before and after each
photo (the `lens` lines).

**The circle** marks the focus point: it surrounds the square zone the
camera focuses on (15 % of the viewfinder's short side), and shows inside
it the point magnified twice (with the peaking, if on), to judge the
focus. In the loupe the zone covers as much less of the subject as the
loupe magnifies: use it to aim the focus, or the light (5 %), at a precise
detail. Red: the search did not lock, the lens went back to the distance
before. Touch a part of the subject that fills the zone: what lies behind
it inside the zone counts too. It stays on screen -- it never
disappears by itself: dotted while searching, solid when sharp, dashed when
it could not focus. Remove it with a touch on it or by handing the focus
back to auto.
In the loupe, the point is as small as the loupe shows. The circle (focus)
and the sun (exposure) are independent and can stand together.

Simple mode has no FOCUS button: it stays in auto, and a touch does not
take the focus. (Until 0.3.8 auto ignored touches and the keys, and the
FOCUS button switched between auto and manual. Until 0.3.2 a third
mode, "once", searched at a touch and held: manual now does that, and lets
the distance be adjusted.)

Manual distances are only as exact as the focus calibration of your phone
(see "Settings" and `DEVICE-PROFILES.md`).

## Long exposures (Σ)

Past 1/2 s, the SPEED wheel shows **Σ** values: 1 s to 8 min, with the real
time the shot will take underneath ("(2:48)"). The phone cannot expose longer
than 1/2 s at once, so it takes many short exposures and adds them: the
result is one DNG as bright as a single exposure of that length. A Σ speed
switches the type to raw (a stack is a DNG); the viewfinder says
"experimental feature". During the shot the screen stays on and black, shows
the progress and the time left, and every control but **abort** is locked;
abort keeps what has been taken so far. Use a tripod. Details and noise in
`STACKING.md`.

### How the viewfinder measures a Σ exposure

**The only way to measure the light, with the means available.** The sensor
cannot expose longer than 1/2 s at once, and there is no other way to
measure the scene: the photo sensor is the only one that sees it (the
phone's ambient-light sensor faces you, not the subject). So a Σ exposure is
measured and shown through ONE short frame of the **same exposure**: what
the sensor collects is the exposure time times the gain (the ISO), so a
frame 20 times shorter at 20 times the ISO collects as much as the stack.

**The rhythm.** That frame is made as short as a target rate allows
(10 frames per second by default, `sigmaPreviewFps` in `rawfish-ir.conf`), the
ISO raised to match. Measuring, the exposure loop and the focus search all
work one frame at a time, so a fast frame keeps them responsive. If the ISO
this needs is beyond the sensor's maximum, the frame is made longer, just
enough, up to 1/2 s: in dim light the viewfinder slows down.

The same holds for a speed of **1/4 s or 1/2 s** set by hand: the viewfinder
shows one 1/10 s frame of the same exposure (ISO 250 for 1/4 s at ISO 100),
not the real 1/4 s, so it stays fluid; the photo is taken at the speed and
ISO on the wheels. Faster than 1/4 s the viewfinder shows the real
exposure as before. A speed left to the app (ISO priority) goes no longer
than 1/15 s: in the dark -- a pocket, a hand over the lens -- it no longer
sinks to 1/2 s, a viewfinder of 2 frames a second slow to come back.

**What the histogram shows.** The right position and the right clipping for
the photo, but **more noise**, so it spreads further into the shadows: one
frame at a high ISO is noisier than the sum of many frames at the photo's
ISO. The photo itself will be cleaner than the viewfinder suggests.

**The messages**, under "Σ stacking: experimental feature":

| Message | What it means |
|---|---|
| *ISO limit: preview slowed to N frames/s* | the maximum ISO is reached, the frame is longer than the target; the measure is still right, only slower |
| *preview N EV darker than the photo* (orange) | even at 1/2 s and the maximum ISO the frame cannot collect as much light as the stack: the meter compensates by calculation, but **the histogram and the viewfinder are too dark** by that much |
| *too dark to measure* (red) | only at the real limit: the frame is already at 1/2 s and the maximum ISO, and its signal is still down in the sensor's noise. Nothing reliable can be measured; the exposure is left as it is: set it by hand or add light. (Before the limit a dark viewfinder just means a dark photo: in manual the ± number says by how much, in speed priority the ISO rises by itself) |

**Technical limits of the hardware**, to know: the longest single exposure is
1/2 s (the camera's declared 16 s is not real, measured); the maximum ISO is
the sensor's; and the sensor's maximum **analog** gain is a declaration of
the HAL (the sensor's driver) that has not been verified -- beyond it the gain
may be digital, which stays proportional but quantises the shadows (a test is
planned, see `docs/ROADMAP.md`).

## The level

The **level** button shows a cross running edge to edge: its horizontal arm
turns as the phone leans sideways (the roll) and the whole cross moves up or
down as you tilt the phone (the pitch), at the image's own scale; a small
fixed cross at the centre is the reference, and the degrees ("H" roll, "V"
pitch) are written at the bottom of the image. The
horizontal arm and H turn **yellow** when the roll is level within one
degree, the vertical arm and V when the pitch is, and everything turns
**green** when both are. It needs the phone's accelerometer; if the button
stays dim, the level is unavailable on this device.

Phones' sensors are often a degree or two off. To **zero** the level: hold
the phone truly upright in portrait (its bottom edge on a level surface, its
back against a vertical one), and **hold** the "level" button: the reading
of that moment becomes the zero ("level zeroed here"), kept in the
settings. The "Level" column of the settings menu shows "zeroed" and lets
you go back to the sensor's own zero ("factory").

## The settings menu (swipe down in the viewfinder)

The image dims; the settings stand as columns of round buttons in a band wider
than the screen: drag it left and right (the bar at the bottom shows where you
are). Pressing a button shows its full name at the top. Tap outside the
columns or swipe up to close.

| Column | What it sets |
|---|---|
| Mode | the first column: **simple** (jpg or snap, automatic focus and exposure, a brightness slider instead of the wheels, the cameras named standard / wide / tele / selfie) or **advanced** |
| Flash | off, auto (when dark), on, torch (continuous light); shown only if the camera has a flash. The flash makes everything automatic: with "auto" or "on" the exposure and the focus go automatic ("Toggling to full auto." if something was manual), the ISO, speed and FOCUS controls are greyed (a touch on them, or a tap or hold in the viewfinder, says "Flash is activated: full auto."), and the camera measures with a pre-flash first, so the shot takes half a second to a second longer. What was manual comes back when the flash goes off; the EV compensation stays available. The torch changes nothing. No flash in a burst, a bracket (shown "off", greyed) or a Σ. Our JPG is one frame with the flash. |
| Timer | off, 3 s, 10 s |
| Grid | rule-of-thirds lines |
| Format | 4:3 or 16:9. By default, the shape of the RAW the camera gives ("as the RAW": 4:3 on the JP2, 16:9 on the Xperia XA2); the other one crops. The viewfinder takes the photo's shape (grey bands only if the camera has no preview of that shape) |
| Resolution | the JPEG sizes this camera offers; greyed in RAW (the sensor's size) and in snap (its own small size, shown) |
| Quality | JPEG quality: 20, 40, 60, 80, 90, 95 (default) or 100 |
| White bal. | automatic or a preset (daylight, cloudy, tungsten...) |
| Bracket | (not in snap, not with a Σ shutter: there it shows "off", greyed, and the chosen bracket comes back when the shutter leaves Σ; in jpg the photos are the camera's own JPEG, as a line in the viewfinder says when the bracket is chosen) off; **x3**, **x5**, **x7**: three, five or seven shots as fast as the sensor allows, same exposure and focus (in JPEG a real burst, a few hundredths of a second apart; in raw one after the other) -- to merge afterwards (noise, or a stack in another program); or three shots at −N, 0, +N stops |
| JPG processing: Denoise, Frames, (Per frame), Colour NR, Moiré, Luma NR, Sharpen | columns framed together, named under them: RAWfish-ir's processing of the **jpg**, shown when Settings > Camera menu > "JPG processing in the camera menu" is on (else set in `rawfish-ir.conf`). **Each camera keeps its own**: a choice made on one camera does not change the others, and each camera starts from its own defaults (the green rings; a choice of the default is "default" in `rawfish-ir.conf`, in the camera's section `[camera_N]`). The denoise method (classic: each frame shifted whole; tiles: each part aligned on its own, for hand-held shots), which shows its own columns: classic, frames merged (1, 2, 4, 8); tiles, frames merged (2, 4, 8) and the pose per frame (1, 1/2, 1/4: shorter frames, the ISO raised to match, less blur); then the colour noise filter, the false-colour filter, the luminance noise filter (Luma NR: after the merge, the grain left averaged away where it is no larger than 1, 1.5 or 2.2 times the noise, edges kept; off by default, 2 on the Xperia 10 III) and the sharpening; the default of each has a green ring. Greyed together outside our jpg (raw, snap, Σ, a bracket) |
| Level | the level's zero: "factory" (the sensor's) or "zeroed" (recorded by holding "level" with the phone upright) |
| Focus cal. | the focus calibration assistant (when enabled in Settings) |

The band always opens on its left end. The Σ denoise (2x or 4x the frames for a Σ shot) is set in `rawfish-ir.conf`, not in the menu.

In **simple** mode the menu keeps the mode, the flash (if the camera has
one), the grid and the level, plus the timer, format and white balance (these
three can be changed in `~/.config/com.rawfish/rawfish-ir/simple.json`, see
the README).

## The gallery (swipe left to right)

As in Jolla Camera, the gallery and the camera are one strip: swipe left to
right on the camera and the newest capture slides in; keep swiping for older
ones, right to left to come back; past the newest the same movement carries
on into the camera. The strip ends at the oldest
(no loop). "3 / 27" at the top says where you are (1 = the newest). Pinch to
zoom. Tap to show or hide the buttons: share, delete (with the usual few
seconds to undo; the next older capture then takes its place, so sorting
goes on), details, and the cross back to the camera. A **DNG** is shown through a quick
preview made from its own data, with a banner saying so: deleting it deletes
the DNG.

## Two words

**DN** (*digital number*): the integer value of one photosite as the
sensor's converter outputs it. On the JP2 it runs from 0 to 1023 (10 bits),
black being around 64; "1 DN" is the smallest step the sensor can express.
Noise is measured in it (about 0.5 DN at ISO 100-400, 1.7 DN at ISO 3200).

**RAW16**: the name of Camera2's raw format (`RAW_SENSOR`), which stores
each photosite in a 16-bit word, simply as a container. The JP2's sensor
fills only 10 of those bits; the DNG says so (`WhiteLevel` 1023). A Σ
stack, being a sum of frames, has finer levels and is written over the full
16 bits.

## Photo orientation

A photo is tagged upright for the ground, from the phone's own tilt sensor at
the moment of the shot -- whether or not the screen's orientation is locked,
and whatever the viewfinder shows. (Without a tilt sensor, the phone's
natural orientation is assumed.)

## Screenshot

Press both volume keys at once: the screen is saved to
`~/Pictures/Screenshots/`, with a flash.

## Settings (Settings app > Apps > RAWfish-ir)

| Setting | Meaning |
|---|---|
| Also save a DNG with each JPG | a DNG of the same exposure, with the same file name, in the DNG folder; not with the camera's own JPEG, a Σ (DNG only) or a phone that cannot take both at once |
| File name prefix | the start of every file name (`photo` by default): letters, digits, `-`, `_`, not starting with `-`. Empty: no prefix, the files are named by the date alone (`YYYYMMDD_HHMMSS.jpg`). The milliseconds (`_mmm`) are added when two photos fall in the same second, and always for the photos of a bracket or a burst (x3, x5, x7) |
| Storage | the phone's memory or a memory card: moves both folders below to `Pictures/RAWfish-ir` there. If the card is not mounted when the camera starts, the photos go to `~/Pictures/RAWfish-ir` (the log says so) and back to the card once it is there again |
| JPEG folder, DNG folder | where the pictures go; under `~/Pictures` or on a memory card (`/run/media/<user>/<card>/...`), the only places the app may write |
| Reset to defaults | the prefix and the two folders (back to the phone's memory) |
| Start in manual focus and exposure | at start, manual focus at the last distance and both wheels pinned at the last ISO and speed (advanced mode; off by default) |
| Mode at start | simple, advanced, or as left (the last mode used) |
| JPG processing in the camera menu | shows RAWfish-ir's processing of the JPG in the advanced menu: Denoise, Frames, Per frame, Colour NR, Moiré, Luma NR and Sharpen, each kept per camera (otherwise set in `rawfish-ir.conf`, a section `[camera_N]` per camera) |
| Focus calibration in the camera menu | shows the "Focus cal." column |
| Calibrate the lens shading | opens the lens-shading assistant at the next start (a white translucent diffuser on the lens, an even light, about a minute) |
| Create the device profile files | RAWfish-ir writes them the next time it opens (the date is shown once done) |
| Detailed log | the log keeps every command and reply (normally only the starts, the photos and the errors), the photos' reports carry the histograms, and our JPG photos carry their settings in their file names; for reproducing a problem |
| Reset the camera settings | every camera setting and `rawfish-ir.conf` back to their defaults when RAWfish-ir next opens; the file is kept as `rawfish-ir.conf.bak`; the settings of this page are not changed |

Changes take effect the next time the camera starts.

### Calibrating the focus

1. Turn on "Focus calibration in the camera menu".
2. Put something with sharp details at a measured distance, e.g. 0.5 m.
3. Tap the target: the camera searches there and holds the distance it
   found. Wait for the solid circle (red: it did not lock, tap again).
4. Menu, column **Focus cal.**, tap **0.5m**: it blinks red; tap it **again**
   to record (this two-tap rule protects a calibration from a stray touch;
   the same for **clear**, which forgets all the recorded points).
5. Repeat for a few distances (0.25 m, 1 m, 2 m, ∞ on something far). One
   point already corrects the whole scale by a constant shift; with two or
   more the engine draws straight lines between them and extends the end
   ones, so every distance is covered; three or four points across the range
   are plenty. For **0.08m** (the closest), the autofocus cannot get there:
   focus by hand with the volume keys until the target is sharp, then record.
6. Turn the switch off again.

## Files

| What | Where |
|---|---|
| photos | `~/Pictures/RAWfish-ir/` (or the folders set above) |
| DNG quick previews | `.previews/` inside the DNG folder (hidden, not in the system gallery) |
| a new version | at its first start, a newly installed version moves the earlier `rawfish-ir.conf`, `simple.json`, `override-hal.json`, `focus-calibration.json` and measured lens-shading maps to `~/.config/com.rawfish/rawfish-ir/backup-<earlier version>-<date>/`, and starts from its own defaults and device profile -- except your settings: the values of the earlier `rawfish-ir.conf` are carried into the new one (a key the new version no longer has is left out, a value it no longer allows takes its default; the log says how many were carried). Nothing is deleted: to take a file back, copy it from there to where it was. The photos and the Settings page's choices (folders, prefix) stay |
| commented references | `rawfish-ir.conf.example` next to `rawfish-ir.conf`, and `override-hal.example.jsonc` next to `override-hal.json` in `device-profiles/`: every key, what it does, its values and limits, as real comments; in `rawfish-ir.conf.example` each key is written at its default. They are references for you to read: the app never reads them, and writes them again at each start (deleting one changes nothing). `rawfish-ir.conf` itself is written by the app with every key (at its default on a new install), in sections, in alphabetical order, without comments: look in the example for what a key means. Keys a new version adds are added, keys nothing reads any more -- or put in another section than their own -- are dropped, your values stay. Edit it with the app closed: the app rewrites it when a setting changes. Settings > "Reset the camera settings" puts every key back to its default (the previous file is kept as `rawfish-ir.conf.bak`) |
| app settings | `~/.config/com.rawfish/rawfish-ir/` -- `rawfish-ir.conf`, in sections (`[menu]`, `[shooting]`, `[viewfinder]`, `[white_balance]`, `[lens_shading]`, `[kept]`, `[advanced]`, `[internal]`, and one `[camera_N]` per camera for our JPG's processing; a key is read in its own section only), also holds hand-edited keys, among them `fineWarmthStep` (the warmth button's step, in 5 % units, 1..4), `fineTintStep` (the tint button's step in % of green, 1..25), `sigmaPreviewFps` (the Σ viewfinder's target frame rate, 1..30, 10 by default), `peakTweak` and `zebraTweak`; every key is described in `rawfish-ir.conf.example` next to it |
| device profile | `~/.local/share/com.rawfish/rawfish-ir/device-profiles/` |
| the engine's log | `~/.cache/com.rawfish/rawfish-ir/rawfish-ir.log` -- the first thing to send when something goes wrong. It keeps each start (device, settings), each photo and every error with the command that caused it. For a problem of focus, exposure or gestures, turn on Settings > Log > "Detailed log" and reproduce it: the log then keeps every command and reply, and for each photo the histogram the app showed and the photo's own (on the command line: `checks on`). With it, our JPG photos also carry their settings in their file names, the one place the Gallery's details show them: `photo_tiles4-half_1-48s_iso4428_col2_moire0_sharp3_..._717.jpg` (method and frames, the pose per frame, each frame's exposure and ISO, colour NR, moiré, sharpen) |
| the photo's settings | in every JPG of ours: the Exif's exposure and ISO (each frame's), the Software tag, and a sentence in ImageDescription, UserComment and XMP ("4 frames of 1/48 s at ISO 4428 (each 1/2 of the pose), merged by tiles; colour NR 2, moire 0, sharpen 3"), which desktop viewers show; the Sailfish Gallery shows the exposure but not the ISO nor the text |
