# (AI written, human written user-guite should be made)
# indRAW user guide

How to use the camera, screen by screen. For installing it, see
`BUILDING.md`; for the reasons behind each behaviour, `UI-NOTES.md`.

## The camera screen (portrait)

From top to bottom: a black band around the front camera with four text
buttons, the viewfinder, and the control deck.

### The four buttons around the front camera

| Button | What it does |
|---|---|
| **zebra** | stripes over what is about to clip (bold = on) |
| **peak** | thin red lines on the edges that are in focus |
| **level** | a cross on the true horizon, with the degrees (see "The level") |
| **AE** | back to full automatic exposure; also removes the reference circle |

### The viewfinder

It shows the picture as it will be: no brightening for display, what you see
is the exposure you get.

| Gesture | Effect |
|---|---|
| tap | sets the reference point: focus and exposure there (see "Focus") |
| tap on the circle | removes it (back to the camera's own choice, the centre) |
| two fingers apart / together | focus loupe, up to 12x, for the viewfinder only (the photo is not zoomed): up to the camera's limit (about 4x on the JP2) the sensor's real detail, beyond that an enlargement; a red frame and diagonal say it is on |
| one finger, loupe on | moves around in the magnified image; while a finger moves, the exposure does not change and the view is an enlargement (smooth); when it stops, the sensor's real detail comes back and the exposure, the wheels and AE work as usual. On cameras that can only magnify the centre, the real detail is largest in the middle and the view is enlarged more the further you move from it |
| tap, loupe on | focus and light measured at that point on a spot as small as the loupe shows -- a precise spot measurement -- then held |
| swipe down | opens the settings menu |
| swipe left to right | opens the gallery (a clearly sideways swipe: a slanting downward one opens the menu instead) |

While the loupe is on, the menu and gallery swipes are off.

### The deck

| Part | What it is |
|---|---|
| **FOCAL** | the camera, shown by its 35 mm-equivalent focal length; tap to switch camera |
| **TYPE** | raw (DNG) or jpg (the camera's JPEG) |
| **FOCUS** | circle, auto, or a distance (see "Focus") |
| histogram | the tones of **what the viewfinder shows** (zoomed or in the loupe: that area), per colour, with clipping bars at each end. In **jpg** it is measured on the processed image; in **raw** on the sensor data itself, so it sits further left and narrower (no tone curve lifting the shadows), and its left bar is bigger: it counts photosites at or below black, where noise puts about half of the truly dark ones -- shadows with no information, as the DNG will hold them. With a reference circle, the circle's own tones are drawn as bright outlines over it |
| **EV** wheel | brightness correction, in the camera's steps |
| shutter | takes the picture |
| **+ / −** (three of each, coloured) | left: warmth, by steps of 5 % red/blue (orange + warmer, blue − cooler); middle: digital zoom, white (the JPEG is cropped; a DNG stays full frame): a tap steps by 0.25, a quick double tap (within 0.15 s) goes to the max / back to x1, and **holding** zooms progressively -- slowly at first, faster the longer you hold, at the same pace whatever the magnification (x1 to x2 takes as long as x2 to x4); right: tint, by steps of 5 % green (magenta +, green −). A tap shows for a moment what it added |
| readouts beside the shutter | the white balance in force, estimated from the camera's own colour gains and its colour calibration: the colour temperature in kelvin, and the tint as Duv in thousandths (+ green side, − magenta) -- more reliable as a difference than as an absolute value (the camera's calibration reads a 2700 K bulb as ~3300 K); note that the camera's automatic balance only corrects part of the way: daylight photos come out a little cold, lamp-lit ones a little warm; they follow the automatic white balance and your steps alike; a readout is coloured while its shift is not zero, and a double tap on it resets that shift. **Measuring the white**: aim at something white or grey (in the circle if you set one, else in the middle) and hold a finger 2 s on either readout -- a ring fills around the shutter and a light-blue circle shows the zone being measured; releasing early cancels. The white is measured on the sensor's raw data and kept (both readouts turn light blue): JPEGs and the viewfinder are then neutral for that light, and the DNG records that white as its "as shot" balance (EXIF: manual white balance); the ± buttons still apply on top. When the app restarts, the measured white is applied for 6 s with a button **Keep previous manual WB** and a bar running out: a tap keeps it, otherwise the automatic balance comes back. A double tap on a readout returns to automatic. It refuses a white that is clipped (too bright) or too dark |
| **SPEED** and **ISO** wheels | exposure time and sensitivity |
| `ui NN` | the app's build number |

## Exposure

The SPEED and ISO wheels decide the mode:

| Mode | How to get there | What is automatic | With a circle |
|---|---|---|---|
| **automatic** | no wheel touched, or the AE button | speed and ISO, by the camera; the EV wheel corrects | the app measures the point and sets the exposure from it (a few quick corrections), then **holds** it until you remove the circle |
| **ISO priority** | touch the ISO wheel | the speed, by the app; EV corrects | the app measures the point instead of the centre; the point's light level shows above the circle |
| **speed priority** | touch the SPEED wheel | the ISO, by the app; EV corrects; past 1/2 s: Σ (stacked, raw) | same |
| **manual** | touch both | nothing; the EV wheel becomes a "+/−" number: stops brighter or darker than automatic would be | the number tells how **the point** is exposed; the point's light level shows above the circle |

The light level above the circle is an absolute EV at ISO 100 ("EV 8.3"),
what a spot light meter would read -- as the distance shows below the circle
in manual focus.

The AE button goes back to automatic from any mode, and removes the circle.
A photo is always exposed as the viewfinder shows: when a pair is held (a
wheel, or a circle in automatic) the photo takes that pair. Switching to
simple mode returns to automatic exposure; another camera, another
resolution or coming back to the app starts without circle or loupe.
Under a scene, the camera drives the exposure: a circle only focuses and
measures for the histogram.
"auto" under a wheel means the camera (or the app) is turning it.

## Focus

The FOCUS button cycles through three modes; the camera starts in **auto**:

| Mode | Without a tap | With a tap |
|---|---|---|
| **once** | one search when the mode is chosen, then the lens stays where it is | one search on the point; once sharp (solid circle) the lens is **held** there however you reframe, until you remove the circle, tap elsewhere (new search) or change mode |
| **auto** | the camera keeps focusing by itself, following the scene | the tap does not touch the focus: it only measures **the light** (a small sun instead of the circle) |
| **a distance** (e.g. "1.50 m") | manual focus at that distance; volume up / down moves it by small steps (up = farther), hold a key to move faster | one search on the point; the distance found becomes the manual distance, shown under the circle, and the volume keys continue from there |

A photo never starts a new search: it is taken where the focus is.

With a circle, the focus is searched first (the camera's own exposure stays
on during the search), then the light is measured on the point. The point
measured is the one the circle shows, at any zoom or loupe (in the loupe, a
small spot).

**The circle** marks the reference point. It stays on screen -- it never
disappears by itself: dotted while searching, solid when sharp, dashed when
it could not focus. Remove it with a tap on it, the AE button, or a change of
focus mode. In the loupe, a tap measures a spot as small as the loupe shows.

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
(10 frames per second by default, `sigmaPreviewFps` in `indraw.conf`), the
ISO raised to match. Measuring, the exposure loop and the focus search all
work one frame at a time, so a fast frame keeps them responsive. If the ISO
this needs is beyond the sensor's maximum, the frame is made longer, just
enough, up to 1/2 s: in dim light the viewfinder slows down.

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
| Mode | **simple** (JPEG only, no wheels, a brightness slider instead) or **advanced** |
| Timer | off, 3 s, 10 s |
| Format | 4:3 or 16:9. By default, the shape of the RAW the camera gives ("as the RAW": 4:3 on the JP2, 16:9 on the Xperia XA2); the other one crops. The viewfinder takes the photo's shape (grey bands only if the camera has no preview of that shape) |
| Resolution | the JPEG sizes this camera offers |
| Quality | JPEG quality |
| White bal. | automatic or a preset (daylight, cloudy, tungsten...) |
| Noise, Sharpen | the camera's noise reduction and sharpening **for the photos** |
| Vf noise, Vf sharpen | the same for the **viewfinder** only (shown when enabled in Settings); by default light noise reduction and no sharpening, which keeps a high-ISO preview clean |
| Bracket | off; **x3**: three shots as fast as the sensor allows, same exposure and focus (in JPEG a real burst, a few hundredths of a second apart; in raw one after the other); or three shots at −N, 0, +N stops |
| Gap | with Bracket x3 in JPEG: the three frames as fast as possible, or 0.1, 0.2, 0.3 or 0.5 s apart -- the sensor itself spaces them, exactly (at most what the camera delivers: 0.5 s on the JP2; with a pose longer than the gap, the frames follow each other at the pose's pace) |
| Flash | off, auto (when dark), on, torch (continuous light); shown only if the camera has a flash. In automatic exposure the camera measures with a pre-flash first, so the shot takes half a second to a second longer. When the exposure is held (a wheel touched, or a circle in automatic) "auto" cannot judge and does not fire; "on" always fires. No flash in a burst. |
| Grid | rule-of-thirds lines |
| Σ denoise | for Σ (stacked) shots only: 2x or 4x the frames for less noise, 2x or 4x the time |
| Focus cal. | the focus calibration assistant (when enabled in Settings) |
| Scene | only if Settings > indRAW > "Scenes in the camera menu" is on (off by default, not finished): the camera's scene modes, in simple and advanced mode; while one is on, the controls it takes over are greyed |

In **simple** mode the menu only keeps the flash, timer, format and white balance (this
can be changed in `~/.config/com.indraw/indraw/simple.json`, see the README).

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

## Settings (Settings app > Apps > indRAW)

| Setting | Meaning |
|---|---|
| File name prefix | the start of every file name (`photo` by default): letters, digits, `-`, `_` |
| JPEG folder, DNG folder | where the pictures go; under `~/Pictures` (the only place the app may write) |
| Reset to defaults | the three above |
| Scenes in the camera menu (not yet implemented) | shows the Scene column in the menu (off by default) |
| Start in manual focus and exposure | at start, manual focus at the last distance and both wheels pinned at the last ISO and speed (advanced mode; off by default) |
| Mode at start | simple, advanced, or as left (the last mode used) |
| Viewfinder processing in the camera menu | shows the "Vf noise" and "Vf sharpen" columns |
| Focus calibration in the camera menu | shows the "Focus cal." column |
| Create the device profile files | indRAW writes them the next time it opens (the date is shown once done) |

Changes take effect the next time the camera starts.

### Calibrating the focus

1. Turn on "Focus calibration in the camera menu".
2. Put something with sharp details at a measured distance, e.g. 0.5 m.
3. FOCUS on **once**, tap the target, wait for the solid circle.
4. Menu, column **Focus cal.**, tap **0.5m**: it blinks red; tap it **again**
   to record (this two-tap rule protects a calibration from a stray touch;
   the same for **clear**, which forgets all the recorded points).
5. Repeat for a few distances (0.25 m, 1 m, 2 m, ∞ on something far). One
   point already corrects the whole scale by a constant shift; with two or
   more the engine draws straight lines between them and extends the end
   ones, so every distance is covered; three or four points across the range
   are plenty.
6. Turn the switch off again.

## Files

| What | Where |
|---|---|
| photos | `~/Pictures/indRAW/` (or the folders set above) |
| DNG quick previews | `.previews/` inside the DNG folder (hidden, not in the system gallery) |
| commented references | `indraw.conf.example` next to `indraw.conf`, and `override-hal.example.jsonc` next to `override-hal.json` in `device-profiles/`: every key, what it does, its values and limits, as real comments. References only (never read; the .jsonc is not loadable JSON: copy the lines you need without the comments). Rewritten at each start of the app; your own files are never touched and stay without comments (edit `indraw.conf` with the app closed: it rewrites that file and drops comments) |
| app settings | `~/.config/com.indraw/indraw/` -- `indraw.conf` also holds hand-edited keys, among them `fineWarmthStep` (the warmth button's step, in 5 % units, 1..4), `fineTintStep` (the tint button's step in % of green, 1..25), `sigmaPreviewFps` (the Σ viewfinder's target frame rate, 1..30, 10 by default), `peakTweak` and `zebraTweak`; every key is described in `indraw.conf.example` next to it |
| device profile | `~/.local/share/com.indraw/indraw/device-profiles/` |
| the engine's log | `~/.cache/com.indraw/indraw/indraw-ui.log` -- the first thing to send when something goes wrong |
