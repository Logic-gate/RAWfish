#!/bin/sh
# ADVANCED method (the simple one is the app's "Focus cal." menu column, see
# docs/DEVICE-PROFILES.md): focus calibration sweeps, in RAW (DNG) for reliable
# sharpness judging (no JPEG
# smoothing). For each MEASURED target distance, shoot 8 frames sweeping the
# focus= setting around the theoretical value, so you can pick the sharpest and
# learn which focus= setting really matches that distance.
#
# Put a sharp, contrasty target at the measured distance, run its block, then
# look in ~/Pictures/RAWfish-ir/ and note which DNG is sharpest. The filename
# encodes the TARGET distance and the SETTING tested:
#   calib_<target>_<setting>_<timestamp>.dng
#
# Run one block at a time, moving the target to the matching measured distance
# first. RAW, focus only (exposure stays auto). 8 frames per distance.
set -e
RUN="run 0 4096x3072 raw=4096x3072"

case "${1:-help}" in
# --- Far distances: use a very contrasty far target, ideally with an even
#     farther background, to see where the lens actually focuses. ---
inf)   printf 'raw calib_inf_inf focus=inf\nraw calib_inf_10m focus=10m\nraw calib_inf_5m focus=5m\nraw calib_inf_333m focus=3.333m\nraw calib_inf_2m focus=2m\nraw calib_inf_143m focus=1.429m\nraw calib_inf_1m focus=1m\nraw calib_inf_067m focus=0.667m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
5m)    printf 'raw calib_5m_inf focus=inf\nraw calib_5m_10m focus=10m\nraw calib_5m_5m focus=5m\nraw calib_5m_286m focus=2.857m\nraw calib_5m_2m focus=2m\nraw calib_5m_143m focus=1.429m\nraw calib_5m_1m focus=1m\nraw calib_5m_067m focus=0.667m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
2m)    printf 'raw calib_2m_5m focus=5m\nraw calib_2m_333m focus=3.333m\nraw calib_2m_25m focus=2.5m\nraw calib_2m_2m focus=2m\nraw calib_2m_154m focus=1.538m\nraw calib_2m_125m focus=1.25m\nraw calib_2m_1m focus=1m\nraw calib_2m_077m focus=0.769m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
# --- Mid / near: sweep tighter around the theoretical diopter. ---
1m)    printf 'raw calib_1m_25m focus=2.5m\nraw calib_1m_167m focus=1.667m\nraw calib_1m_125m focus=1.25m\nraw calib_1m_1m focus=1m\nraw calib_1m_077m focus=0.769m\nraw calib_1m_063m focus=0.625m\nraw calib_1m_05m focus=0.5m\nraw calib_1m_04m focus=0.4m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
05m)   printf 'raw calib_05m_071m focus=0.714m\nraw calib_05m_063m focus=0.625m\nraw calib_05m_056m focus=0.556m\nraw calib_05m_05m focus=0.5m\nraw calib_05m_044m focus=0.435m\nraw calib_05m_039m focus=0.385m\nraw calib_05m_033m focus=0.333m\nraw calib_05m_029m focus=0.286m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
025m)  printf 'raw calib_025m_05m focus=0.5m\nraw calib_025m_037m focus=0.370m\nraw calib_025m_030m focus=0.303m\nraw calib_025m_025m focus=0.25m\nraw calib_025m_021m focus=0.208m\nraw calib_025m_018m focus=0.179m\nraw calib_025m_015m focus=0.154m\nraw calib_025m_013m focus=0.125m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
01m)   printf 'raw calib_01m_013m focus=0.125m\nraw calib_01m_012m focus=0.115m\nraw calib_01m_011m focus=0.108m\nraw calib_01m_01m focus=0.1m\nraw calib_01m_009m focus=0.093m\nraw calib_01m_009b focus=0.086m\nraw calib_01m_008m focus=0.080m\nraw calib_01m_007m focus=0.071m\nquit\n' | ./rawfish-ir-cli --log rawfish-ir.log $RUN ;;
*) echo "usage: $0 {inf|5m|2m|1m|05m|025m|01m}  (one measured target distance per run)"
   echo "Put the target at the measured distance, then run that block (8 RAW frames)."; exit 1 ;;
esac
