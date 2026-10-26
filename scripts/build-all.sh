#!/bin/sh
# Builds BOTH packages in one go -- the engine RPM (NDK library + CLI) and the
# app RPM -- and lists them. The one command a newcomer needs after the
# one-time setup in docs/BUILDING.md:
#   ANDROID_NDK_HOME=/path/to/ndk ./scripts/build-all.sh
# Add --clean to rebuild everything from scratch. Switching the sfdk target
# to another architecture needs nothing more: the engine's CLI and the app
# always build from clean (they are small), the library follows the target,
# and the binaries are checked against the target's architecture.
#
# --all builds for every installed SailfishOS target of the two
# architectures (aarch64 and armv7hl), one after the other (the build is
# in-tree: two at once would overwrite each other), then puts the sfdk
# target back as it was, even on a failure. The RPMs carry their
# architecture in their names and stay side by side in RPMS/ and app/RPMS/.
# Pick the targets with INDRAW_TARGETS="target1 target2" if more than one
# per architecture is installed.
set -eu
self="$(cd "$(dirname -- "$0")" && pwd)/$(basename -- "$0")"   # for --all's own calls
cd "$(dirname -- "$0")/.."
clean=""
all=""
for a in "$@"; do
    case "$a" in
        --clean) clean="--clean" ;;
        --all) all=1 ;;
        *) echo "usage: build-all.sh [--clean] [--all]" >&2; exit 2 ;;
    esac
done
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the Android NDK directory (see docs/BUILDING.md)}"
sfdk=${SFDK:-sfdk}
# Every sfdk build runs with 'no-fix-version': with a git tag in the history
# (a release, v0.47.69...) sfdk would otherwise replace BOTH specs' versions
# by the tag's plus a git suffix (indraw-app-0.47.69+main.2026...), the app
# would then require an engine version that no package carries. The
# versions are the specs' own (the engine's and the app's differ).
if [ -n "$all" ]; then
    previous=$("$sfdk" config --show 2>/dev/null | sed -n 's/^target = //p')
    targets=${INDRAW_TARGETS:-$("$sfdk" tools list 2>/dev/null | grep -o 'SailfishOS-[0-9.]*-\(aarch64\|armv7hl\)' | sort -u)}
    [ -n "$targets" ] || { echo "no aarch64 / armv7hl SailfishOS target found ('sfdk tools list')" >&2; exit 1; }
    restore() { [ -n "$previous" ] && "$sfdk" config target="$previous" >/dev/null 2>&1; }
    trap restore EXIT
    done_list=""
    for t in $targets; do
        echo "######## $t ########"
        "$sfdk" config target="$t"
        sh "$self" $clean
        done_list="$done_list $t"
    done
    echo
    echo "Built for:$done_list (the sfdk target is back to ${previous:-unset})."
    echo "Packages:"
    for a in aarch64 armv7hl; do
        ls -1t RPMS/indraw-engine-*."$a".rpm 2>/dev/null | head -1
        ls -1t app/RPMS/indraw-app-*."$a".rpm 2>/dev/null | head -1
    done
    exit 0
fi
if ! "$sfdk" config --show 2>/dev/null | grep -q "^target = "; then
    echo "sfdk has no target set: run  sfdk config target=<SailfishOS-x.y.z-aarch64 or -armv7hl>  first" >&2
    exit 1
fi
# The target's architecture (aarch64 or armv7hl): the binaries are checked
# against it after the build.
target=$("$sfdk" config --show 2>/dev/null | sed -n 's/^target = //p')
case "$target" in
    *aarch64*) arch=aarch64; want="ARM aarch64" ;;
    *armv7hl*) arch=armv7hl; want="32-bit LSB.*ARM" ;;
    *) echo "sfdk target '$target': cannot tell its architecture (aarch64 or armv7hl)" >&2; exit 1 ;;
esac
echo "== engine (library with the NDK, CLI + RPM with sfdk) =="
if [ -n "$clean" ]; then rm -rf build/android RPMS sailfish/indraw; fi
./scripts/build-engine-rpm.sh
echo "== app (sfdk) =="
( cd app && "$sfdk" -c no-fix-version build $clean )      # the app's spec always builds from clean

# The binaries must be of the target's architecture (a package built from a
# stale binary of the other one cannot be installed). Checked with file(1),
# on the binaries themselves; no check possible = stop, not pass.
command -v file >/dev/null || { echo "STOP: 'file' is needed to check the binaries' architecture" >&2; exit 1; }
for bin in sailfish/indraw app/indraw-app; do
    if ! file "$bin" | grep -q "$want"; then
        echo "STOP: $bin is not an $arch binary:" >&2
        file "$bin" >&2
        exit 1
    fi
done
eng=$(ls -1t RPMS/indraw-engine-*."$arch".rpm 2>/dev/null | head -1)
app=$(ls -1t app/RPMS/indraw-app-*."$arch".rpm 2>/dev/null | head -1)
echo
echo "Packages to copy to the phone and install together (as root: pkcon install-local -y <both>):"
echo "$eng"
echo "$app"
