# (AI generated)
# Sony Xperia XA2 (H3113 / H3123 / H3133 / H4113 / H4133)

What was measured on an XA2 Dual (H4113, Sailfish X 5.1, October 2026).
RAWfish-indRAW's shipped profile for it: `android/data/device-profiles/sony-xperia-xa2.json`.

## Vendor binaries: use v16

| Sony binaries | JPEG / preview max | RAW | Otherwise |
|---|---|---|---|
| v16 (`..._8.1.6.4_r1_v16_nile.img`) | **5984x3392 (20.3 MP)** | 5984x3392 | identical |
| v17 (`..._8.1.6.4_r1_v17_nile.img`) | 5504x3104 (17 MP) | 5984x3392 | adds a 1080p60 video mode |

Both are accepted by Sailfish X's flash-config (`*_v1[67]_nile.img`). Only
the `oem_a` partition changes (`fastboot flash oem_a <image>`): system,
kernel and data stay.

## What the HAL declares (probe)

- Hardware level 3; capabilities backward_compatible, manual_sensor,
  manual_post_processing, read_sensor_settings, burst_capture, raw.
- Back camera: RAW 5984x3392 (a 16:9 readout of the 4:3 IMX300 -- the
  HAL offers no full 4:3 readout: its largest 4:3 is 4208x3120, 13.1 MP,
  the same as Jolla Camera's), ISO 50-4800, exposure 9 us - 0.61 s, AF
  auto / macro / continuous, closest focus 6.667 HAL diopters (~15 cm,
  "approximate" calibration), shading map 17x13.
- Front camera: RAW 3280x2464, ISO 40-3200, exposure up to 0.81 s
  (unverified), no AF.
- Request keys: everything RAWfish-indRAW drives, plus shading.mode, tonemap,
  colorCorrection, hotPixel, blackLevel.lock, optical stabilisation.
- Three Sony vendor request keys: 0x801b0000 (u8, 0 in every template),
  0x801b0001 (unset), 0x801c0000 (i32: 1 in the preview template, 0 in the
  still one). Setting them made no difference to anything tested.
- The lens-shading map is not listed among the result keys, yet the HAL
  delivers it: the DNG carries it.

## Measured

- **Exposure**: the HAL delivers exactly what it is asked up to 0.61 s
  (0.25 / 0.5 / 0.6 s applied to the nanosecond, the centre doubling from
  0.25 to 0.5 s) -- unlike the JP2, which announces 16 s and stops at 0.5 s.
  Profile: observed_shutter_max_ns = 610665315.
- **Vignetting**: corners at 21.8 % of the centre (gain 4.58 needed); the
  HAL's map corrects about 72 % of it. Colour shading weak (R/G 3 % from
  centre to corner). The on-phone assistant measures it in a minute (lens
  positions 0 to 6.7). Shipped: `shading_sony-xperia-xa2_cam0_5984x3392.json`
  (25 Oct 2026, six lens positions 0.27-6.71 with corner gains 4.68 down to
  4.28, two lights: 5500 K daylight and 4200 K LED), named in the profile
  for every XA2 variant.
- **Focus**: a tap-triggered scan works in manual exposure; the
  continuous AF stops following when the exposure is manual (it relies on
  the AE). The lens moving at the shutter (only full auto was safe) was
  ours, fixed in 0.47.56 (a stale trigger request); no capture delay is
  needed since.
- **Screen**: 1080x1920, 16:9, no notch. 4:3 photos fill the width (band
  words over the image, compact deck); 16:9 photos leave no room under the
  image and the viewfinder narrows.

## Also checked (25 Oct 2026)

Stacking (Sigma), the flash, the front camera's photos: work. RAW noise
and linearity not measured, but RAW files are sound (a flat field reads
plain; the calibration built from them matches the HAL's map).

The level did not work on 24 Oct and works on 25 Oct with the same
packages (QtSensors 5.6.3 installed on both phones): most likely the
sensor daemon (sensorfwd) was not delivering until a full reboot. The
log's '# level:' line (ui 172) says, two seconds after the level is
switched on, whether the module loaded and how many readings arrived.
