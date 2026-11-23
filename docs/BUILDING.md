#(AI generated)
# Building RAWfish-ir from scratch

A step-by-step guide for someone who has never built a Sailfish app. It takes
about an hour the first time (most of it downloads), five minutes afterwards.

RAWfish-ir is made of two parts, built by two different toolchains, because the phone
runs two worlds at once:

| Part | What it is | Built with | Why |
|---|---|---|---|
| the engine | `librawfish-ir.so` (talks to the camera through Android's Camera2) and the `rawfish-ir-cli` command-line tool | the **Android NDK** for the library, the **Sailfish SDK** for the tool | the camera driver (HAL) is Android code; Sailfish reaches it through libhybris |
| the app | the Qt/QML user interface | the **Sailfish SDK** | a normal Sailfish application |

One spec, `rpm/rawfish-ir.spec`, builds both parts into two packages:

| Package | Holds | For |
|---|---|---|
| `rawfish-ir-all-in-one` | the app **and** its engine | the camera app (what nearly everyone installs) |
| `rawfish-ir-cli-only` | the engine only, no app | the command line, or an interface of your own |

Install one of the two: they hold the same engine files and exclude each
other. The app and its engine can no longer be of different versions (up to
0.3.9-48x they were two packages, rawfish-ir-cli and rawfish-ir-gui,
installed separately; an app with an older engine lost its photos). Both
replace that former pair when installed over it. The version is written in
one file, `rawfish-ir-version.h`; the tool refuses a library of another one.

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
   (the Linux zip, version **r27** is what RAWfish-ir is built with; newer
   versions should work).
2. Unzip it somewhere permanent, e.g. `~/android-ndk-r27d`.
3. Tell the scripts where it is, in your shell profile:
   `echo 'export ANDROID_NDK_HOME=~/android-ndk-r27d' >> ~/.bashrc`.

Nothing else from Android is needed: no Android Studio, no SDK, no emulator.

## 4. Get the source

```sh
git clone https://github.com/Logic-gate/RAWfish.git
cd RAWfish
git checkout indraw
```
(the repository is private for now: cloning needs access to it, and over
HTTPS a fine-grained personal access token as the password)
(or unzip a release archive and `cd` into it). Everything below runs from
this directory.

## 5. Build

```sh
./scripts/build-all.sh
```

That is all. The script builds the library with the NDK, then the tool, the
app and the two RPMs in the Sailfish build engine, and ends by listing them:

```
RPMS/rawfish-ir-all-in-one-0.3.10-516.beta.aarch64.rpm
RPMS/rawfish-ir-cli-only-0.3.10-516.beta.aarch64.rpm
```

What the script does, if you prefer the pieces:

| Step | Command | Result |
|---|---|---|
| library | `./scripts/build-android.sh` | `build/android/librawfish-ir.so` |
| the RPMs | `./scripts/build-rpm.sh` (runs the step above too) | `RPMS/rawfish-ir-all-in-one-*.rpm`, `RPMS/rawfish-ir-cli-only-*.rpm` |

The scripts number each build's Release with the last patch applied (the
newest commit whose subject starts with four digits: `0424-...` gives
`0.3.8-424.beta`), so that the RPMs of every build can stay in one folder
and each installs as an update of the one before
(`scripts/with-build-release.sh`; `BUILD_NO=<n>` to choose the number).
A plain `sfdk build` keeps the spec's own Release.

A quick sanity check that needs no SDK at all (syntax and types of the C
code, with stubbed Android headers): `./scripts/check-host.sh`. With Qt 5's
tools installed (`qttools5-dev-tools`, `qtdeclarative5-dev-tools` on
Debian/Ubuntu) it also checks the translations (`lupdate`) and the QML's
syntax (`qmllint`); without them those two steps are skipped and say so. The
QML is always checked for JavaScript newer than the phone's Qt 5.6 (`let`,
`const`, `=>`, template strings: `scripts/qmlcheck.py`).

## 6. Install on the phone

1. Copy the RPM to the phone, e.g. over SSH (`nemo`/`defaultuser` is the
   user name shown in developer tools; the password is the one you set):
   ```sh
   scp RPMS/rawfish-ir-all-in-one-0.3.10-*.aarch64.rpm defaultuser@<phone-ip>:Downloads/
   ```
