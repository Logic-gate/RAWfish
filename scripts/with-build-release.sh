#!/bin/sh
# Runs a build with a numbered Release in an RPM spec, then puts the spec
# back as it was (even on a failure or an interrupt):
#   scripts/with-build-release.sh <spec> <command> [args...]
# The number replaces the leading number of the spec's Release ("1.beta"
# becomes "424.beta"), so that every build has a file name of its own and
# the RPMs can all be kept in one folder; rpm also sees each as newer than
# the one before, so an install over an older build is an update.
# The number: the last patch applied, read from the newest commit whose
# subject starts with four digits ("0424-focus-circle-magnifier": 424);
# without one, the number of commits; outside git, 1. BUILD_NO=<n> in the
# environment overrides it. Two builds on the same patch share the number
# (the second replaces the first).
set -eu
[ $# -ge 2 ] || { echo "usage: with-build-release.sh <spec> <command> [args...]" >&2; exit 2; }
spec=$1; shift
[ -f "$spec" ] || { echo "with-build-release.sh: no spec $spec" >&2; exit 1; }
here=$(cd "$(dirname -- "$0")/.." && pwd)
no=${BUILD_NO:-}
if [ -z "$no" ] && git -C "$here" rev-parse --git-dir >/dev/null 2>&1; then
    no=$(git -C "$here" log --format=%s 2>/dev/null | sed -n 's/^\([0-9]\{4\}\)-.*/\1/p' | head -1)
    [ -n "$no" ] || no=$(git -C "$here" rev-list --count HEAD 2>/dev/null || true)
fi
no=$(echo "${no:-1}" | sed 's/^0*//'); [ -n "$no" ] || no=0
case "$no" in *[!0-9]*) echo "with-build-release.sh: BUILD_NO must be a number ('$no')" >&2; exit 2 ;; esac
backup="$spec.release-orig"
cp -p "$spec" "$backup"
restore() { [ -f "$backup" ] && mv -f "$backup" "$spec"; }
trap restore EXIT
trap 'restore; exit 130' INT TERM
sed -i "s/^\(Release:[[:space:]]*\)[0-9][0-9]*/\1$no/" "$spec"
echo "release: $(sed -n 's/^Release:[[:space:]]*//p' "$spec") ($spec)"
"$@"
