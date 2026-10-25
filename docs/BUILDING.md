#(AI generated)
# Building indRAW from scratch

A step-by-step guide for someone who has never built a Sailfish app. It takes
about an hour the first time (most of it downloads), five minutes afterwards.

indRAW is two packages, built by two different toolchains, because the phone
runs two worlds at once:

| Package | What is in it | Built with | Why |
|---|---|---|---|
| `indraw-engine` | `libindraw.so` (talks to the camera through Android's Camera2) and the `indraw` command-line tool | the **Android NDK** for the library, the **Sailfish SDK** for the tool | the camera driver (HAL) is Android code; Sailfish reaches it through libhybris |
| `indraw-app` | the Qt/QML user interface | the **Sailfish SDK** | a normal Sailfish application |

Both are plain RPM files you copy to the phone and install. The engine's
version is written in one file, `indraw_version.h`, and the tool refuses to run
with a library of another version, so always install both packages together.

## 1. What you need

- A Linux PC (Ubuntu 22.04/24.04 or Fedora are known to work; macOS works with
  the SDK's VirtualBox engine but is not covered here), ~15 GB free, and a
  reasonably fast internet connection.
- The phone with **developer mode** on (Settings > Developer tools), a
  password set there, and the phone reachable from the PC (USB or Wi-Fi).
- `git`, `curl`, `unzip` on the PC: `sudo apt install git curl unzip` (Ubuntu).

## 2. Install the Sailfish SDK (once)

1. Download the **Sailfish SDK** installer for Linux from
   https://sailfishos.org/develop/ (free, needs no account) and run it:
   `chmod +x SailfishSDK-*.run && ./SailfishSDK-*.run`.
2. During installation choose the **Docker** build engine if the PC has
   Docker installed (`sudo apt install docker.io`, then add yourself to the
   `docker` group and log out/in); otherwise choose **VirtualBox** (the
   installer explains how to get it). Either works; Docker is lighter.
3. The SDK installs a command-line tool, `sfdk`, typically in
   `~/SailfishOS/bin/`. Put it on your PATH:
   `echo 'export PATH=$PATH:~/SailfishOS/bin' >> ~/.bashrc` (or `.zshrc`),
   then open a new terminal and check `sfdk --version`.
4. Install the **build target** matching the phone's Sailfish version (look it
   up in Settings > About product). List what the SDK offers and pick the
   `aarch64` one, for example:
   ```sh
   sfdk tools list                                  # shows the available targets
   sfdk tools install SailfishOS-5.1.0.11-aarch64   # the version on the phone
   sfdk config target=SailfishOS-5.1.0.11-aarch64   # remembered from now on
   # a 32-bit phone: the same with -armv7hl; the engine's library follows the target
   # switching between the two: change the target and run build-all.sh again
   # (the CLI and the app always build from clean; the binaries are checked
   # against the target's architecture; the packages of both architectures
   # stay side by side in RPMS/ and app/RPMS/)
   # both at once: ./scripts/build-all.sh --all  -- every installed aarch64
   # and armv7hl target, one after the other, the target put back after
   # (INDRAW_TARGETS="t1 t2" to choose them)
   ```
   The first build installs a few packages into that target (gcc, Qt
   headers); warnings about `NOKEY` signatures are normal.

## 3. Install the Android NDK (once)

The library is Android code, compiled by the NDK's clang for 64-bit ARM.

1. Download the NDK from https://developer.android.com/ndk/downloads
   (the Linux zip, version **r27** is what indRAW is built with; newer
   versions should work).
2. Unzip it somewhere permanent, e.g. `~/android-ndk-r27d`.
3. Tell the scripts where it is, in your shell profile:
   `echo 'export ANDROID_NDK_HOME=~/android-ndk-r27d' >> ~/.bashrc`.

Nothing else from Android is needed: no Android Studio, no SDK, no emulator.

## 4. Get the source

```sh
git clone https://github.com/Ric9k-TMO-FSO/indraw.git
cd indraw
```
(the repository is private for now: cloning needs access to it, and over
HTTPS a fine-grained personal access token as the password)
(or unzip a release archive and `cd` into it). Everything below runs from
this directory.

## 5. Build

```sh
./scripts/build-all.sh
```

That is all. The script builds the library with the NDK, then the tool and
the engine RPM in the Sailfish build engine, then the app RPM, and ends by
listing the two files:

```
RPMS/indraw-engine-0.41.0-1.aarch64.rpm
app/RPMS/indraw-app-0.13.0-1.aarch64.rpm
```

What the script does, if you prefer the pieces:

| Step | Command | Result |
|---|---|---|
| library | `./scripts/build-android.sh` | `build/android/libindraw.so` |
| engine RPM | `./scripts/build-engine-rpm.sh` (runs the step above too) | `RPMS/indraw-engine-*.rpm` |
| app RPM | `cd app && sfdk build` | `app/RPMS/indraw-app-*.rpm` |

A quick sanity check that needs no SDK at all (syntax and types of the C
code, with stubbed Android headers): `./scripts/check-host.sh`.

## 6. Install on the phone

1. Copy the two RPMs to the phone, e.g. over SSH (`nemo`/`defaultuser` is
   the user name shown in developer tools; the password is the one you set):
   ```sh
   scp RPMS/indraw-engine-*.rpm app/RPMS/indraw-app-*.rpm defaultuser@<phone-ip>:Downloads/
   ```
2. On the phone (Terminal app or `ssh defaultuser@<phone-ip>`), become root
   and install both at once:
   ```sh
   devel-su
   cd /home/defaultuser/Downloads
   pkcon install-local -y indraw-engine-*.rpm indraw-app-*.rpm
   ```
   To update later, the same command with the new files; to remove:
   `pkcon remove indraw-app indraw-engine`.
3. indRAW appears in the app grid. The first launch creates
   `~/Pictures/indRAW/` for the photos and `~/.cache/com.indraw/indraw/indraw-ui.log`,
   the engine's log -- the first thing to look at if something is wrong.

### Device profile (optional but recommended)

The engine adapts to the phone through two JSON files in
`~/.local/share/com.indraw/indraw/device-profiles/` (see `docs/DEVICE-PROFILES.md`):

```sh
indraw probe          # writes generated-hal.json: what the camera driver claims
```
and, next to it, `override-hal.json` with what you measured (the single-exposure
ceiling, the peaking/zebra thresholds, the focus calibration). Without it the
built-in defaults apply, which is fine to start with.

## 7. Rebuilding from zero

When in doubt, or after changing SDK or NDK, wipe every build product and
start again. Nothing you need is lost: the sources are in git, the device
profile and the settings are on the phone.

```sh
./scripts/build-all.sh --clean      # removes build/android, RPMS/, sailfish/indraw; sfdk build --clean
```

For a truly clean slate:

```sh
git clean -xdf -e patches/          # deletes every untracked file (build outputs, .sfdk/ state)
git status                          # should be empty
./scripts/build-all.sh
```

If the SDK itself misbehaves (a target that will not update, a build engine
that will not start): `sfdk engine stop`, then `sfdk tools remove <target>`
and install it again (step 2.4). Reinstalling the whole SDK is rarely needed.

On the phone, to start as a brand-new user:

```sh
pkcon remove indraw-app indraw-engine
rm -rf ~/.config/com.indraw ~/.cache/com.indraw ~/.local/share/com.indraw
# your photos in ~/Pictures/indRAW/ are yours: keep or delete them
```

## 8. When something goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `ld.lld: error: undefined symbol: ...` while building the library | a C function the Android library does not link (e.g. from libm) | the library only links `camera2ndk` and `mediandk` on purpose; keep it that way in your code |
| `nothing provides 'libc.so()(64bit)'` at install | an old spec let rpm scan the Android library for dependencies | fixed in `rpm/indraw-engine.spec` (scanner excluded); rebuild |
| `version mismatch: CLI x, library y` in the log | engine tool and library from different builds | install both RPMs from the same build |
| `jpeg size not listed` / `raw16 size not listed` in the log | a size the camera does not offer (another camera, 16:9) | the app asks for `max` now; if you drive the tool by hand, use `max` |
| white screen from the app grid, works from a terminal | the app ran sandboxed and could not find its files | fixed by installing to `/usr/share/indraw`; make sure the RPM is the one you just built (`rpm -ql indraw-app`) |
| `engine stopped` in the viewfinder | the engine process died | read the end of `~/.cache/com.indraw/indraw/indraw-ui.log`; the app retries three times by itself |
| `sfdk: no target set` | step 2.4 was skipped | `sfdk config target=...` |

## 9. How the repository is organised

```
android/      the Android library (Camera2 through the NDK): indraw.c, dngwrite.c, exifwrite.c
sailfish/     the command-line tool that loads it through libhybris, and controller.c (the
              controller layer, docs/ARCHITECTURE.md) with its host test in tests/
app/          the Qt/QML application (qml/, indrawpreview.cpp, the spec in rpm/)
rpm/          the engine's RPM spec
scripts/      build and diagnostic scripts (build-all.sh is the one to remember)
docs/         USER-GUIDE.md, ENGINE.md (command reference), UI-NOTES.md (what the app does and why), DEVICE-PROFILES.md,
              GENERATED-HAL.md, STACKING.md, PORTING.md, ROBUSTNESS.md, ROADMAP.md, ARCHITECTURE.md,
              this file
indraw_version.h   the engine's version, shared by the library, the tool and the spec
patches/      the project's history as a patch series (not needed to build)
```