2. On the phone (Terminal app or `ssh defaultuser@<phone-ip>`), become root
   and install it:
   ```sh
   devel-su
   cd /home/defaultuser/Downloads
   pkcon install-local -y rawfish-ir-all-in-one-0.3.10-*.rpm
   ```
   Over the earlier names (`rawfish-ir` up to 0.3.9-50x, or the pair
   rawfish-ir-cli + rawfish-ir-gui) it replaces them. To update later, the
   same command with the new file; to remove:
   `pkcon remove rawfish-ir-all-in-one`. The engine alone, for the command
   line: `rawfish-ir-cli-only-*.rpm` instead (it removes
   rawfish-ir-all-in-one, and the reverse). To go back to a build from
   before the rename (a `rawfish-ir-0.3.9-…` file), remove
   rawfish-ir-all-in-one first: rpm will not install the old name over it.
3. RAWfish-ir appears in the app grid. The first launch creates
   `~/Pictures/RAWfish-ir/` for the photos and `~/.cache/com.rawfish/rawfish-ir/rawfish-ir.log`,
   the engine's log -- the first thing to look at if something is wrong.

### Device profile (optional but recommended)

The engine adapts to the phone through two JSON files in
`~/.local/share/com.rawfish/rawfish-ir/device-profiles/` (see `docs/DEVICE-PROFILES.md`):

```sh
rawfish-ir-cli probe          # writes generated-hal.json: what the camera driver claims
```
and, next to it, `override-hal.json` with what you measured (the single-exposure
ceiling, the peaking/zebra thresholds, the focus calibration). Without it the
built-in defaults apply, which is fine to start with.

## 7. Rebuilding from zero

When in doubt, or after changing SDK or NDK, wipe every build product and
start again. Nothing you need is lost: the sources are in git, the device
profile and the settings are on the phone.

```sh
./scripts/build-all.sh --clean      # removes build/android, RPMS/, sailfish/rawfish-ir-cli; sfdk build --clean
```

For a truly clean slate:

```sh
git clean -xdf                      # deletes every untracked file (build outputs, .sfdk/ state)
git status                          # should be empty
./scripts/build-all.sh
```

If the SDK itself misbehaves (a target that will not update, a build engine
that will not start): `sfdk engine stop`, then `sfdk tools remove <target>`
and install it again (step 2.4). Reinstalling the whole SDK is rarely needed.

On the phone, to start as a brand-new user:

```sh
pkcon remove rawfish-ir-all-in-one
rm -rf ~/.config/com.rawfish ~/.cache/com.rawfish ~/.local/share/com.rawfish
# your photos in ~/Pictures/RAWfish-ir/ are yours: keep or delete them
```

## 8. When something goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `ld.lld: error: undefined symbol: ...` while building the library | a C function the Android library does not link (e.g. from libm) | the library only links `camera2ndk` and `mediandk` on purpose; keep it that way in your code |
| `nothing provides 'libc.so()(64bit)'` at install | an old spec let rpm scan the Android library for dependencies | fixed in `rpm/rawfish-ir.spec` (scanner excluded); rebuild |
| `version mismatch: CLI x, library y` in the log | engine tool and library from different builds | install both RPMs from the same build |
| `jpeg size not listed` / `raw16 size not listed` in the log | a size the camera does not offer (another camera, 16:9) | the app asks for `max` now; if you drive the tool by hand, use `max` |
| white screen from the app grid, works from a terminal | the app ran sandboxed and could not find its files | fixed by installing to `/usr/share/rawfish-ir`; make sure the RPM is the one you just built (`rpm -ql rawfish-ir-gui`) |
| `engine stopped` in the viewfinder | the engine process died | read the end of `~/.cache/com.rawfish/rawfish-ir/rawfish-ir.log`; the app retries three times by itself |
| `sfdk: no target set` | step 2.4 was skipped | `sfdk config target=...` |

## 9. How the repository is organised

