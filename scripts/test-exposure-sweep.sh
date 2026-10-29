#!/bin/sh
# Phone-side diagnosis of long exposures (run next to rawfish-ir-cli, camera app closed,
# phone still, dim and STEADY light: do not touch the lamps during the test).
#   sh test-exposure-sweep.sh        -> sweep.log + a summary on screen
#
# Every shot keeps exposure x ISO = 400 (1/30 s @ ISO 12000 ... 4 s @ ISO 100),
# so if the sensor really applies the exposure, measured center_mean stays
# about the SAME for all of them. Where it drops, the real exposure is capped.
# 8 s and 16 s @ ISO 100 are 2x and 4x brighter on purpose (ISO 100 is the floor).
# RAW is measured on the phone and not written ("raw -"): nothing to copy.
#
# The same sweep is run for each variant:
#   (none) | preview=pause | fps=low | control=off | all three together
set -u
log=sweep.log
: > "$log"

shots='exp=1/30 iso=12000
exp=1/8 iso=3200
exp=1/2 iso=800
exp=1 iso=400
exp=2 iso=200
exp=4 iso=100
exp=8 iso=100
exp=16 iso=100'

{
    for variant in '' 'preview=pause' 'fps=low' 'control=off' 'preview=pause fps=low control=off'; do
        echo "$shots" | while read -r shot; do
            echo "raw - focus=now $shot $variant"
        done
    done
    echo quit
} | ./rawfish-ir-cli --log "$log" run 0 4096x3072 raw=4096x3072 > /dev/null

# Summary: one line per shot (command, measured mean, time to image, preview
# frames delivered during the capture, or the error).
awk '
function field(name,   r) {
    if (match($0, "\"" name "\":[-0-9.]+")) { r = substr($0, RSTART, RLENGTH); sub(/.*:/, "", r); return r }
    return "-"
}
/> raw/ { cmd = substr($0, index($0, "> raw") + 8); next }
cmd != "" && /\{/ {
    if (index($0, "\"ok\":true"))
        printf "%-52s mean %7s  image_ms %8s  preview_frames %s\n", cmd, field("center_mean"), field("image"), field("preview_frames_during")
    else {
        e = $0; sub(/.*"error":"/, "", e); sub(/".*/, "", e)
        printf "%-52s FAILED: %s\n", cmd, e
    }
    cmd = ""
}' "$log"
