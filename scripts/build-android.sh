#!/bin/sh
# Builds build/android/librawfish-ir.so with the Android NDK (API 24) for one ABI:
#   ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-android.sh            # arm64-v8a (64-bit phones)
#   ANDROID_ABI=armeabi-v7a ANDROID_NDK_HOME=... ./scripts/build-android.sh   # 32-bit phones
# build-engine-rpm.sh picks the ABI from the sfdk target (aarch64 / armv7hl).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)        # project root
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK directory}
abi=${ANDROID_ABI:-arm64-v8a}
case "$abi" in
    arm64-v8a)   triple=aarch64-linux-android24 ;;
    armeabi-v7a) triple=armv7a-linux-androideabi24 ;;
    *) echo "ANDROID_ABI must be arm64-v8a or armeabi-v7a (got $abi)" >&2; exit 1 ;;
esac
cc="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/$triple-clang"
mkdir -p "$root/build/android"
# -fvisibility=hidden: only IR_API functions are exported.
# -Wl,--no-undefined: a missing symbol fails here, not on the phone.
"$cc" -std=c11 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra \
    -shared -Wl,--no-undefined \
    "$root/android/indraw.c" "$root/android/dngwrite.c" "$root/android/exifwrite.c" -lcamera2ndk -lmediandk -lm \
    -o "$root/build/android/librawfish-ir.so"
echo "built $root/build/android/librawfish-ir.so ($abi)"
