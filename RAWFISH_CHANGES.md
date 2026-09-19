# RAWfish Changes From Jolla Camera

RAWfish is based on Jolla Camera, with the app renamed and extended for Camera2 RAW/JPEG capture work.

## Main Changes

- Renamed the app branding to RAWfish.
- Added the RAWfish launcher icon.
- Changed the app package, binary, desktop files, D-Bus service, settings entry, and dconf paths to use `rawfish`.
- Added a Camera2 bridge for direct Android Camera2 still capture.
- Added warm Camera2 preview capture paths for faster JPEG and RAW capture.
- Added RAW16 capture with JSON metadata sidecars.
- Added DNG output using libtiff.
- Added EXIF/metadata handling for RAW/DNG/JPEG-from-RAW outputs.
- Added RAWfish file naming and storage:
  - Folder: `Pictures/RAWfish`
  - Prefix: `RAWfish_`
- Updated the gallery model to recognize RAWfish filenames.
- Added live histogram support from Camera2 preview frames.
- Added clipping colors to the first and last histogram bars.
- Added Camera2 focus and metering tap support.
- Moved the settings pull-down trigger into the black strip above the preview.
- Adjusted the preview layout so the full preview can be used for focus/metering.
- Added Camera2 controls for speed, size, focus, scene, noise reduction, white balance, tint, exposure, rotation, timeout, and JPEG quality.
- Added live ISO, shutter speed, lens, and histogram display from Camera2 metadata.
- Fixed startup defaults for Auto ISO and Auto shutter speed.
- Fixed repeated settings hint behavior.
- Bumped version to `1.3.1-1`.

## Device Limitations

RAWfish is currently built around behavior observed on the Jolla Phone 2. Other devices may need porting work.

- Camera2 support depends on the Android HAL exposing compatible Camera2 APIs through libhybris.
- RAW capture depends on the camera HAL exposing usable `RAW_SENSOR` / RAW16 output.
- DNG quality depends on complete and correct sensor metadata from the HAL.
- Camera IDs may differ between devices, so front/back camera selection may be wrong.
- Preview orientation, mirroring, crop, and focus coordinates may need per-device adjustment.
- Lens, ISO, shutter speed, aperture, and focus metadata may be missing or incorrect on some HALs.
- Fast warm JPEG/RAW capture depends on the preview helper staying alive and receiving frames.
- Vendor library namespace issues may block Camera2 on some devices.
- Performance varies heavily by sensor size, HAL behavior, storage speed, and CPU.
- The app assumes Sailfish permissions and sandbox rules similar to Jolla Phone 2.
- RAWfish has not been validated as a generic Camera2 app for all Sailfish devices.

If porting to another phone, first verify the Camera2 probe, preview helper, JPEG capture, RAW16 capture, and metadata output before testing DNG conversion.
