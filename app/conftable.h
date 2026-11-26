/*
 * conftable.h -- every key of rawfish-ir.conf: its section, its default, the
 * values it takes and what it does (0.3.11, patch 0551; the readme 0553).
 *
 * The one place the settings are written down. The app reads it to place a
 * key in its section (confKey), to complete the file with the keys it lacks,
 * to carry the user's values into a new file (a new version, or the file of
 * before the sections) and to write rawfish-ir.conf.readme, the user's
 * reference, which nothing reads (conf_write_readme; rawfish-ir.conf.example
 * until 0.3.11-551). Plain C, also valid C++: the app includes it, and so
 * does a host test (check-host.sh), which checks that every default passes
 * its own check and that the readme reads back as the table.
 *
 * check: "list:a,b,c" one of those words ("list:,4:3" admits the empty
 * value), "range:LO..HI" a number from LO to HI (whole-number ends), "re:REGEX" a POSIX extended
 * regular expression the whole value matches, "any" anything.
 * def NULL: a key the app writes itself (state with no default): never
 * written at its default, carried to a new file when it is there.
 */
#ifndef RAWFISH_CONFTABLE_H
#define RAWFISH_CONFTABLE_H

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name, *doc; } conf_section_t;
typedef struct { const char *section, *key, *def, *check, *doc; } conf_key_t;

/* The readme's text before its first section. */
static const char *const k_conf_head =
        "rawfish-ir.conf.readme -- a reference, for you to read: every key of\n"
        "~/.config/com.rawfish/rawfish-ir/rawfish-ir.conf, what it does, the values it\n"
        "takes and, on the line \"key=value\" below its text, its DEFAULT value.\n"
        "\n"
        "RAWfish-ir writes this file from its own table at each start; it never reads\n"
        "it. Deleting or editing it changes nothing (it comes back at the next start).\n"
        "It is not a settings file and cannot be used as one: copy a line from it\n"
        "into rawfish-ir.conf, in the same section, to change a setting.\n"
        "\n"
        "The real file, rawfish-ir.conf:\n"
        "- holds every setting, in sections; the app writes it in ALPHABETICAL order\n"
        "  (the sections, and the keys in each), without comments: look here for what\n"
        "  a key means. Its values are yours; the values here are the defaults.\n"
        "- A new install writes it with every key at its default. A new version of\n"
        "  the app carries your values into a new file (keys it no longer has are left\n"
        "  out, a value no longer allowed takes its default) and keeps the earlier\n"
        "  file in ~/.config/com.rawfish/rawfish-ir/backup-<earlier version>-<date>/;\n"
        "  only our JPG's processing of each camera ([camera_N]) starts again at\n"
        "  \"default\", the new version's defaults.\n"
        "- To edit it: CLOSE the app first (it rewrites the file when a setting\n"
        "  changes), edit, reopen. A value out of its range is clamped, a value not in\n"
        "  its list takes the default: a typo never stops the app. A key must stay in\n"
        "  its own section: in another one it is not read (and is dropped at the next\n"
        "  start).\n"
        "- Back to the defaults: Settings > Apps > RAWfish-ir > \"Reset the camera\n"
        "  settings\" (the file is kept as rawfish-ir.conf.bak), or delete the file\n"
        "  with the app closed.\n"
        "\n"
        "The sections (in this file in a reading order): [menu] the camera menu's\n"
        "choices, [shooting] how a photo is taken, [viewfinder], [white_balance],\n"
        "[lens_shading], [kept] the values kept for a manual start, [advanced] tests\n"
        "and diagnostics, [internal] the app's own bookkeeping, and one [camera_N] per\n"
        "camera for our JPG's processing (see the end).\n"
        "\n"
        "Settings NOT in this file (dconf, from Settings > Apps > RAWfish-ir):\n"
        "  /apps/rawfish-ir/menu/startMode     last | simple | advanced   mode at start\n"
        "  /apps/rawfish-ir/menu/startManual   true | false               start in manual focus + exposure\n"
        "  /apps/rawfish-ir/menu/showScenes    true | false               scenes in the camera menu (no switch in Settings at present)\n"
        "  output folders and file prefix (the [output] settings of the Settings page)\n"
        "The device's own data (sensor limits, focus calibration, the default JPG\n"
        "processing of each camera) are not here either: they are in the device\n"
        "profile, ~/.local/share/com.rawfish/rawfish-ir/device-profiles/override-hal.json\n"
        "for your own corrections (see override-hal.example.jsonc beside it).\n";