```
android/      the Android library (Camera2 through the NDK): rawfish-ir.c, dngwrite.c, exifwrite.c
sailfish/     the command-line tool that loads it through libhybris, and controller.c (the
              controller layer, docs/ARCHITECTURE.md) with its host test in tests/
app/          the Qt/QML application (qml/, rawfish-ir-preview.cpp, the spec in rpm/)
rpm/          the engine's RPM spec (rawfish-ir-cli)
scripts/      build and diagnostic scripts (build-all.sh is the one to remember)
docs/         USER-GUIDE.md, ENGINE.md (command reference), UI-NOTES.md (what the app does and why), DEVICE-PROFILES.md,
              GENERATED-HAL.md, STACKING.md, PORTING.md, ROBUSTNESS.md, ROADMAP.md, ARCHITECTURE.md,
              this file
rawfish-ir-version.h   the version, shared by the library, the tool and both specs
```

## Versions and git tags

The packages carry the version written in their spec (rpm/rawfish-ir.spec).
sfdk would otherwise derive them from a git tag found in the history
(`git describe`) -- after a release tag such as v0.47.69 both packages
came out as 0.47.69+main.<date>.<hash>, and the app's "requires engine >="
could not be met. The build scripts therefore run `sfdk -c no-fix-version
build`. Building by hand: add the same option.

## dconf keys

The system Settings page's values (everything else is in `rawfish-ir.conf`):

| Key | Values | Default |
|---|---|---|
| `/apps/rawfish-ir/output/prefix` | file name prefix: letters, digits, `-` `_` (not first); empty = no prefix | `photo` |
| `/apps/rawfish-ir/output/jpgDir`, `dngDir` | folders, under `~/Pictures` | `~/Pictures/RAWfish-ir` |
| `/apps/rawfish-ir/menu/startMode` | `last`, `simple`, `advanced` | `last` |
| `/apps/rawfish-ir/menu/startManual` | start in manual focus and exposure | `false` |
| `/apps/rawfish-ir/menu/showScenes` | the Scene column in the menu | `false` |
| `/apps/rawfish-ir/menu/previewColumns` | the Vf noise / Vf sharpen columns | `false` |
| `/apps/rawfish-ir/device/focusCalibrationMenu` | the focus calibration column | `false` |
| `/apps/rawfish-ir/device/probeRequested`, `probeDone` | write the profile files at the next start; when done | `false`, empty |
| `/apps/rawfish-ir/device/shadingCalRequested` | open the lens-shading assistant at the next start | `false` |

`dconf dump /apps/rawfish-ir/` shows them; an absent key takes its default,
so `dconf reset -f /apps/rawfish-ir/` returns to a fresh install's state.

## Starting from scratch

What a new user gets, to check a release: with the app closed, as root
`pkcon remove rawfish-ir-all-in-one`, then as the user

```sh
rm -rf ~/.local/share/com.rawfish/rawfish-ir ~/.config/com.rawfish/rawfish-ir ~/.cache/com.rawfish/rawfish-ir
dconf reset -f /apps/rawfish-ir/
```

(not the whole `~/.local/share/com.rawfish`: RAWfish 1.x keeps its data there
too; photos are not touched). The focus calibration and a lens-shading
calibration of your own are measurements: keep a copy first if you have
some (`tar czf ~/rawfish-ir-perso.tgz -C ~ .local/share/com.rawfish/rawfish-ir .config/com.rawfish/rawfish-ir`).
Then install the package rawfish-ir-all-in-one: the first start asks Sailjail's permissions,
uses the lens-shading calibration shipped for the phone, has no focus
calibration (it is made by hand), and copies the commented example
`override-hal.example.jsonc` into `~/.local/share/com.rawfish/rawfish-ir/device-profiles/`.
`generated-hal.json` and a starter `override-hal.json` are written only by the
probe: the Settings page's "Create the device profile files" (at the app's next
start) or `rawfish-ir-cli probe`; the app does not need them.

## Continuous integration

`.github/workflows/check-host.yml` runs `./scripts/check-host.sh` on GitHub's
Ubuntu runners at every push and pull request: both halves compiled with
-Werror by gcc and clang, the Android side against pinned NDK headers, every
host test (several under AddressSanitizer / UndefinedBehaviorSanitizer), the
controller under ThreadSanitizer and Helgrind, the lock checker, the specs
and device profiles, the translations and the QML. It needs no phone and builds no package. On a private
repository it uses the account's free monthly Actions minutes (a run takes a
minute or two); on a public one it is free. The result shows on the commit
in GitHub (a tick or a cross) and in the repository's Actions tab.
