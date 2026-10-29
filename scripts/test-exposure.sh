#!/bin/sh
# Phone-side test of manual exposure, up to the sensor maximum (16 s on the JP2).
# Run next to indraw, camera app closed, phone still (on a table):
#   sh test-exposure.sh            -> files and indraw.log in the current directory
# Expected, for each photo: applied.exposure_ns == requested.exposure_ns,
# applied.frame_ns >= exposure, ms.image a bit above the exposure.
# The last line asks for 20 s: it must be REFUSED ("exposure outside sensor range").
set -u
printf '%s\n' \
    'jpeg m-exp_1_1000-iso_100.jpg off_noise off_edge focus=now exp=1/1000 iso=100' \
    'jpeg m-exp_1-iso_100.jpg off_noise off_edge focus=now exp=1 iso=100' \
    'jpeg m-exp_4-iso_100.jpg off_noise off_edge focus=now exp=4 iso=100' \
    'jpeg m-exp_8-iso_100.jpg off_noise off_edge focus=now exp=8 iso=100' \
    'jpeg m-exp_16-iso_100.jpg off_noise off_edge focus=now exp=16 iso=100' \
    'raw m-raw-exp_16-iso_100.raw16 focus=now exp=16 iso=100' \
    'jpeg m-auto.jpg off_noise off_edge focus=now' \
    'jpeg m-exp_20-iso_100.jpg focus=now exp=20 iso=100' \
    quit \
| ./rawfish-ir-cli --log rawfish-ir.log run 0 4096x3072 raw=4096x3072