/* The sections, in the readme's order (the real file sorts them).
 * "camera" stands for one [camera_N] per camera. */
static const conf_section_t k_conf_sections[] = {
    { "menu",
        "--- The camera menu's choices --------------------------------------------\n" },
    { "shooting",
        "--- How a photo is taken -------------------------------------------------\n" },
    { "viewfinder",
        "--- The viewfinder and its aids --------------------------------------------\n" },
    { "white_balance",
        "--- White balance (the deck's ± and readouts) ------------------------------\n" },
    { "lens_shading",
        "--- Lens shading (vignetting) in the DNG -------------------------------------\n" },
    { "kept",
        "--- Kept for \"Start in manual focus and exposure\" --------------------------\n" },
    { "advanced",
        "--- Tests and diagnostics ----------------------------------------------------\n" },
    { "internal",
        "--- The app's own bookkeeping: not to edit -------------------------------------\n" },
    { "camera",
        "--- Our JPG's processing: one section per camera, [camera_N] ---------------\n"
        "JPG is RAWfish-ir's JPEG: the camera's image before its own noise reduction\n"
        "and sharpening (its YUV stream), processed by RAWfish-ir, then compressed.\n"
        "The menu shows these with Settings > \"JPG processing in the camera menu\".\n"
        "Each camera has its own section, [camera_0], [camera_2], ..., written with\n"
        "its seven keys the first time the camera is used. \"default\" is the\n"
        "camera's own default: its device profile's (jpg_* keys,\n"
        "docs/DEVICE-PROFILES.md), else the app's, given below. A value is your\n"
        "choice for that camera (the menu writes it; choosing the default in the\n"
        "menu writes \"default\" again). For example, a camera whose sharpening was\n"
        "set to 1 in the menu:\n" },
    { NULL, NULL }
};

