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

## AI Policy and Usage

The development of RAWfish has relied on, and will continue to rely on, AI assistance in various forms, including research, debugging, and code generation.
I understand that many community members may object to this approach. 

AI-assisted contributions are welcome and are subject to the [AI Usage Policy](AI_USAGE_POLICY.md).

## Versioning
RAWfish retains Jolla Camera’s version numbering as it stood at the time of the fork, with divergence beginning at version 1.3.0-1. If the project gains traction, I will detach it from the fork network to keep it separate from forks intended to contribute pull requests to Jolla Camera.

## Licensing 
RAWfish retains Jolla Camera’s BSD-3-Clause license.

