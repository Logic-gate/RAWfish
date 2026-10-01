# RAWfish Camera App for JP2

This is a fork of Jolla Camera, intended to serve as a working experiment until we finally get Camera2 support in droidmedia. Please note that this app should only be tested on the Jolla Phone 2.

For the bridge and related probes, please look under [sfos-camera2-bridge](sfos-camera2-bridge)

Please note that the project still retains unused code from Jolla Camera.

At this point, RAWfish will not be released on OpenRepos due to outstanding issues and the work required to support more devices. Currently, RAWfish is built around behavior observed on the Jolla Phone 2. Other devices may require porting work for the following reasons:

* Camera2 support depends on the Android HAL exposing compatible Camera2 APIs through libhybris.
* RAW capture depends on the camera HAL exposing usable `RAW_SENSOR` RAW16 output.
* DNG quality depends on complete and correct sensor metadata from the HAL.
* Camera IDs may differ between devices, so front or back camera selection may be incorrect.
* Preview orientation, mirroring, cropping, and focus coordinates may need per-device adjustments.
* Lens, ISO, shutter speed, aperture, and focus metadata may be missing or incorrect on some HALs.
* Fast warm JPEG RAW capture depends on the preview helper staying alive and receiving frames.
* Vendor library namespace issues may block Camera2 on some devices.
* Performance varies considerably depending on sensor size, HAL behavior, storage speed, and CPU.

## How does RAWfish differ from Jolla Camera?

* Added a Camera2 bridge for direct Android Camera2 still capture.
* Added warm Camera2 preview capture paths for faster JPEG and RAW capture.
* Added RAW16 capture with JSON metadata sidecars.
* Added DNG output using `libtiff`.
* Added EXIF and metadata handling for RAW, DNG, and JPEG-from-RAW outputs.
* Updated the gallery model to recognize RAWfish filenames.
* Added live histogram support from Camera2 preview frames, with clipping colors applied to the first and last histogram bars.
* Added Camera2 support for tap-to-focus and tap-to-meter.
* Added Camera2 controls for speed, size, focus, scene, noise reduction, white balance, tint, exposure, rotation, timeout, and JPEG quality.
* Added live displays of ISO, shutter speed, and lens information from Camera2 metadata, alongside the live histogram.

## What Currently Works

* The main camera (ID 0) on the JP2 (Sony IMX766, 50 MP, C-PHY MIPI).
* RAW16, DNG, and JPEG capture, including RAW-to-JPEG conversion and native JPEG capture.
* Controls for speed, size, focus, scene, noise reduction, white balance, tint, exposure, rotation, timeout, and JPEG quality.

## Contributions and Credit Where It’s Due
[ric9k](https://forum.sailfishos.org/u/ric9k/summary) has been instrumental in smoothing out the rough edges and providing valuable feedback.
Tadi for Bracketing([licenses/RawBracket-MIT.txt](licenses/RawBracket-MIT.txt).) and general support. 

## AI Policy and Usage

The development of RAWfish has relied on, and will continue to rely on, AI assistance in various forms, including research, debugging, and code generation.
I understand that many community members may object to this approach. 

AI-assisted contributions are welcome and are subject to the [AI Usage Policy](AI_USAGE_POLICY.md).

## Versioning
RAWfish retains Jolla Camera’s version numbering as it stood at the time of the fork, with divergence beginning at version 1.3.0-1. If the project gains traction, I will detach it from the fork network to keep it separate from forks intended to contribute pull requests to Jolla Camera.

## Licensing 
RAWfish retains Jolla Camera’s BSD-3-Clause license.

## Photo metadata

RAWfish shares one capture-metadata model across Simple and Advanced modes, using libexif for JPEG EXIF and libtiff for DNG. Available standard tags include orientation; additional capture details are stored as JSON in EXIF UserComment. GPS follows Save location. Full JSON sidecars are retained, including for bare RAW16/RAW10 files; oversized comments identify fields retained only in the sidecar. Building requires libexif-devel and libtiff-devel.

## Exposure controls

Simple mode uses automatic exposure with EV compensation. Advanced uses the ISO and shutter carousels’ Auto entries, with no separate mode buttons. Optional estimated metering is disabled by default under Settings → Apps → RAWfish; The manual meter is hidden when disabled.

Settings → Apps → RAWfish also offers **Experimental exposure layout** (off by default). In Advanced photo mode it widens histogram/EV and places Speed and ISO at shutter-centre height, with scrollable values below. The Speed and ISO wheels are straight. Touching either shows plain floating text matching its selected value above the finger (beside it near the top edge), without a background or border. It follows the gesture, stays through inertial scrolling and fades after 250 ms at rest. Auto is labelled explicitly with the actual value underneath. Turning it off restores the standard layout without changing exposure.


### RAW bracketing

With RAW selected, **2-stop pair**, **4-stop pair** and **6-stop pair** capture
the current exposure followed by a shorter exposure at the same ISO
(shutter time divided by 4, 16 or 64).
The camera's limits can reduce that separation. JPEG mode has no bracketing;
cameras must support RAW, manual sensor controls and exposure-result metadata.
Existing `ev1`/`ev2` preferences select the 2/4-stop pair respectively; `ev3`
selects the 6-stop pair. Larger gaps can increase noise in recovered highlights.

The background CPU renderer adapts Tadi's RawBracket algorithm: normalize
using actual shutter/ISO, keep unclipped long-frame pixels, and recover clipped
highlights using the short frame only when its normalized luminance is higher.
It includes the reference's WB-neutral highlight ceiling, conservative gain
refinement and highlight compression. The normal viewfinder and focus controls
are unchanged. There is no alignment or deghosting; moving edges can leave
artifacts, especially with the sequential cold-capture fallback.

One merged JPEG is saved with EXIF/orientation and a JSON sidecar describing both
exposures, actual separation, frame gap, gain refinement and merge time. Existing
RAW/DNG saving preferences retain individual sensor frames, not a synthetic DNG.
Missing or mismatched exposure/colour metadata causes a capture error rather than
an incorrectly normalized merge. MIT attribution is in
[licenses/RawBracket-MIT.txt](licenses/RawBracket-MIT.txt).

Non-build capture-control checks: `node tests/bracket.js`. The native numerical
and RAW10/RAW16 integration tests are in `tests/rawbracket.pro`; compile/run them
when building. Device validation should cover range-limited pairs, motion,
colour/highlight recovery, portrait/landscape orientation, cancellation, and
preview restoration after both warm bursts and cold captures.

For CPU/shader parity, build the small adapter in `tests/rawbracketpixels.pro`,
then run `python3 tests/bracket_reference.py /path/to/rawbracket_pixels` with
`numpy`, `moderngl` and EGL available. It compares 24 synthetic Bayer/WB/gain
cases against the sibling RawBracket checkout's production shader.

The capture-command parser has standalone C regression tests in
`sfos-camera2-bridge/tests/preview_commands.c` (build instructions in that file).
Diagnostics log `capture-command received` and `capture-command submitted`;
RAW bracket timeouts include the number of frames received out of two.
