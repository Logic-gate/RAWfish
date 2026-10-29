# (AI generated)
# Robustness and safety review (October 2026)

What was examined, what it relies on, and what remains. Written for whoever
installs RAWfish-indRAW on a device that is not the author's.

## Privilege and reach

- Both the engine (`rawfish-ir-cli` + `librawfish-ir.so`) and the app run as the normal
  user, from the Sailjail sandbox the app's `.desktop` declares: **Camera,
  Pictures, MediaIndexing** and nothing else. No network, no location, no
  other user directories. The engine process is a child of the app and
  inherits the same sandbox.
- Everything the engine writes goes to `~/Pictures/RAWfish-ir/` (captures; the
  settings may point the JPEG and DNG folders elsewhere under `~/Pictures`,
  and the prefix is limited to letters, digits, `-`, `_`),
  `~/Pictures/Screenshots/` (the app's screenshots), `~/.cache/com.rawfish/rawfish-ir/`
  (the engine log) and `~/.config/com.rawfish/rawfish-ir/` (settings). It reads
  `~/.local/share/com.rawfish/rawfish-ir/device-profiles/`.
- Photos carry no location data. The Exif holds exposure, ISO, lens,
  date/time, orientation, the device's make/model and the software version.

## Inputs the engine accepts, and how they are bounded

- Capture names on stdin are letters, digits, `-` and `_` only (`valid_name`);
  the engine appends the timestamp and extension itself, so no caller can
  choose a path.
- Sizes, exposures, ISO, zoom, regions, white-balance shifts, scene numbers
  and AF waits are parsed with range checks and refused with a JSON error
  otherwise; nothing unparsed reaches the HAL.
- `override-hal.json` is scanned (not parsed as full JSON) for numeric fields
  by camera id, with a 1 MB cap; a malformed file is ignored. The key is
  `"camera_id"`.
- The HAL's JPEG is treated as untrusted bytes: every segment length is checked
  against the buffer before the Exif writer reads or skips a segment.
- `noiseprofile FILE` and `preview-frame FILE` take a path from stdin: these
  are developer commands for the command line (same user, same privileges);
  the app never sends them.

## Inputs the app accepts

- Settings come from an INI file and are parsed and clamped to what the menu
  offers; `simple.json` is used only in its expected shape. A corrupt file
  cannot drive the engine into a refused value (it used to stop it).
- The caps line from the engine (sizes, scene modes, thresholds) is parsed
  into typed values; a scene the camera does not list is sent as off.
- The gallery deletes only files of its own folder, through the system's
  remorse timer.

## Failure behaviour

- The engine checks the library's version against its own and refuses a
  mismatch with a clear message.
- A photo's 3A wait has a bounded timeout; a timeout is reported, never a hang.
  The meter thread never leaves a RAW frame unreleased; photos and meter
  frames cannot cross on the RAW stream (quiesce/release).
- An unexpected engine exit is retried three times with a growing pause, then
  reported; a deliberate stop is never mistaken for a crash.
- The camera is never left open without a reason: the engine stops when the
  app goes inactive (screen off, app behind) -- except during a stacked
  exposure, which is allowed to finish and is followed by the stop. If the
  app dies mid-stack, the engine sees its stdin close, aborts the stack at
  the next sub-frame, writes the frames accumulated so far and closes the
  camera. The user can abort from the screen the same way (SIGUSR1).
- Files are written through `.part` + rename: a failure never leaves a
  truncated photo behind. An Exif-writer failure falls back to the plain JPEG.

## Known limits (not safety issues)

- A stacked exposure (Σ) is previewed at the sub-frame length with the ISO
  raised; it cannot show a 16 s exposure live.
- Manual white balance is a shift from the HAL's AWB, not an absolute Kelvin
  scale.
- The RAW16 output is never cropped by the HAL: a zoomed shot yields a
  full-frame DNG.
- At high ISO the viewfinder grows maze-like patterns and an afterimage
  after some seconds on a still scene (RAWfish too); photos are not
  affected. Hypothesis: the HAL's temporal noise reduction on the preview,
  out of reach of the standard keys -- see ROADMAP.md.

## From the October 2026 code review (0.47.58)

A full static and sanitizer pass (gcc -fanalyzer, ASan/UBSan on the eight
test suites) found no memory hazard; what it found was design fragility,
fixed as follows:
- **The event pipe never blocks a camera thread.** emit_event() runs on
  the Camera2 threads under out_lock; the controller's reader may be busy
  for a second (a focus command waiting for the lens) while 'ae' and
  'vfmeter' lines keep coming, so a blocking write stalled the preview. The
  pipe's write end is now non-blocking and 1 MB where Linux allows; a
  state line that finds the pipe full is dropped (the next supersedes it),
  a one-off event waits up to 300 ms then is dropped and counted
  (events_dropped in the close reply).
- **SIGPIPE is ignored** in the CLI: a dead interface makes the writes fail
  with EPIPE (handled) instead of killing the engine before close_all().
- Not a bug, checked: ctl_stop()'s set_event_fd(-1) then close(c.wr) --
  the fd is read and written under out_lock, and the setter takes the same
  lock, so no writer can be left with the old fd.
- **dng_write no longer assembles the whole file in memory**: the header
  and directories go in one buffer, the pixels are written from the
  caller's copy (100 MB less at 8192x6144 on the JP2; the first ENOMEM
  candidate on a 32-bit phone).
