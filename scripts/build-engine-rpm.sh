#!/bin/sh
# Builds the indraw-engine RPM: the Android library with the NDK (on this PC),
# then the CLI + packaging in the Sailfish SDK build engine (sfdk, at the top
# of the repository, where rpm/indraw-engine.spec is).
#   sfdk config target=<SailfishOS-x.y.z-aarch64 or -armv7hl>   # once
#   ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-engine-rpm.sh
# The RPM lands in RPMS/ at the top of the repository.
set -eu
cd "$(dirname -- "$0")/.."
sfdk=${SFDK:-sfdk}
ver=$(sed -n 's/^#define IR_VERSION "\(.*\)"/\1/p' indraw_version.h)
spec_ver=$(sed -n 's/^Version: *//p' rpm/indraw-engine.spec)
if [ "$ver" != "$spec_ver" ]; then
    echo "indraw_version.h says $ver but rpm/indraw-engine.spec says $spec_ver" >&2
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
./scripts/build-android.sh                 # build/android/libindraw.so (prebuilt for the RPM)
"$sfdk" build-init
"$sfdk" -c no-fix-version build
echo "indraw-engine $ver:"
ls -1 RPMS/indraw-engine-*.rpm
