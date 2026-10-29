# Credits and lineage

## Human written

RAWfish-indRAW is written and maintained by Mad_Dev and/or Ric9k with AI assistance,
every change reviewed and tested on the device.
RAWfish-indRAW is modelled on RAWfish (itself a fork of Jolla Camera): it takes
RAWfish's logic and the look of its interface, without copying its code.
Thanks to mad_dev, who unlocked the RAW on the JP2.
Thanks to Jolla too, who opensourced the camera.


## LLM generated

- **RAWfish** (a Jolla Camera fork by its maintainer, Camera2 on the Jolla
  Phone 2) is the reference for the app's layout and components: the caption
  header and four-column control deck, the cycle buttons and value wheels,
  the swipe-down settings menu and the gallery swipe, the Camera2 mode names,
  the capability-derived wheel values, and the SF2P/SF2M frame-and-events
  protocol between the engine process and the viewfinder, whose format
  RAWfish-indRAW follows (reimplemented, not copied). `docs/UI-NOTES.md` ("vs RAWfish") says what RAWfish-indRAW deliberately
  does differently. RAWfish is BSD-3-Clause.
- **Jolla Camera** (Jolla Ltd., Open Mobile Platform LLC, Jolla Mobile Ltd),
  BSD-3-Clause, is RAWfish's base and therefore the origin of that design
  lineage; its license text is kept in `LICENSES/Jolla-Camera-BSD-3-Clause.txt`.
- The Jolla system shutter icon (`image://theme/icon-camera-shutter`) is
  provided by Sailfish OS at runtime and is not part of this repository.

In short: the logic and the look are RAWfish's, the code is new. No source
file of Jolla Camera or RAWfish is copied into RAWfish-indRAW: the engine
(`android/`, `sailfish/`) and the app (`app/`) are written from scratch, with
AI assistance, and tested on a Jolla Phone 2. The whole of RAWfish-indRAW is
BSD-3-Clause (`LICENSE`).