static const conf_key_t k_conf_keys[] = {
    /* [menu] */
    { "menu", "timerSec", "0", "list:0,3,10",
        "Self-timer before a shot, seconds.            values: 0, 3, 10        default 0\n" },
    { "menu", "format", "", "list:,4:3,16:9",
        "Image proportion of the JPEG (\"\" = the sensor's own, 4:3 on most phones).\n"
        "                                              values: \"\", 4:3, 16:9   default \"\"\n" },
    { "menu", "captureSize", "", "re:^([1-9][0-9]{2,4}x[1-9][0-9]{2,4})?$",
        "JPEG size, WIDTHxHEIGHT among the sizes the camera lists (\"\" = the largest\n"
        "of at most 13 megapixels: 4096x3072 on the JP2). Each camera keeps its own\n"
        "(captureSizeMap, written by the app); this is the one in force.\n"
        "                                              e.g. 4096x3072          default \"\"\n" },
    { "menu", "jpegQuality", "95", "list:20,40,60,80,90,95,100",
        "JPEG quality (JPG and HAL JPG).\n"
        "                                values: 20, 40, 60, 80, 90, 95, 100   default 95\n" },
    { "menu", "awbMode", "1", "range:1..8",
        "White-balance preset: 1 auto, 2 incandescent, 3 fluorescent, 4 warm fluorescent,\n"
        "5 daylight, 6 cloudy, 7 twilight, 8 shade (only those the camera lists).\n"
        "Back to 1 (auto) at each start of the app.\n"
        "                                              range 1..8              default 1\n" },
    /* "0" is 0x3's name until 0.3.11-551, still read (as 0x3) */
    { "menu", "bracket", "off", "list:off,0x3,0x5,0x7,1,2,3,0",
        "Bracketing: off; 0x3, 0x5, 0x7 = three, five or seven shots at the same\n"
        "exposure (x3, x5, x7); 1, 2, 3 = three shots at -N, 0, +N stops. In JPG\n"
        "they are the camera's own JPEGs; with a DNG with each JPG (Settings), the\n"
        "JPEG series, then the same series as DNGs.\n"
        "                                    values: off, 0x3, 0x5, 0x7, 1, 2, 3   default off\n" },
    { "menu", "gridOn", "0", "list:0,1",
        "Rule-of-thirds grid.                          values: 0, 1            default 0\n" },
    { "menu", "flash", "off", "list:off,auto,on,torch",
        "Flash.                                        values: off, auto, on, torch   default off\n" },
    { "menu", "sceneMode", "0", "range:0..18",
        "Scene mode (Camera2 number; 0 off). Used only when the scenes are shown.\n"
        "                                              range 0..18             default 0\n" },
    { "menu", "advanced", "0", "list:0,1",
        "Mode last used (simple 0, advanced 1); read when \"Mode at start\" is \"last\".\n"
        "A new install opens in simple mode.\n"
        "                                              values: 0, 1            default 0\n" },
    { "menu", "snapSize", "", "re:^([1-9][0-9]{2,4}x[1-9][0-9]{2,4})?$",
        "snap (in TYPE), where the session cannot hold RAWfish-ir's YUV stream: the\n"
        "camera driver's own JPEG at a small size, for utility shots (a receipt, a\n"
        "sign), noise reduction and sharpening HQ. Its size, as WIDTHxHEIGHT among\n"
        "the camera's JPEG sizes (\"\" = the largest 4:3 size of at most 5\n"
        "megapixels: 2592x1944 on the JP2). (Elsewhere snap is our light JPG.)\n"
        "                                              e.g. 3264x2448          default \"\"\n" },
    { "menu", "stackDenoise", "1", "list:1,2,4",
        "Σ stacks: 1 = the stack's own frames; 2 or 4 = that many times more frames,\n"
        "divided out (less noise, 2x or 4x the time).  values: 1, 2, 4         default 1\n" },
    /* [shooting] */
    { "shooting", "focusScanWait", "1000", "range:0..3000",
        "Continuous autofocus caught searching at the release (after a reframing):\n"
        "the photo waits for the search to end, at most this long, then is taken\n"
        "anyway. A lens already settled costs nothing. In milliseconds; 0 = never\n"
        "wait. The JP2's searches take 0.4 to 0.95 s.\n"
        "                                              values: 0 .. 3000      default 1000\n" },
    { "shooting", "hapticShutter", "40", "range:0..200",
        "Vibration at the release (the press, or the end of the timer), in\n"
        "milliseconds; 0 = none.                       values: 0 .. 200       default 40\n" },
    { "shooting", "hapticType", "20", "range:0..100",
        "Vibration at a change of TYPE, in milliseconds; 0 = none. Below about\n"
        "15 ms the JP2's vibration motor does not move at all.\n"
        "                                              values: 0 .. 100       default 20\n" },
    { "shooting", "halJpegNoise", "auto", "list:auto,default,off,fast,hq,minimal",
        "Stacks, brackets and bursts use the HAL JPEG and its settings below. On a\n"
        "phone whose session cannot hold the YUV stream (exclusive streams), JPG is\n"
        "the HAL JPEG.\n"
        "The HAL JPEG's noise reduction.               values: auto, default, off, fast, hq, minimal   default auto\n"
        "auto: hq where the session cannot hold RAWfish-ir's YUV stream (a device\n"
        "profile's \"streams\": \"exclusive\"), whose JPG is the camera's JPEG; off elsewhere.\n" },
    { "shooting", "halJpegSharpen", "hq", "list:default,off,fast,hq",
        "The HAL JPEG's sharpening.                    values: default, off, fast, hq            default hq\n" },
    { "shooting", "frameLatencyMs", "200", "range:0..5000",
        "Measured by the app after each stack (time the camera adds between\n"
        "sub-frames); used for the time estimate under the wheel. Leave it.\n"
        "                                              range 0..5000 ms        default 200\n" },
    /* [viewfinder] */
    { "viewfinder", "zebraOn", "0", "list:0,1",
        "Zebra and focus peaking as last left, restored at the start in advanced\n"
        "mode (simple mode starts without them).      values: 0, 1            default 0\n" },
    { "viewfinder", "peakingOn", "0", "list:0,1",
        "" },
    { "viewfinder", "zebraJpegMode", "luma", "list:luma,channel",
        "The zebra in JPG: luma = a pixel is striped when it comes out white (its\n"
        "luminance reaches the level); channel = when any one colour channel does\n"
        "(a blue sky or a red folder then stripe long before they look burnt). In\n"
        "RAW this key is not used: the zebra stripes what the RAW meter finds\n"
        "clipped in the sensor data (its share, on the viewfinder's brightest\n"
        "pixels; zebraTweak shifts it): full stripes where two or three colour\n"
        "channels are lost, light (white only) where one alone is -- mostly the\n"
        "green, which a RAW developer rebuilds from the two others.\n"
        "                                              values: luma, channel   default luma\n" },
    { "viewfinder", "peakTweak", "0", "list:-30,0,30",
        "A change in % applied to the device's own thresholds (override-hal.json:\n"
        "peak_edge, zebra_jpeg, zebra_raw). Negative = more peaking / zebra appears\n"
        "earlier; positive = less / later. In raw the zebra is the RAW's measured\n"
        "clipping: the tweak scales the striped share.  values: -30, 0, 30      default 0\n" },
    { "viewfinder", "zebraTweak", "0", "list:-30,0,30",
        "" },
    { "viewfinder", "viewfinderNoise", "hq", "list:default,off,fast,hq,minimal",
        "The viewfinder's noise reduction (the HAL's).  values: default, off, fast, hq, minimal   default hq\n" },
    { "viewfinder", "viewfinderSharpen", "hq", "list:default,off,fast,hq",
        "The viewfinder's sharpening (the HAL's).      values: default, off, fast, hq            default hq\n" },
    { "viewfinder", "sigmaPreviewFps", "10", "range:1..30",
        "Target frame rate of the Σ viewfinder: one frame of the stack's exposure, as\n"
        "short as this allows, the ISO raised to match (slower when the maximum ISO\n"
        "is reached). Also the viewfinder's rate for a manual speed of 1/4 s or 1/2 s,\n"
        "shown the same way.                                  range 1..30 frames/s    default 10\n" },
    { "viewfinder", "screenOnMinutes", "5", "range:0..60",
        "screenOnMinutes: the display kept on while the viewfinder runs, up to N\n"
        "minutes after the last touch; 0 = the system's own delay. Applied when\n"
        "this file is saved.                         values: 0 to 60   default 5\n" },
    /* [white_balance] */
    { "white_balance", "wbShift", "0", "range:-20..20",
        "Warmth shift from the white in force, in steps of 5 % on red/blue\n"
        "(+ warmer).                                   range -20..20           default 0\n" },
    { "white_balance", "tint", "0", "range:-100..100",
        "Tint shift, % of green (- greener, + magenta). range -100..100        default 0\n" },
    { "white_balance", "wbFixed", "", "re:^([0-9]+(\\.[0-9]+)?(,[0-9]+(\\.[0-9]+)?){3})?$",
        "A white measured on a neutral object (the 2 s press on a readout): gains\n"
        "\"R,Ge,Go,B\"; \"\" = none. At start it is kept only if \"Keep previous manual WB\"\n"
        "is tapped.\n"
        "                                              e.g. 1.8868,1,1,1.4961  default \"\"\n" },
    { "white_balance", "fineWarmthStep", "1", "range:1..4",
        "Size of one press of the warmth button, in 5 % units.   range 1..4   default 1\n" },
    { "white_balance", "fineTintStep", "5", "range:1..25",
        "Size of one press of the tint button, in % of green.    range 1..25  default 5\n" },
    /* [lens_shading] */
    { "lens_shading", "shadingPivot", "50", "range:0..100",
        "When a measured calibration exists for the phone (one is shipped for the\n"
        "Jolla Phone 2), each DNG gets a gain map composed for its own lens position\n"
        "and light; these three set how it is applied (darktable applies it).\n"
        "Pivot: 0 = only brighten the edges, 100 = only darken towards the darkest\n"
        "corner; in between, both. darktable clips values pushed above white, so a\n"
        "pivot keeps bright edges from clipping (raise the exposure in darktable).\n"
        "                                              range 0..100            default 50\n" },
    { "lens_shading", "shadingStrength", "100", "range:0..100",
        "How much of the correction is applied.        range 0..100            default 100\n" },
    { "lens_shading", "shadingMaxGain", "5", "range:1..8",
        "The largest gain allowed (more amplifies the corners' noise).\n"
        "                                              range 1..8              default 5\n" },
    /* [kept] */
    { "kept", "lastFocusD", NULL, "range:0..100",
        "Saved when the app goes to the background or closes; leave them (absent\n"
        "from a new file: the start then takes the automatic values).\n"
        "Focus distance in diopters (1/metres; 0 = infinity).\n" },
    { "kept", "lastIso", NULL, "range:1..1000000",
        "ISO and shutter (nanoseconds; beyond 500000000 a Σ stack).\n" },
    { "kept", "lastShutterNs", NULL, "range:1..100000000000",
        "" },
    /* [advanced] */
    { "advanced", "logChecks", "false", "list:false,true",
        "Checks in the log: each photo's report then carries the histogram the app\n"
        "showed (viewfinder, or RAW meter) and the photo's own, for histo-check.py.\n"
        "About 2 KB of log per photo; off for everyday use. Our JPG photos then\n"
        "carry their settings in their file names as well (the Gallery's details\n"
        "show the name, not the Exif we write):\n"
        "photo_tiles4_1-48s_iso4428_col2_moire0_sharp3_..._717.jpg. The same switch is in\n"
        "Settings > Apps > RAWfish-ir > Log > \"Detailed log\".\n"
        "                                              values: false, true     default false\n" },
    { "advanced", "jpgYuvStream", "jpeg", "re:^(jpeg|max|[0-9]{3,5}x[0-9]{3,5})$",
        "The YUV stream's size for JPG: jpeg = the photo's size, taken through the\n"
        "largest listed YUV size that is a whole multiple of it, averaged down (the\n"
        "normal case: a mid-size YUV stream makes the JP2's viewfinder lag, the full\n"
        "size does not); max = the largest, processed at that size (slow); or a\n"
        "WIDTHxHEIGHT the camera lists, which sets the photo's size to that size\n"
        "on EVERY camera (a 1280x960 stream makes 1.2 MP photos). A test key.\n"
        "                                              values: jpeg, max, WxH  default jpeg\n" },
    { "advanced", "jpgYuvFromLargestStream", "1", "list:0,1",
        "JPG, with jpgYuvStream=jpeg: where the camera lists no YUV stream that is a\n"
        "whole multiple of the photo's size (the JP2's selfie camera: its 3264x2448\n"
        "photo, and an in-between stream makes its viewfinder lag), take the camera's\n"
        "LARGEST stream and average it down by 2 or 4, when it is that many times the\n"
        "photo's size within 2 % (3280x2464 photos from its 6560x4928 stream). On the\n"
        "JP2 that is the selfie camera's 3264x2448 alone, of every size of the three\n"
        "cameras. Tried on the phone (14 Nov 2026): the selfie's viewfinder no longer\n"
        "lags. 0 = as before (the in-between stream).\n"
        "                                              values: 0, 1            default 1\n" },
    { "advanced", "viewfinderYuv", "1", "list:0,1",
        "viewfinderYuv: how the viewfinder's frames come from the engine. 1: as YUV\n"
        "(I420), turned into colour by the phone's GPU -- half the bytes through the\n"
        "pipe and no conversion on the camera's thread (0.3.11). 0: as RGB, converted\n"
        "by the engine, as up to 0.3.9: to compare the two (CPU, frames per second,\n"
        "the detailed log's \"vfpace\" line). Applied when this file is saved.\n"
        "                                              values: 0, 1            default 1\n" },
    { "advanced", "viewfinderZeroCopy", "1", "list:0,1",
        "viewfinderZeroCopy (branch zero_copy): the viewfinder's frames stay in the\n"
        "camera's buffers, drawn by the GPU as they are; only the buffers' ids go\n"
        "through the pipe (with the zebra or the peaking on, also their values per\n"
        "2x2 pixels, worked out by the engine). Falls back to viewfinderYuv by\n"
        "itself if anything fails (the log says \"# zero copy: ...\"). 0: off.\n"
        "Applied when this file is saved.            values: 0, 1      default 1\n" },
    { "advanced", "aidsEvery", "2", "range:1..4",
        "aidsEvery: with zero copy, the zebra and the peaking are worked out by the\n"
        "engine on one frame in N (the viewfinder itself keeps every frame): 1 =\n"
        "every frame (30 a second), 2 = 15, 3 = 10, 4 = 7.5 -- less CPU and battery,\n"
        "the aids lag the picture a little when the phone moves. Applied when this\n"
        "file is saved.                              values: 1 to 4    default 2\n" },
    { "advanced", "aidsGpu", "1", "list:0,1",
        "aidsGpu: with zero copy, the zebra and the peaking drawn by the GPU from\n"
        "the engine's values (1), or as an image the app builds each time (0: to\n"
        "compare; the RAW zebra is always the latter). Applied when this file is\n"
        "saved.                                      values: 0, 1      default 1\n" },
    { "advanced", "statsLog", "0", "range:0..3600",
        "statsLog: every N seconds, a \"# stats\" line in the log: the viewfinder's\n"
        "mode and fps, CPU (100% = one core, as top), RSS, peak RSS and threads of\n"
        "the app and the engine, the system's CPU, the battery (current, voltage,\n"
        "charge, charging or not; unplug the phone to compare consumption). The\n"
        "first line after an engine starts has no engine CPU yet (\"-\").\n"
        "statsLog, viewfinderZeroCopy and viewfinderYuv apply as soon as this file\n"
        "is saved (\"# conf changed: ...\" in the log).\n"
        "                                            values: 0 (off) to 3600  default 0\n" },
    { "advanced", "vfBuffers", "6", "range:4..10",
        "vfBuffers: the camera buffers the viewfinder rotates through (the app may\n"
        "hold all but two). More smooth out a late frame, each costs one frame of\n"
        "memory (1.8 MB at 1280x960). Read when an engine starts.\n"
        "                                            values: 4 to 10   default 6\n" },
    { "advanced", "hwProbe", "0", "range:0..20",
        "hwProbe (branch zero_copy, a test): the engine sends its next N viewfinder\n"
        "buffers to the app to be imported without a copy (eglHybrisCreateRemoteBuffer);\n"
        "the log says what worked, a picture that came through is saved beside it\n"
        "(hwprobe-*.png). The viewfinder is unchanged. Read when an engine starts.\n"
        "                                              values: 0 (off) to 20   default 0\n" },
    { "advanced", "hwProbeGpuUsage", "0", "list:0,1",
        "hwProbeGpuUsage: with hwProbe, the viewfinder's buffers allocated for GPU\n"
        "sampling too (AImageReader_newWithUsage).    values: 0, 1            default 0\n" },
    /* [internal] */
    { "internal", "sizeDefaultApplied", "0", "list:0,1",
        "1 once the 12.5 MP default (0.3.1) has been applied over a size stored by\n"
        "an earlier version's default.\n" },
    { "internal", "knownCameras", "", "any",
        "The camera ids and their sides (back, front), as the last session listed\n"
        "them, for the camera button when no session has opened yet (rewritten at\n"
        "each start).\n" },
    { "internal", "knownFacing", "", "any",
        "" },
    { "internal", "menuDiscovered", "0", "list:0,1",
        "The start's peek: the menu comes down a little and goes back up at the\n"
        "start, until it has been opened once (then 1). 0 shows the peek again.\n"
        "                                              values: 0, 1            default 0\n" },
    { "internal", "captureSizeMap", NULL, "any",
        "Each camera's JPEG size, as last chosen on it (JSON, written by the app).\n" },
    { "internal", "levelRoll0", NULL, "any",
        "The level's zero, recorded by holding \"level\" with the phone upright\n"
        "(degrees; empty or absent = the sensor's own zero).\n" },
    { "internal", "levelPitch0", NULL, "any",
        "" },
    /* [white_balance], written by the app (placed after the others in the example) */
    { "white_balance", "wbFixedMap", NULL, "any",
        "The whites measured per camera (JSON, written by the app).\n" },
    { "white_balance", "wbOffset", NULL, "any",
        "A measured white carried to another camera (written by the app).\n" },
    /* [camera_N]: our JPG's processing, one section per camera */
    { "camera", "jpgNrMethod", "default", "list:default,classic,tiles",
        "The merge: classic (each frame shifted whole onto the sharpest) or tiles\n"
        "(each part of a frame aligned on its own: for hand-held shots and things\n"
        "that move a little).                         values: default, classic, tiles   app's default classic\n" },
    { "camera", "jpgFrames", "default", "list:default,1,2,4,8",
        "The classic merge's frames: less grain, no texture lost, a little more time.\n"
        "                                              values: default, 1, 2, 4, 8       app's default 1\n" },
    { "camera", "jpgTileFrames", "default", "list:default,2,4,8",
        "The tile merge's frames.                      values: default, 2, 4, 8          app's default 4\n" },
    { "camera", "jpgNoiseColourFilter", "default", "list:default,0,1,2,3,4,5",
        "Colour noise filter: 0 off, 1..5 = 1, 2, 3, 5, 8 times the measured noise\n"
        "(4 and 5 deliberately too much).             values: default, 0..5             app's default 2\n" },
    { "camera", "jpgMoireFilter", "default", "list:default,0,1,2,3,4",
        "False colour on fine textures (a zipper, a weave): a median on the colour,\n"
        "0 off, 1..4 = 3x3, 5x5, 7x7, 9x9; it also removes real colour dots that\n"
        "small.                                        values: default, 0..4             app's default 0\n" },
    { "camera", "jpgLumaNoiseFilter", "default", "list:default,0,1,2,3",
        "The luminance noise filter, after the merge and before the sharpening:\n"
        "each pixel averaged with its neighbours that differ from it by less than\n"
        "1, 1.5 or 2.2 times the noise measured (1..3), so that grain goes and\n"
        "edges stay; 0 off.                            values: default, 0..3             app's default 0 (device profiles: 2)\n" },
    { "camera", "jpgSharpen", "default", "list:default,0,1,2,3,4,5",
        "RAWfish-ir's sharpening of the fine detail above the noise: 0 off, 1..5 =\n"
        "0.5, 1, 1.5, 2.5, 4 times the detail.        values: default, 0..5             app's default 3\n" },
    /* The one-for-all JPG keys of before 0.3.11-551, at the top of the file:
     * carried as they are into a new file, until the menu has given each
     * camera its own (SettingsMenu jpgEnsureKeys) and removed them. Not in
     * the example. */
    { "", "jpgNrMethod", NULL, "list:classic,tiles", "" },
    { "", "jpgFrames", NULL, "list:1,2,4,8", "" },
    { "", "jpgTileFrames", NULL, "list:2,4,8", "" },
    { "", "jpgNoiseColourFilter", NULL, "list:0,1,2,3,4,5", "" },
    { "", "jpgMoireFilter", NULL, "list:0,1,2,3,4", "" },
    { "", "jpgLumaNoiseFilter", NULL, "list:auto,0,1,2,3", "" },
    { "", "jpgSharpen", NULL, "list:0,1,2,3,4,5", "" },
    { NULL, NULL, NULL, NULL, NULL }
};