- trail_put() reserves the padding byte explicitly (it relied on the
  blob's even length); exifwrite remembers a put() that did not fit and
  refuses the APP1, and refuses a .part path that would be cut.
- shading_parse refuses two entries with the same key (no division by
  zero was possible -- sh_bracket stops before a zero interval -- but the
  second entry was dead data).
- fail() quotes its error text (a path in it stays valid JSON).
- The string-search JSON readers (the CLI's override / profile readers,
  shading.h's sh_key) skip a key named inside another string (an escaped
  quote, as in a "_comment": \"peak_edge\"); tested.
- The benign races named by the review are commented at the spot
  (g.sent_crop: one thread; g.state_events: a flag set once). The engine's
  code cannot run under ThreadSanitizer on the host (it needs the Camera2
  HAL); the host tests do run under ASan/UBSan.
- check-host.sh checks that the engine's longest event line fits the
  controller's reader buffer.

Done since: the HAL's Exif APP1 merged with ours (0.47.59/60); the YUV
conversion per row (0.47.61); the app's frame parsing by index and the
aids' overlay on a worker thread (ui 167); the engine stopped without
waiting on the UI thread, killed after 3 s if it does not end, the
destructor alone waiting (ui 168); a truncated JSON reply replaced by a
valid error reply (0.47.62).

### Second pass (0.47.64): the Exif merge's own reader

The 288 lines that read the HAL's APP1 (0.47.59) were reviewed in turn:
- every bound is subtractive (`off > tl || len > tl - off`): on a 32-bit
  phone (the XA2) size_t is 32 bits and an offset read from the APP1 near
  2^32 plus a small length wrapped past an additive test;
- a thumbnail needs at least 4 bytes (SOI + EOI): a 1-byte one made the
  `t[to + 1]` check read one past the TIFF;
- the entry lists hold the HAL's maximum (64) plus all of our tags (8 /
  16); a refused merge is an error, not a silent drop.
Tests: a value offset at 0xFFFFFFFC, a 1-byte thumbnail, an IFD0 pointer
near 2^32, a HAL Exif IFD of 64 foreign entries keeping all 15 of ours
(59 checks under ASan/UBSan; a 32-bit build was not possible on the host,
the subtractive form is correct on both). The host-only `extern
__system_property_get` next to the NDK include is gone.

### Third pass (0.1.1-0.1.3): three audits of the whole tree

Bugs fixed: parseFrames' 32-bit size check (a crafted header crashed the
app), the controller's lock initialisation, silent Exif drops, the PC DNG
converter's hard-coded Bayer pattern / make / orientation and a stacking
corner case, the viewfinder Noise / Sharpen columns. Packaging: %license,
one LICENSE, pinned and checksummed NDK headers. Concurrency: the contract
written and partly enforced (lockcheck.awk, three rules), the controller
under ThreadSanitizer and Helgrind. Not done, by decision: a JSON library
instead of the bounded hand readers (ROADMAP); restricting the CLI's
--jpgdir / --dngdir (it is a terminal tool, not sandboxed, by design: the
README says so).
