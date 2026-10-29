#!/bin/sh
# PC-only sanity check: compiles both halves with the host gcc.
# The Android side is only syntax/type-checked against NDK headers fetched
# from AOSP (LineageOS mirror) at PINNED commits and checked against
# scripts/ndk-headers.sha256 (they were fetched from a moving branch until
# 0.1.2: October 2026 audit); the real build is build-android.sh.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
hdr=${NDK_HEADERS:-/tmp/ndkhdr}
av_rev=43a832b45a7129f3db1357c18157dcfcfa53b82e      # LineageOS android_frameworks_av, lineage-21.0, Oct 2026
native_rev=bb156c091b21aab6e5752bc7368832065c2fa0c8  # LineageOS android_frameworks_native, idem
if [ ! -f "$hdr/.pinned-$av_rev" ]; then
    rm -rf "$hdr"
    mkdir -p "$hdr/camera" "$hdr/media" "$hdr/android"
    av=https://raw.githubusercontent.com/LineageOS/android_frameworks_av/$av_rev
    for f in NdkCameraManager NdkCameraDevice NdkCameraError NdkCameraMetadata \
             NdkCameraMetadataTags NdkCameraWindowType NdkCaptureRequest NdkCameraCaptureSession; do
        curl -sfL "$av/camera/ndk/include/camera/$f.h" -o "$hdr/camera/$f.h"
    done
    for f in NdkImage NdkImageReader NdkMediaError; do
        curl -sfL "$av/media/ndk/include/media/$f.h" -o "$hdr/media/$f.h"
    done
    curl -sfL https://raw.githubusercontent.com/LineageOS/android_frameworks_native/$native_rev/libs/nativewindow/include/android/native_window.h \
        -o "$hdr/android/native_window.h"
    ( cd "$hdr" && sha256sum -c --quiet "$root/scripts/ndk-headers.sha256" ) || { echo "NDK headers: checksum mismatch (scripts/ndk-headers.sha256)" >&2; exit 1; }
    touch "$hdr/.pinned-$av_rev"
    # bionic's system properties (Exif Make/Model): a stand-in for the host.
    mkdir -p "$hdr/sys"
    printf '#pragma once\n#define PROP_VALUE_MAX 92\nint __system_property_get(const char *name, char *value);\n' > "$hdr/sys/system_properties.h"
    # Tiny stand-ins for headers we never use.
    echo '#pragma once
typedef struct _JNIEnv JNIEnv; typedef void *jobject;' > "$hdr/jni.h"
    echo '#pragma once
#include <stdint.h>' > "$hdr/android/data_space.h"
    echo '#pragma once
typedef struct AHardwareBuffer AHardwareBuffer;
enum { AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM = 2, AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM = 4 };' > "$hdr/android/hardware_buffer.h"
    echo '#pragma once
#include <stdint.h>
typedef struct ARect { int32_t left, top, right, bottom; } ARect;' > "$hdr/android/rect.h"
    echo '#pragma once
#include <stddef.h>
#define __INTRODUCED_IN(x)
#define __ANDROID_API__ 34' > "$hdr/stub_prefix.h"
fi
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only \
    -include "$hdr/stub_prefix.h" -I "$hdr" -I "$root/android" "$root/android/indraw.c"
echo "android/indraw.c: OK (syntax/types)"
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only "$root/android/dngwrite.c" -I "$root/android"
echo "android/dngwrite.c: OK"
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only "$root/android/exifwrite.c" -I "$root/android"
echo "android/exifwrite.c: OK"
# A second pass at -O2: the optimiser's flow analysis finds bugs -fsyntax-only
# misses (e.g. use of an uninitialised variable). Compile to an object we throw
# away; this only checks, it does not build the shipped binary (the NDK does).
gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -c \
    -include "$hdr/stub_prefix.h" -I "$hdr" -I "$root/android" "$root/android/indraw.c" -o /dev/null
echo "android/indraw.c: OK (-O2 flow analysis)"
gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -c "$root/android/dngwrite.c" -I "$root/android" -o /dev/null
echo "android/dngwrite.c: OK (-O2)"
# The NDK compiles with clang, which has warnings gcc lacks: check with it too if present.
if command -v clang >/dev/null 2>&1; then
    clang -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only \
        -include "$hdr/stub_prefix.h" -I "$hdr" -I "$root/android" "$root/android/indraw.c"
    echo "android/indraw.c: OK with clang"
else
    echo "android/indraw.c: clang not installed, NDK-only warnings not checked"