/* 1 when the value `v` passes `check` (see the top). */
static inline int conf_value_ok(const char *check, const char *v)
{
    if (!check || !v) return 0;
    if (!strcmp(check, "any")) return 1;
    if (!strncmp(check, "list:", 5)) {
        const char *p = check + 5;
        size_t n = strlen(v);
        for (;;) {
            const char *e = strchr(p, ',');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l == n && !strncmp(p, v, n)) return 1;
            if (!e) return 0;
            p = e + 1;
        }
    }
    if (!strncmp(check, "range:", 6)) {
        /* the ends as integers ("1..8": strtod would read "1." and stop on
         * the second dot), the value as any number */
        char *end;
        const char *dd = strstr(check + 6, "..");
        double lo, hi, x;
        if (!dd) return 0;
        lo = (double)strtoll(check + 6, NULL, 10);
        hi = (double)strtoll(dd + 2, NULL, 10);
        x = strtod(v, &end);
        return end != v && *end == '\0' && x >= lo && x <= hi;
    }
    if (!strncmp(check, "re:", 3)) {
        regex_t re;
        int ok;
        if (regcomp(&re, check + 3, REG_EXTENDED | REG_NOSUB) != 0) return 0;
        ok = regexec(&re, v, 0, NULL, 0) == 0;
        regfree(&re);
        return ok;
    }
    return 0;
}

