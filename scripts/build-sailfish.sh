#!/bin/sh
# Builds sailfish/rawfish-ir-cli in the Sailfish SDK build engine (run from the sfdk workspace).
#   sfdk config target=<aarch64 target>   # once
#   ./scripts/build-sailfish.sh
set -eu
cd "$(dirname -- "$0")/.."
sfdk=${SFDK:-sfdk}
# sfdk only builds from the top of a "build tree": build-init marks this
# directory as one. Harmless if already done.
"$sfdk" build-init
"$sfdk" build-shell make -C sailfish clean all
echo "built $(pwd)/sailfish/rawfish-ir-cli"