fi
# Same flags as sailfish/Makefile (no -D_GNU_SOURCE: the file must declare
# what it needs itself, as on the build engine), warnings as errors.
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-host "$root/sailfish/indraw.c" "$root/sailfish/controller.c" "$root/sailfish/ctl_exposure.c" "$root/sailfish/ctl_focus.c" "$root/sailfish/ctl_view.c" "$root/sailfish/ctl_shoot.c" -ldl -lpthread -lm
echo "sailfish/indraw.c + controller: OK"
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/indraw.c" -o /dev/null
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/controller.c" -o /dev/null
# The controller runs without Android or Qt: its host test (sailfish/tests).
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-ctl-test "$root/sailfish/tests/controller_test.c" "$root/sailfish/controller.c" "$root/sailfish/ctl_exposure.c" "$root/sailfish/ctl_focus.c" "$root/sailfish/ctl_view.c" "$root/sailfish/ctl_shoot.c" -lpthread -lm
/tmp/indraw-ctl-test
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/ctl_exposure.c" -o /dev/null
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-exp-test "$root/sailfish/tests/exposure_test.c" "$root/sailfish/ctl_exposure.c" -lm
/tmp/indraw-exp-test | tail -1
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/ctl_focus.c" -o /dev/null
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-foc-test "$root/sailfish/tests/focus_test.c" "$root/sailfish/ctl_focus.c" -lm
/tmp/indraw-foc-test | tail -1
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/ctl_view.c" -o /dev/null
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-view-test "$root/sailfish/tests/view_test.c" "$root/sailfish/ctl_view.c" -lm
/tmp/indraw-view-test | tail -1
gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$root/sailfish/ctl_shoot.c" -o /dev/null
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-shoot-test "$root/sailfish/tests/shoot_test.c" "$root/sailfish/ctl_shoot.c" -lm
/tmp/indraw-shoot-test | tail -1
echo "sailfish/indraw.c: OK (-O2)"

# RPM specs: in %install every line is a shell command, so a line that is
# The app is built from app/ alone (sfdk), so it carries its own copy of the
# licence for its %license: the two must stay the same file.
cmp -s "$root/LICENSE" "$root/app/LICENSE" || { echo "app/LICENSE differs from LICENSE: copy it again" >&2; exit 1; }
echo "licence: app/LICENSE = LICENSE (OK)"
# One version number for the two packages and the library (RAWfish-indRAW 0.1.0+).
v_h=$(sed -n 's/^#define IR_VERSION "\(.*\)"/\1/p' "$root/indraw_version.h")
v_cli=$(sed -n 's/^Version: *//p' "$root/rpm/rawfish-ir-cli.spec")
v_gui=$(sed -n 's/^Version: *//p' "$root/app/rpm/rawfish-ir-gui.spec")
if [ "$v_h" = "$v_cli" ] && [ "$v_h" = "$v_gui" ]; then echo "version: $v_h in indraw_version.h and both specs (OK)"
else echo "version MISMATCH: indraw_version.h $v_h, rawfish-ir-cli.spec $v_cli, rawfish-ir-gui.spec $v_gui" >&2; exit 1; fi

# only a path ("%{_datadir}/...") is a %files entry put in the wrong section
# (it then runs as a command and the build fails in %install). Twice so far.
for spec in "$root/rpm/rawfish-ir-cli.spec" "$root/app/rpm/rawfish-ir-gui.spec"; do
    bad=$(awk '/^%install/{f=1;next} /^%(files|clean|changelog|post|pre)/{f=0} f && /^%\{_/' "$spec")
    if [ -n "$bad" ]; then echo "$spec: path line(s) in %install (belong in %files):"; echo "$bad"; exit 1; fi
    echo "$(basename "$spec"): %install OK"
done

# RPM specs: a "#" after a tag on the same line is NOT a comment for rpm (it
# would read "# words" as more dependency names). Comments go on their own line.
for spec in "$root/rpm/rawfish-ir-cli.spec" "$root/app/rpm/rawfish-ir-gui.spec"; do
    bad=$(grep -nE '^[A-Za-z]+[(A-Za-z0-9)]*:.*[[:space:]]#' "$spec" || true)   # no match = fine (set -e)
    if [ -n "$bad" ]; then echo "$spec: comment after a tag (rpm would parse it):"; echo "$bad"; exit 1; fi
done
echo "specs: no trailing comments on tag lines"