/* A key of the table: in `section` ("camera" for any [camera_N], "" for the
 * top of the file), or NULL. */
static inline const conf_key_t *conf_find(const char *section, const char *key)
{
    int i;
    for (i = 0; k_conf_keys[i].key; ++i)
        if (!strcmp(k_conf_keys[i].section, section) && !strcmp(k_conf_keys[i].key, key)) return &k_conf_keys[i];
    return NULL;
}

/* Writes rawfish-ir.conf.readme (the user's reference) from the table, as
 * plain text to read (no ';' before the texts, which made it hard to read:
 * it is not a settings file): the head, then each section in its reading
 * order -- its "[name]" and its text -- and in it each key after an empty
 * line: its text, then the key at its default ("key=value": what the real
 * file holds when nothing was changed). A key with no default (written by
 * the app) is shown as "key=...   (written by the app; absent until then)".
 * The [camera_N] sections at the end, as an example ([camera_2]), then the
 * text of each of their keys. Until 0.3.11-551 this was
 * rawfish-ir.conf.example, every text line behind a ';'. Returns 0, or -1
 * on a write error. */
static inline int conf_write_readme(FILE *f)
{
    int s, i;
    fputs(k_conf_head, f);
    for (s = 0; k_conf_sections[s].name; ++s) {
        const char *name = k_conf_sections[s].name;
        const int cam = !strcmp(name, "camera");
        fputs("\n\n", f);
        if (!cam) fprintf(f, "[%s]\n", name);
        fputs(k_conf_sections[s].doc, f);
        if (cam) fputs("\n[camera_2]\n", f);
        for (i = 0; k_conf_keys[i].key; ++i) {
            const conf_key_t *k = &k_conf_keys[i];
            if (strcmp(k->section, name)) continue;
            if (cam) {
                fprintf(f, "%s=%s\n", k->key, !strcmp(k->key, "jpgSharpen") ? "1" : k->def);
                continue;
            }
            fputs("\n", f);
            fputs(k->doc, f);
            if (k->def) fprintf(f, "%s=%s\n", k->key, k->def);
            else fprintf(f, "%s=...   (written by the app; absent until then)\n", k->key);
        }
        if (cam) {
            fputs("\nThe seven keys:\n", f);
            for (i = 0; k_conf_keys[i].key; ++i)
                if (!strcmp(k_conf_keys[i].section, "camera")) {
                    fprintf(f, "\n%s\n", k_conf_keys[i].key);
                    fputs(k_conf_keys[i].doc, f);
                }
        }
    }
    return ferror(f) ? -1 : 0;
}

#endif
