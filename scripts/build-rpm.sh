#!/bin/sh
# Builds the two packages of rpm/rawfish-ir.spec: rawfish-ir-all-in-one (the app with its
# engine) and rawfish-ir-cli-only (the engine alone). The Android library with
# the NDK (on this PC), then the CLI, the app and the packaging in the Sailfish
# SDK build engine (sfdk, at the top of the repository, where the spec is).
#   sfdk config target=<SailfishOS-x.y.z-aarch64 or -armv7hl>   # once
#   ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-rpm.sh
# The RPMs land in RPMS/ at the top of the repository, their Release numbered
# by the last patch applied (scripts/with-build-release.sh: 0.3.11-557.beta).
set -eu
cd "$(dirname -- "$0")/.."
sfdk=${SFDK:-sfdk}
ver=$(sed -n 's/^#define IR_VERSION "\(.*\)"/\1/p' rawfish-ir-version.h)
spec_ver=$(sed -n 's/^Version: *//p' rpm/rawfish-ir.spec)
if [ "$ver" != "$spec_ver" ]; then
    echo "rawfish-ir-version.h says $ver but rpm/rawfish-ir.spec says $spec_ver" >&2
    exit 1
fi
# The library's ABI follows the sfdk target: a 64-bit target (aarch64) gets
# arm64-v8a, a 32-bit one (armv7hl) armeabi-v7a; the spec installs it in
# droid-hybris' lib64 or lib accordingly.
target=$("$sfdk" config --show 2>/dev/null | sed -n 's/^target = //p')
case "$target" in
    *aarch64*) export ANDROID_ABI=arm64-v8a ;;
    *armv7hl*) export ANDROID_ABI=armeabi-v7a ;;
    *) echo "sfdk target '$target': cannot tell its architecture (expected aarch64 or armv7hl)" >&2; exit 1 ;;
esac
./scripts/build-android.sh                 # build/android/librawfish-ir.so (prebuilt for the RPM)
"$sfdk" build-init
./scripts/with-build-release.sh rpm/rawfish-ir.spec "$sfdk" -c no-fix-version build
echo "rawfish-ir $ver:"
ls -1t RPMS/rawfish-ir-all-in-one-*.rpm RPMS/rawfish-ir-cli-only-*.rpm 2>/dev/null | head -4