# The NDK link is not reproduced here (no Android headers): at least make sure
# that if the Android sources call libm, the build script links it (a missing
# -lm fails only in the real build, 0.47.1).
if grep -qE '\b(pow|sqrt|log|log2|exp|floor|ceil|lround|fabs|sin|cos|atan2)\(' "$root"/android/*.c; then
    grep -qE -- '-lm\b' "$root/scripts/build-android.sh" || { echo "build-android.sh: Android sources use libm but the link lacks -lm"; exit 1; }
    echo "build-android.sh: links libm (OK)"
fi

# The white balance in kelvin (android/colortemp.h, pure C): the JP2's own
# matrices and five measured whites.
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-ct-test "$root/android/tests/colortemp_test.c" -lm
/tmp/indraw-ct-test | tail -1

# The measured lens-shading calibration (android/shading.h, pure C): the
# JP2's own file, parsing, interpolation, dosing.
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-shading-test "$root/android/tests/shading_test.c" -lm
(cd "$root" && /tmp/indraw-shading-test) | tail -1

# Building a calibration from flat-field DNGs (android/shading_build.h): write
# synthetic flats with the real dng_write(), read them back, assemble, round
# trip -- also exercises the DNG writer's pixels and GainMap.
gcc -std=c11 -Wall -Wextra -Werror -o /tmp/indraw-shading-build-test "$root/android/tests/shading_build_test.c" "$root/android/dngwrite.c" -lm
/tmp/indraw-shading-build-test | tail -1

# The preview's YUV -> RGB: pixel-exact with the per-pixel reference.
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -o /tmp/indraw-yuv-test "$root/android/tests/yuv_test.c"
/tmp/indraw-yuv-test | tail -1

# The Exif merge: the HAL's APP1 kept (entries, byte order, thumbnail), ours
# replacing or adding, under the sanitizers.
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -o /tmp/indraw-exif-test \
    "$root/android/tests/exif_test.c" "$root/android/exifwrite.c" -I "$root/android" -lm
/tmp/indraw-exif-test | tail -1

# The engine's longest event line (vfmeter: line[2600]) must fit the
# controller's reader line (line[4096]) with room: a line longer than the
# reader's buffer is cut in silence.
eng=$(grep -o 'char line\[[0-9]*\], \*p, \*end;' "$root/android/indraw.c" | grep -o '[0-9]*' | sort -n | tail -1)
ctl=$(grep -o 'char buf\[4096\], line\[[0-9]*\]' "$root/sailfish/controller.c" | grep -o 'line\[[0-9]*' | grep -o '[0-9]*')
if [ -n "$eng" ] && [ -n "$ctl" ] && [ "$eng" -lt "$ctl" ]; then echo "event line: engine $eng < controller $ctl (OK)"; else echo "event line: engine '$eng' vs controller '$ctl': NOT OK"; exit 1; fi

# The shipped device profiles: valid JSON, a match block, every key known to
# the CLI's override checker (sailfish/indraw.c k_ov_fields / k_ov_struct).
python3 - "$root" <<'PYEOF'
import json, glob, re, sys
root = sys.argv[1]
src = open(root + "/sailfish/indraw.c").read()
known = set(re.findall(r'\{ "([a-z_]+)",\s+\d', src))
blk = src[src.index("k_ov_struct[]"):src.index("NULL };", src.index("k_ov_struct[]"))]
known |= set(re.findall(r'"([a-z_]+)"', blk))
bad = 0
for f in sorted(glob.glob(root + "/android/data/device-profiles/*.json")):
    d = json.load(open(f))
    def keys(o):
        if isinstance(o, dict):
            for k, v in o.items():
                yield k
                yield from keys(v)
        elif isinstance(o, list):
            for v in o: yield from keys(v)
    unk = [k for k in keys(d) if not k.startswith("_") and k not in known]
    if "match" not in d or unk:
        print("device profile %s: %s" % (f.split("/")[-1], "no match block" if "match" not in d else "unknown keys %s" % unk)); bad = 1
if not bad: print("device profiles: OK (%d, valid, known keys)" % len(glob.glob(root + "/android/data/device-profiles/*.json")))
sys.exit(bad)
PYEOF

# g.lock is not reentrant: taking it twice in one function deadlocks the
# camera at the first frame (0.47.15). A straight-line scan, fast and crude:
# a lock re-taken in the other arm of an if carries "lockcheck: branch".
awk -f "$root/scripts/lockcheck.awk" "$root/android/indraw.c" || { echo "android/indraw.c: g.lock taken while held (see above)"; exit 1; }
echo "android/indraw.c: no g.lock taken twice (OK)"

# A "(64bit)" provide named outside an "%ifarch aarch64" block breaks the
# 32-bit package (armv7hl: "nothing provides ...(64bit)", 0.47.24).
for spec in "$root/rpm/rawfish-ir-cli.spec" "$root/app/rpm/rawfish-ir-gui.spec"; do
    awk '/^%ifarch aarch64/ { a = 1; next } /^%(else|endif)/ { a = 0; next }
         /^(Requires|BuildRequires|Provides)/ && /\(64bit\)/ && !a { print FILENAME ":" FNR ": " $0; bad = 1 }
         END { exit bad }' "$spec" || { echo "specs: a (64bit) dependency outside %ifarch aarch64"; exit 1; }
done
echo "specs: 64-bit dependency names only in %ifarch aarch64 (OK)"
