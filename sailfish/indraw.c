/*
 * indraw.c -- Sailfish side of indraw (glibc program, built with the Sailfish SDK).
 *
 * Loads the Android-side library librawfish-ir.so through libhybris and drives it:
 * camera listing and capabilities, the device profile files, and a session
 * ("run" / "stream") that keeps the camera open and executes one command per
 * line of stdin (photos, stacks, live viewfinder settings). The complete
 * reference -- every command, option, range and exit code, with examples -- is
 * docs/ENGINE.md; `indraw` with no argument prints a summary.
 *
 * Every result is ONE line of JSON on stdout; diagnostics go to stderr.
 * --log FILE appends a timestamped copy of the command line, every stdin
 * command ("> ...") and every JSON result to FILE, ready to be shared.
 */

#define _DEFAULT_SOURCE  /* getline(), strtok_r() */

#include <dlfcn.h>   /* dlopen/dlsym: load libhybris            */
#include <stdint.h>  /* int64_t exposure times                  */
#include <stdio.h>   /* printf, getline, fopen                  */
#include <dirent.h>  /* the shipped device profiles directory   */
#include <ctype.h>   /* tolower: matching model names           */
#include <stdlib.h>  /* atoi, free, getenv                      */
#include <string.h>  /* strcmp, strncmp, strtok_r               */
#include <errno.h>   /* errno for mkdir_p                       */
#include <time.h>    /* time, localtime_r, strftime            */
#include <sys/stat.h> /* mkdir, for the device-profiles directory */
#include <sys/types.h>
#include <unistd.h>   /* access, unlink                          */
#include <signal.h>   /* sigaction: SIGUSR1 aborts a stack       */
#include <poll.h>     /* poll: notice the app's stdin closing    */
#include <math.h>     /* fabsf: comparing calibration distances  */
#include <pthread.h>  /* the stdin hang-up watcher thread         */
#include "../indraw_version.h"
#include "controller.h"  /* IR_VERSION, shared with the library  */

#define OUT_SIZE 32768 /* room for any JSON report (caps lists per-size timings) */

/* Exit codes: which layer broke. */
enum {
    EXIT_USAGE = 2,       /* bad command line                          */
    EXIT_HYBRIS = 10,     /* libhybris or android_dlopen unavailable   */
    EXIT_BRIDGE = 12,     /* librawfish-ir.so not loaded / symbol missing */
    EXIT_DROIDMEDIA = 14, /* binder thread pool could not be started   */
    EXIT_FAILED = 20,     /* the bridge reported an error              */
};

/* Signatures of what we look up by name. */
typedef void *(*android_dlopen_fn)(const char *name, int flags);
typedef void *(*android_dlsym_fn)(void *handle, const char *name);
typedef const char *(*indraw_version_fn)(void);
typedef int (*indraw_caps_fn)(const char *id, char *out, size_t n);
typedef int (*indraw_cameras_fn)(char *out, size_t n);
typedef int (*indraw_open_fn)(const char *id, int jw, int jh, int pw, int ph, int rw, int rh,
                          char *out, size_t n);
typedef int (*indraw_raw_fn)(const char *path, const char *datetime, int focus_mode,
                         float focus_diopters, int ae_precapture, int64_t exposure_ns,
                         int iso, int variant, int timeout_ms, char *out, size_t n);
typedef int (*indraw_stack_fn)(const char *path, const char *datetime, int64_t film_ns,
                           int64_t sub_ns, int stk_denoise, int iso, char *out, size_t n);
typedef int (*indraw_noiseprofile_fn)(const char *path, int64_t sub_ns, char *out, size_t n);
typedef int (*indraw_set_override_fn)(int64_t observed_shutter_max_ns, int64_t min_frame_ns,
                                      char *out, size_t n);
typedef int (*indraw_set_aids_override_fn)(int64_t peak_edge, int64_t zebra_jpeg, int64_t zebra_raw,
                                           int64_t peak_width, char *out, size_t n);
typedef int (*indraw_set_af_wait_fn)(int ms, char *out, size_t n);
typedef void (*indraw_request_abort_fn)(void);
typedef float (*indraw_lens_position_fn)(void);
typedef int (*indraw_burst_fn)(const char *const *paths, int n, int quality, int nr, int edge,
                               int64_t exposure_ns, int iso, int timeout_ms, char *out, size_t n_out);
typedef int (*indraw_flash_fn)(int mode, char *out, size_t n);
typedef int (*indraw_ae_lock_fn)(int on, char *out, size_t n);
typedef int (*indraw_loupe_fn)(float factor, float cx, float cy, char *out, size_t n);
typedef int (*indraw_vendor_set_fn)(uint32_t tag, int type, double value, int on, char *out, size_t n);
typedef int (*indraw_vendor_report_fn)(char *out, size_t n);
typedef int (*indraw_shading_load_fn)(const char *user_dir, const char *sys_dir, int mode, char *out, size_t n);
typedef int (*indraw_shading_params_fn)(double balance, double strength, double max_gain, char *out, size_t n);
typedef int (*indraw_shading_calibrate_fn)(const char *dir, int full, double cct, const float *lens, int nlens, int shots, char *out, size_t n);
static indraw_shading_params_fn g_shading_params;   /* for the "shading" command */
static void *g_bridge_for_vendor;            /* the loaded library, for apply_vendor_requests */
typedef int (*indraw_orient_fn)(int degrees, char *out, size_t n);
typedef int (*indraw_view_window_fn)(double x, double y, double w, double h, char *out, size_t n);
typedef int (*indraw_wb_pick_fn)(char *out, size_t n);
typedef int (*indraw_wb_fixed_fn)(const float *gains, char *out, size_t n);
typedef int (*indraw_emit_fn)(const char *line);
typedef int (*indraw_set_event_fd_fn)(int fd);
typedef int (*indraw_set_state_events_fn)(int on);
typedef int (*indraw_tap_fn)(float nx, float ny, int do_ae, int do_af, char *out, size_t n);
typedef int (*indraw_wb_fn)(int mode, char *out, size_t n);
typedef int (*indraw_scene_fn)(int mode, char *out, size_t n);
typedef int (*indraw_wb_manual_fn)(int shift, int tint, char *out, size_t n);
typedef int (*indraw_focus_fn)(int mode, float diopters, char *out, size_t n);
typedef int (*indraw_focus_opt_fn)(int mode, float diopters, int wait, char *out, size_t n);
typedef int (*indraw_preview_frame_fn)(const char *path, char *out, size_t n);
typedef int (*indraw_preview_settings_fn)(int nr, int edge, char *out, size_t n);
typedef int (*indraw_stream_start_fn)(int fd, char *out, size_t n);
typedef int (*indraw_preview_exposure_fn)(int64_t exposure_ns, int iso, char *out, size_t n);
typedef int (*indraw_ev_fn)(int steps, char *out, size_t n);
typedef int (*indraw_meter_fn)(int on, int interval_ms, char *out, size_t n);
typedef int (*indraw_zoom_fn)(float factor, char *out, size_t n);
typedef int (*indraw_set_focus_cal_fn)(const float *real_d, const float *hal_d, int n,
                                       char *out, size_t n2);

/* Capture variants, same values as IR_V_* on the Android side. */
#define V_PAUSE 1        /* preview=pause : stop the preview during the capture */
#define V_FPS_LOW 2      /* fps=low       : lowest AE target FPS range          */
#define V_CONTROL_OFF 4  /* control=off   : CONTROL_MODE OFF                    */
typedef int (*indraw_jpeg_fn)(const char *path, int quality, int nr, int edge,
                          int focus_mode, float focus_diopters, int ae_precapture,
                          int64_t exposure_ns, int iso, int variant, int timeout_ms,
                          char *out, size_t n);
typedef int (*indraw_close_fn)(char *out, size_t n);

static android_dlopen_fn a_dlopen; /* libhybris' dlopen for Android libraries */
static android_dlsym_fn a_dlsym;   /* libhybris' dlsym                         */
static FILE *g_log;                /* --log file, or NULL                       */

/* Appends "HH:MM:SS.mmm <prefix><text>" to the log (no-op without --log).
 * Milliseconds since 0.47.70: a command and its reply in the same second
 * said nothing about a 0.5 s capture against a 2 s one. */
static void log_line(const char *prefix, const char *text)
{
    char stamp[16];
    struct timespec ts;
    struct tm tm;
    if (!g_log) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    fprintf(g_log, "%s.%03ld %s%s\n", stamp, ts.tv_nsec / 1000000L, prefix, text);
    fflush(g_log);                              /* survive a crash          */
}

/* Prints one JSON result on stdout and copies it to the log. */
static int g_streaming = 0;   /* 1 in stream mode: stdout carries binary frames */

static void emit(const char *json)
{
    if (!g_streaming) {                         /* normal: JSON on stdout   */
        puts(json);
        fflush(stdout);                         /* readers see it at once   */
    }
    log_line("", json);                         /* always to the log        */
}

/* Resolves the device-profiles directory, honouring the Sailjail sandbox.
 * Order: INDRAW_PROFILE_DIR (for tests), else $XDG_DATA_HOME/com.rawfish/rawfish-ir,
 * else $HOME/.local/share/com.rawfish/rawfish-ir. Sailjail whitelists exactly
 * $HOME/.local/share/<OrganizationName>/<ApplicationName>, i.e.
 * com.rawfish/rawfish-ir, and sets XDG_DATA_HOME inside the sandbox, so reading it
 * (as Qt's QStandardPaths does) is correct both inside and outside the jail.
 * Writes the path into `buf`; returns 0 on success, -1 if it does not fit. */
static int profile_dir(char *buf, size_t n)
{
    const char *over = getenv("INDRAW_PROFILE_DIR");  /* test/override          */
    const char *xdg = getenv("XDG_DATA_HOME");        /* set by Sailjail        */
    const char *home = getenv("HOME");
    int len;
    if (over && over[0])
        len = snprintf(buf, n, "%s", over);
    else if (xdg && xdg[0])
        len = snprintf(buf, n, "%s/com.rawfish/rawfish-ir/device-profiles", xdg);
    else if (home && home[0])
        len = snprintf(buf, n, "%s/.local/share/com.rawfish/rawfish-ir/device-profiles", home);
    else
        return -1;                                    /* no HOME: give up       */
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* Creates `path` and the parents in it that are missing (like mkdir -p). The
 * profile dir is several levels deep and Sailjail only pre-creates the top two,
 * so the rest must be made. Returns 0 on success or an errno value. */
static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, len = strlen(path);
    if (len >= sizeof tmp) return ENAMETOOLONG;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (i = 1; i < len; ++i)                         /* make each component     */
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return errno;
            tmp[i] = '/';
        }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return errno; /* the leaf      */
    return 0;
}

/* Reads one integer sensor-correction field for camera `id` from
 * override-hal.json, if that file exists. This is a deliberately tiny reader,
 * not a JSON parser: override files are small and hand-written, and the bridge
 * stays JSON-free. It looks for a per-camera object {"camera_id":"<id>", ...}
 * and, within the text that follows it, the first "<field>": <integer>.
 * Returns the value, or `fallback` if the file, the camera or the field is
 * absent. Corrections are atomic (one named field), per the device-profiles
 * design. */
static char *ov_source(int which);
static char *ov_cam(char *text, const char *id, char **end);
static char *find_key(char *from, const char *to, const char *kq);
static long long read_override_i64(const char *id, const char *field, long long fallback)
{
    int which;
    char fkey[80];
    snprintf(fkey, sizeof fkey, "\"%s\"", field);
    for (which = 0; which < 2; ++which) {           /* the user's, then the shipped */
        char *text = ov_source(which), *end = NULL, *cam, *key;
        int found = 0;
        long long v = fallback;
        if (!text) continue;
        cam = ov_cam(text, id, &end);
        if (cam && (key = find_key(cam, end, fkey)) && (key = strchr(key, ':'))) {
            char *e;
            v = strtoll(key + 1, &e, 10);
            found = e != key + 1;
        }
        free(text);
        if (found) return v;
    }
    return fallback;
}

/* Reads the focus_calibration array for camera `id` from override-hal.json into
 * real_d[] / hal_d[] (real focus diopters ascending, and the HAL diopters to
 * send). An entry is {"real_m": <metres> | null, "hal_diopters": <d>}; real_m
 * null or <=0 means infinity (real diopter 0). Tiny hand-written-JSON scanner,
 * like read_override_i64, not a full parser. Returns the point count (0 if none
 * or no file), sorted ascending by real diopter. */
/* Reads the focus_calibration points of camera `id` from one JSON file
 * (override-hal.json, or the assistant's focus-calibration.json: same
 * shape). Returns the number of points, sorted by real diopter. */
static int read_focus_calibration_file(const char *file, const char *id, float *real_d, float *hal_d, int max)
{
    char *buf, *cam, *arr, *p2;
    long sz;
    int n = 0, i, j;
    FILE *f;
    f = fopen(file, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20) { fclose(f); return 0; }
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    buf[sz] = '\0'; fclose(f);
    /* Find this camera, then its "focus_calibration" array within its section. */
    cam = buf;
    while ((cam = strstr(cam, "\"camera_id\"")) != NULL) {
        char *q = strchr(cam + 11, '"'), *q2 = q ? strchr(q + 1, '"') : NULL;
        if (q && q2) { size_t l = (size_t)(q2 - q - 1);
            if (l == strlen(id) && strncmp(q + 1, id, l) == 0) break; }
        cam = cam + 11;
    }
    if (!cam) { free(buf); return 0; }
    {   char *next = strstr(cam + 11, "\"camera_id\"");
        arr = strstr(cam, "\"focus_calibration\"");
        if (!arr || (next && arr > next)) { free(buf); return 0; }
    }
    /* Walk the {...} objects in the array, reading real_m and hal_diopters. */
    p2 = strchr(arr, '[');
    while (p2 && n < max) {
        char *obj = strchr(p2, '{');
        char *end = obj ? strchr(obj, '}') : NULL;
        char *rk, *hk;
        if (!obj || !end) break;
        rk = strstr(obj, "\"real_m\"");
        hk = strstr(obj, "\"hal_diopters\"");
        if (hk && hk < end) {
            float real = 0.0f, hal;
            char *c = strchr(hk, ':');
            hal = c ? (float)atof(c + 1) : 0.0f;
            if (rk && rk < end) {                  /* real_m: null or a number     */
                char *rc = strchr(rk, ':');
                while (rc && (*rc == ':' || *rc == ' ')) ++rc;
                if (rc && strncmp(rc, "null", 4) != 0) {
                    float m = (float)atof(rc);
                    real = (m > 0.0f) ? 1.0f / m : 0.0f;
                }
            }
            real_d[n] = real; hal_d[n] = hal; ++n;
        }
        p2 = end + 1;
        if (strchr(p2, '{') == NULL || (strchr(arr, ']') && p2 > strchr(arr, ']'))) break;
    }
    free(buf);
    /* Sort ascending by real diopter (insertion sort, tiny n). */
    for (i = 1; i < n; ++i)
        for (j = i; j > 0 && real_d[j] < real_d[j - 1]; --j) {
            float tr = real_d[j]; real_d[j] = real_d[j - 1]; real_d[j - 1] = tr;
            { float th = hal_d[j]; hal_d[j] = hal_d[j - 1]; hal_d[j - 1] = th; }
        }
    return n;
}

/* Formats a timespec as an EXIF datetime string "YYYY:MM:DD HH:MM:SS". */
static void exif_from_ts(const struct timespec *ts, char *buf, size_t n)
{
    time_t t = ts->tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y:%m:%d %H:%M:%S", &tm);
}

/* True if `name` is a safe filename prefix: non-empty and only letters, digits,
 * '-' and '_'. This refuses '/', spaces and shell/path metacharacters, so a
 * prefix can never escape ~/Pictures/RAWfish-ir/ or make an odd filename. */
static int valid_name(const char *name)
{
    const char *p;
    if (!name || !name[0]) return 0;
    for (p = name; *p; ++p) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* --- Checking override-hal.json ---------------------------------------
 * The file is hand-edited, so it is checked before use and every finding is
 * written to the log (and stderr): a key the engine does not know (a typo
 * such as "peak_egde" was ignored in silence before), a value outside its
 * range (ignored, the default stays), unbalanced braces or quotes (the file
 * is then not trusted at all). Keys starting with "_" are comments or
 * references (the starter's "_hal_..." values) and are never read. */
typedef struct { const char *key; long long lo, hi; } ov_field_t;
static const ov_field_t k_ov_fields[] = {
    { "observed_shutter_max_ns", 1000000LL, 60000000000LL },   /* 1 ms .. 60 s  */
    { "min_frame_ns",            1000000LL, 1000000000LL },    /* 1 ms .. 1 s   */
    { "peak_edge",               1, 255 },
    { "peak_width",              1, 6 },
    { "zebra_jpeg",              1, 255 },
    { "zebra_raw",               1, 1000 },
    { "capture_delay_ms",        1, 3000 },                     /* a pause before each capture */
    { NULL, 0, 0 }
};
/* Keys allowed anywhere in a camera block besides the fields above. */
static const char *const k_ov_struct[] = { "camera_id", "overrides", "focus_calibration",
                                           "real_m", "hal_diopters", "cameras",
                                           "vendor_request", "tag", "type", "value", "on",
                                           "shading", "still_template", "session_request",
                                           "raw_stream", "jpeg_max",
                                           /* a shipped device profile's own keys */
                                           "match", "make", "models", "raw", "notch", "shading_file",
                                           "name", NULL };

static void ov_log(const char *msg)
{
    log_line("# ", msg);
    fprintf(stderr, "rawfish-ir-cli: %s\n", msg);
}

/* Two sources of device corrections, read in this order for every key:
 *   0. the user's override-hal.json (device-profiles/ in the app's data),
 *      used unless its structure is broken (check_override);
 *   1. the device profile shipped with indRAW for this phone
 *      (/usr/share/rawfish-ir/device-profiles/, one JSON file per camera
 *      module), matched by make and model,
 *      or by the camera's RAW size when no model matches.
 * A key the user writes replaces the shipped one; a key the user does not
 * write comes from the shipped profile; neither: the built-in default. */
static char g_ov_shipped[1100];      /* the matched shipped profile ("" none) */
static int g_ov_user_ok = 1;         /* the user's file is usable              */
static char *ov_slurp(const char *file)
{
    char *buf;
    long sz;
    FILE *f;
    if (!file || !file[0] || !(f = fopen(file, "r"))) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 20 || !(buf = malloc((size_t)sz + 1))) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}
/* The text of source `which` (0 user, 1 shipped), NULL if absent or not to
 * be used. To free. */
static char *ov_source(int which)
{
    char dir[1024], file[1100];
    if (which == 1) return ov_slurp(g_ov_shipped);
    if (!g_ov_user_ok || profile_dir(dir, sizeof dir) != 0) return NULL;
    snprintf(file, sizeof file, "%s/override-hal.json", dir);
    return ov_slurp(file);
}
/* The block of camera `id` in a profile text: from its "camera_id" to the
 * next one (or the end). NULL if the camera is not listed. */
static char *ov_cam(char *text, const char *id, char **end)
{
    char *cam = text;
    while (cam && (cam = strstr(cam, "\"camera_id\"")) != NULL) {
        char *q = strchr(cam + 11, '"'), *q2 = q ? strchr(q + 1, '"') : NULL;
        if (q && q2 && (size_t)(q2 - q - 1) == strlen(id) && !strncmp(q + 1, id, strlen(id))) break;
        cam += 11;
    }
    if (cam && end) *end = strstr(cam + 11, "\"camera_id\"");
    return cam;
}
/* A string value "key": "..." in camera `id`'s block, first source that has
 * it; 1 if found. */
static int ov_string(const char *id, const char *key, char *val, size_t n)
{
    int which, found = 0;
    char kq[80];
    snprintf(kq, sizeof kq, "\"%s\"", key);
    for (which = 0; which < 2 && !found; ++which) {
        char *text = ov_source(which), *end = NULL, *cam, *p, *q;
        if (!text) continue;
        cam = ov_cam(text, id, &end);
        if (cam && (p = find_key(cam, end, kq)) && (p = strchr(p + strlen(kq), ':'))
            && (p = strchr(p, '"')) && (q = strchr(p + 1, '"')) && (!end || q < end)) {
            snprintf(val, n, "%.*s", (int)(q - p - 1), p + 1);
            found = 1;
        }
        free(text);
    }
    return found;
}

static int parse_size(const char *s, int *w, int *h);   /* defined with the commands below */

/* "key": true|false in camera `id`'s block (the user's file, then the
 * shipped profile); 1 found. */
static int ov_bool(const char *id, const char *key, int *val)
{
    int which, found = 0;
    char kq[80];
    snprintf(kq, sizeof kq, "\"%s\"", key);
    for (which = 0; which < 2 && !found; ++which) {
        char *text = ov_source(which), *end = NULL, *cam, *p;
        if (!text) continue;
        cam = ov_cam(text, id, &end);
        if (cam && (p = find_key(cam, end, kq)) && (p = strchr(p + strlen(kq), ':'))) {
            for (++p; *p == ' ' || *p == '\t'; ++p) {}
            if (!strncmp(p, "true", 4)) { *val = 1; found = 1; }
            else if (!strncmp(p, "false", 5)) { *val = 0; found = 1; }
        }
        free(text);
    }
    return found;
}

/* The device profile's limits on the streams, applied BEFORE open:
 * "raw_stream": false (no RAW stream) and "jpeg_max": "WxH" (the largest
 * JPEG asked for). For a camera driver that dies with the full combination
 * (the Xperia 10 III). Logged, so a limited phone is never silent. */
static void apply_stream_limits(void *bridge, const char *id)
{
    typedef int (*lim_fn)(int, int, int);
    lim_fn lf = (lim_fn)a_dlsym(bridge, "indraw_set_stream_limits");
    char v[32], msg[200];
    int raw_on = 1, jw = 0, jh = 0;
    if (!lf) return;
    ov_bool(id, "raw_stream", &raw_on);
    if (ov_string(id, "jpeg_max", v, sizeof v) && !parse_size(v, &jw, &jh)) {
        snprintf(msg, sizeof msg, "jpeg_max: \"%s\" is not WxH -- ignored", v);
        ov_log(msg); jw = jh = 0;
    }
    if (jw < 0) jw = jh = 0;                           /* "max" means no limit */
    if (raw_on && jw == 0) return;
    lf(!raw_on, jw, jh);
    snprintf(msg, sizeof msg, "streams limited by the device profile for camera %s:%s%s -- a stopgap: please report this phone (docs/DEVICE-PROFILES.md)",
             id, raw_on ? "" : " no RAW stream", jw > 0 ? " JPEG at most" : "");
    if (jw > 0) snprintf(msg + strlen(msg), sizeof msg - strlen(msg), " %dx%d", jw, jh);
    log_line("# ", msg);
}

/* The user's override file (NULL if absent or too big): ov_slurp on its path. */
static char *ov_read_file(void)
{
    char dir[1024], file[1100];
    if (profile_dir(dir, sizeof dir) != 0) return NULL;
    snprintf(file, sizeof file, "%s/override-hal.json", dir);
    return ov_slurp(file);
}

/* "key" (quoted) in [from, to), skipping an escaped quote (a key named
 * inside a "_comment" string). NULL if absent. */
static char *find_key(char *from, const char *to, const char *kq)
{
    size_t n = strlen(kq);
    char *p = from;
    while ((p = strstr(p, kq)) != NULL && (!to || p < to)) {
        if (p == from || p[-1] != '\\') return p;
        p += n;
    }
    return NULL;
}

/* Case-insensitive equality of two strings. */
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
/* A quoted string value after "key" within [from, to); 1 if found. */
static int str_in(const char *from, const char *to, const char *key, char *val, size_t n)
{
    char kq[64];
    const char *p, *q;
    snprintf(kq, sizeof kq, "\"%s\"", key);
    if (!(p = find_key((char *)from, to, kq)) || !(p = strchr(p + strlen(kq), ':')) ||
        !(p = strchr(p, '"')) || !(q = strchr(p + 1, '"')) || (to && q >= to)) return 0;
    snprintf(val, n, "%.*s", (int)(q - p - 1), p + 1);
    return 1;
}
/* Finds the device profile shipped for this phone: the files in
 * /usr/share/rawfish-ir/device-profiles/, each with a "match" block
 *   "match": { "make": "Sony", "models": ["Xperia XA2", "H4113", ...],
 *              "raw": [5984, 3392] }
 * A model listed (any case) with the same make wins; else, with the same
 * make, a profile whose "raw" size is this camera's (one camera module, two
 * phones with different model names). Sets g_ov_shipped, and the screen's
 * notch ("notch": true/false at the top level) for the interface. */
static void find_shipped_profile(void *bridge, const char *id)
{
    static const char *const dirpath = "/usr/share/rawfish-ir/device-profiles";
    typedef int (*info_fn)(char *, size_t);
    typedef int (*caps_fn)(const char *, char *, size_t);
    typedef int (*notch_fn)(int);
    info_fn info = (info_fn)a_dlsym(bridge, "indraw_device_info");
    caps_fn caps = (caps_fn)a_dlsym(bridge, "indraw_caps");
    notch_fn setnotch = (notch_fn)a_dlsym(bridge, "indraw_set_screen_notch");
    char buf[4096], make[96] = "", model[96] = "", msg[1600];
    int raw_w = 0, raw_h = 0, pass;
    DIR *d;
    struct dirent *de;
    g_ov_shipped[0] = '\0';
    if (!info || info(buf, sizeof buf) != 0) return;
    str_in(buf, NULL, "make", make, sizeof make);
    str_in(buf, NULL, "model", model, sizeof model);
    for (pass = 0; pass < 2 && !g_ov_shipped[0]; ++pass) {
        if (pass == 1) {                     /* by the camera's RAW size, only if needed */
            const char *r;
            if (!caps || caps(id, buf, sizeof buf) != 0 || !(r = strstr(buf, "\"raw16\":[["))) break;
            if (sscanf(r + 10, "%d,%d", &raw_w, &raw_h) != 2) break;
        }
        if (!(d = opendir(dirpath))) return;
        while ((de = readdir(d)) != NULL && !g_ov_shipped[0]) {
            char file[1100], pmake[96] = "", *text, *m, *mend, *list, *lend, *q;
            size_t l = strlen(de->d_name);
            if (l < 6 || strcmp(de->d_name + l - 5, ".json")) continue;
            snprintf(file, sizeof file, "%s/%s", dirpath, de->d_name);
            if (!(text = ov_slurp(file))) continue;
            m = strstr(text, "\"match\"");
            mend = m ? strchr(m, '}') : NULL;
            if (m && mend && str_in(m, mend, "make", pmake, sizeof pmake) && ieq(pmake, make)) {
                if (pass == 0 && (list = strstr(m, "\"models\"")) && list < mend &&
                    (list = strchr(list, '[')) && (lend = strchr(list, ']')) && lend < mend) {
                    for (q = list; (q = strchr(q, '"')) && q < lend; ) {
                        char *q2 = strchr(q + 1, '"'), one[96];
                        if (!q2 || q2 > lend) break;
                        snprintf(one, sizeof one, "%.*s", (int)(q2 - q - 1), q + 1);
                        if (ieq(one, model)) {
                            snprintf(g_ov_shipped, sizeof g_ov_shipped, "%s", file);
                            snprintf(msg, sizeof msg, "%s (model %s %s)", file, make, model);
                            break;
                        }
                        q = q2 + 1;
                    }
                }
                if (pass == 1) {
                    char *r = strstr(m, "\"raw\"");
                    int w = 0, h = 0;
                    if (r && r < mend && (r = strchr(r, '[')) && sscanf(r + 1, "%d , %d", &w, &h) == 2 &&
                        w == raw_w && h == raw_h) {
                        snprintf(g_ov_shipped, sizeof g_ov_shipped, "%s", file);
                        snprintf(msg, sizeof msg, "%s (camera %s: RAW %dx%d, make %s; model %s not listed)",
                                 file, id, raw_w, raw_h, make, model);
                    }
                }
            }
            free(text);
        }
        closedir(d);
    }
    if (g_ov_shipped[0]) {
        char *text = ov_slurp(g_ov_shipped), *n;
        log_line("# device profile: ", msg);
        if (text && setnotch && (n = strstr(text, "\"notch\"")) && (n = strchr(n, ':'))) {
            while (*++n == ' ') {}
            setnotch(!strncmp(n, "true", 4) ? 1 : !strncmp(n, "false", 5) ? 0 : -1);
        }
        free(text);
    } else {
        snprintf(msg, sizeof msg, "none for %s %s (built-in defaults; your override-hal.json if any)", make, model);
        log_line("# device profile: ", msg);
    }
}

/* Checks the file once per open; returns 0 when it must not be used (broken
 * structure), 1 otherwise (findings, if any, are logged). */
static int check_override(const char *id)
{
    char *buf = ov_read_file(), *p, msg[300];
    int depth = 0, in_str = 0, problems = 0, found_cam = 0;
    if (!buf) return 1;                             /* no file: defaults, fine     */
    for (p = buf; *p; ++p) {                         /* structure: braces & quotes  */
        if (in_str) { if (*p == '\\' && p[1]) ++p; else if (*p == '"') in_str = 0; continue; }
        if (*p == '"') in_str = 1;
        else if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') { if (--depth < 0) break; }
    }
    if (in_str || depth != 0) {
        ov_log("override-hal.json: unbalanced braces or quotes -- the whole file is IGNORED, built-in defaults used");
        free(buf);
        return 0;
    }
    /* Every key ("name" followed by ':') must be known, or start with '_'. */
    for (p = buf; (p = strchr(p, '"')) != NULL; ) {
        char *q = strchr(p + 1, '"'), *c;
        size_t l;
        int known = 0, i;
        if (!q) break;
        for (c = q + 1; *c == ' ' || *c == '\t' || *c == '\r' || *c == '\n'; ++c) {}
        l = (size_t)(q - p - 1);
        if (*c == ':' && l > 0 && p[1] != '_') {
            for (i = 0; k_ov_fields[i].key; ++i)
                if (strlen(k_ov_fields[i].key) == l && !strncmp(p + 1, k_ov_fields[i].key, l)) known = 1;
            for (i = 0; k_ov_struct[i]; ++i)
                if (strlen(k_ov_struct[i]) == l && !strncmp(p + 1, k_ov_struct[i], l)) known = 1;
            if (!known) {
                snprintf(msg, sizeof msg, "override-hal.json: unknown key \"%.*s\" ignored (typo? see docs/DEVICE-PROFILES.md)", (int)l, p + 1);
                ov_log(msg);
                problems++;
            }
            if (l == 9 && !strncmp(p + 1, "camera_id", 9)) {      /* our camera listed? */
                char *v1 = strchr(c, '"'), *v2 = v1 ? strchr(v1 + 1, '"') : NULL;
                if (v1 && v2 && (size_t)(v2 - v1 - 1) == strlen(id) && !strncmp(v1 + 1, id, strlen(id))) found_cam = 1;
            }
        }
        p = q + 1;                                    /* next string (key or value)  */
    }
    if (!found_cam) {
        snprintf(msg, sizeof msg, "override-hal.json: no entry for camera %s -- built-in defaults for it", id);
        ov_log(msg);
    } else if (!problems) {
        snprintf(msg, sizeof msg, "override-hal.json: checked, camera %s found, no unknown key", id);
        log_line("# ", msg);
    }
    free(buf);
    return 1;
}

/* The device profile's "vendor_request" list for camera `id`, applied one
 * entry at a time: { "tag": "0x80010000", "type": "i32", "value": 0,
 * "on": "preview" } (type: u8 | i32 | float | i64; on: preview | photo |
 * all). Each entry is logged with the engine's answer. The scanner is the
 * same simple one as for the other fields: it finds the camera's block,
 * then the list, then each {...} in it. */
static void apply_vendor_requests(indraw_vendor_set_fn vs, const char *id, char *out)
{
    char *buf = NULL, *cam = NULL, *blk, *lst = NULL, *end = NULL, *p;
    char msg[200];
    int n = 0, which;
    if (!vs) return;
    for (which = 0; which < 2 && !lst; ++which) {    /* the user's list, else the shipped */
        free(buf);
        buf = ov_source(which);
        if (!buf) continue;
        cam = ov_cam(buf, id, &end);
        lst = cam ? strstr(cam, "\"vendor_request\"") : NULL;
        if (lst && end && lst > end) lst = NULL;
    }
    if (!lst) { free(buf); return; }
    blk = cam;
    p = strchr(lst, '[');
    end = p ? strchr(p, ']') : NULL;
    if (!p || !end) { free(buf); return; }
    (void)blk;
    while ((p = strchr(p, '{')) != NULL && p < end) {
        char *close = strchr(p, '}'), *t, *ty, *va, *on;
        unsigned long tag = 0;
        int type = -1, onv = 0;
        double value = 0;
        if (!close) break;
        *close = '\0';
        if ((t = strstr(p, "\"tag\"")) && (t = strchr(t, ':'))) { while (*++t == ' ' || *t == '"') {} tag = strtoul(t, NULL, 16); }
        if ((ty = strstr(p, "\"type\"")) && (ty = strchr(ty, ':'))) {
            while (*++ty == ' ' || *ty == '"') {}
            type = !strncmp(ty, "u8", 2) ? 0 : !strncmp(ty, "i32", 3) ? 1 : !strncmp(ty, "float", 5) ? 2 : !strncmp(ty, "i64", 3) ? 3 : -1;
        }
        if ((va = strstr(p, "\"value\"")) && (va = strchr(va, ':'))) value = strtod(va + 1, NULL);
        if ((on = strstr(p, "\"on\"")) && (on = strchr(on, ':'))) {
            while (*++on == ' ' || *on == '"') {}
            onv = !strncmp(on, "photo", 5) ? 1 : !strncmp(on, "all", 3) ? 2 : 0;
        }
        *close = '}';
        if (tag >= 0x80000000UL && type >= 0) {
            vs((uint32_t)tag, type, value, onv, out, OUT_SIZE);
            snprintf(msg, sizeof msg, "vendor_request 0x%08lx: %.150s", tag, out);
            log_line("# ", msg);
            n++;
        } else {
            snprintf(msg, sizeof msg, "vendor_request: entry ignored (tag must be 0x8000xxxx, type u8|i32|float|i64)");
            ov_log(msg);
        }
        p = close + 1;
    }
    free(buf);
    if (n) {
        indraw_vendor_report_fn vr = (indraw_vendor_report_fn)a_dlsym(g_bridge_for_vendor, "indraw_vendor_report");
        snprintf(msg, sizeof msg, "vendor_request: %d entr%s applied for camera %s", n, n > 1 ? "ies" : "y", id);
        log_line("# ", msg);
        /* What the HAL reports back for those keys, a moment later: does it
         * apply what we sent? ("absent": the results do not carry the key.) */
        if (vr) {
            usleep(700000);
            vr(out, OUT_SIZE);
            log_line("# vendor_request in the results: ", out);
        }
    }
}

/* The camera block's "shading": "measured" (default: the measured lens-
 * shading calibration when one is found for this phone, else the HAL's
 * map), "hal", or "none". As the engine's mode: 1, 0, 2. */
static int read_override_shading(const char *id)
{
    char v[32];
    if (!ov_string(id, "shading", v, sizeof v)) return 1;
    return !strcmp(v, "hal") ? 0 : !strcmp(v, "none") ? 2 : 1;
}

/* The camera block's "session_request": [ {tag, type, value}, ... ] -- keys
 * the HAL reads at session creation (the probe's session_keys). Same entry
 * syntax as vendor_request, without "on". Passed to the engine before open;
 * the user's list if any, else the shipped profile's. */
static void apply_session_requests(void *bridge, const char *id)
{
    typedef int (*set_fn)(uint32_t, int, double);
    set_fn ss = (set_fn)a_dlsym(bridge, "indraw_set_session_request");
    char *buf = NULL, *cam = NULL, *lst = NULL, *end = NULL, *p;
    char msg[200];
    int n = 0, which;
    if (!ss) return;
    for (which = 0; which < 2 && !lst; ++which) {
        free(buf);
        buf = ov_source(which);
        if (!buf) continue;
        cam = ov_cam(buf, id, &end);
        lst = cam ? find_key(cam, end, "\"session_request\"") : NULL;
    }
    if (!lst) { free(buf); return; }
    {   /* the list ends at its ']': the entries of a vendor_request list that
         * follows are not session parameters (the first version read on to
         * the camera block's end and counted them twice) */
        char *lend = strchr(lst, ']');
        if (lend && (!end || lend < end)) end = lend;
    }
    p = lst;
    while ((p = strstr(p, "\"tag\"")) != NULL && (!end || p < end)) {
        char *q = strchr(p, ':'), *e;
        unsigned long tag;
        int type = 1;
        double value = 0;
        char *t, *v;
        if (!q) break;
        tag = strtoul(q + 1, &e, 0);
        if (e == q + 1) { char *qq = strchr(q, '"'); if (!qq) break; tag = strtoul(qq + 1, &e, 0); }
        t = strstr(p, "\"type\"");
        if (t && (!end || t < end) && (t = strchr(t, '"')) && (t = strchr(t + 1, '"'))) {
            if (!strncmp(t + 1, "u8", 2)) type = 0; else if (!strncmp(t + 1, "i32", 3)) type = 1;
            else if (!strncmp(t + 1, "float", 5)) type = 2; else if (!strncmp(t + 1, "i64", 3)) type = 3;
            else if (!strncmp(t + 1, "double", 6)) type = 4;
        }
        v = strstr(p, "\"value\"");
        if (v && (!end || v < end) && (v = strchr(v, ':'))) value = strtod(v + 1, NULL);
        if (ss((uint32_t)tag, type, value) == 0) ++n;
        p = e;
    }
    free(buf);
    snprintf(msg, sizeof msg, "%d entr%s for the session (applied at open if the HAL supports session parameters)", n, n == 1 ? "y" : "ies");
    log_line("# session_request: ", msg);
}

/* The camera block's "still_template": "preview" builds the photo and RAW
 * requests from the preview template (a device-profile test, e.g. the XA2);
 * anything else or absent: the still template, as always. */
static int read_override_still_template(const char *id)
{
    char v[32];
    return ov_string(id, "still_template", v, sizeof v) && !strcmp(v, "preview");
}

/* An override field, range-checked: 0 (= not set, default kept) when the
 * value is outside the field's range, with a log line saying so. */
static long long read_override_checked(const char *id, const char *field)
{
    long long v = read_override_i64(id, field, 0);
    int i;
    char msg[200];
    if (v == 0) return 0;
    for (i = 0; k_ov_fields[i].key; ++i)
        if (!strcmp(k_ov_fields[i].key, field) && (v < k_ov_fields[i].lo || v > k_ov_fields[i].hi)) {
            snprintf(msg, sizeof msg, "override-hal.json: %s = %lld is outside %lld..%lld -- ignored, default kept",
                     field, v, k_ov_fields[i].lo, k_ov_fields[i].hi);
            ov_log(msg);
            return 0;
        }
    return v;
}

/* The two sources of focus calibration, merged: override-hal.json (hand
 * written) first, then focus-calibration.json (the assistant's, see
 * cmd_calib), a point of the latter replacing one at the same distance. */
static int read_focus_calibration(const char *id, float *real_d, float *hal_d, int max)
{
    char dir[1024], file[1100];
    float r2[16], h2[16];
    int n, n2, i, j;
    if (profile_dir(dir, sizeof dir) != 0) return 0;
    snprintf(file, sizeof file, "%s/override-hal.json", dir);
    n = read_focus_calibration_file(file, id, real_d, hal_d, max);
    snprintf(file, sizeof file, "%s/focus-calibration.json", dir);
    n2 = read_focus_calibration_file(file, id, r2, h2, 16);
    for (i = 0; i < n2; ++i) {
        for (j = 0; j < n; ++j) if (fabsf(real_d[j] - r2[i]) < 1e-4f) { hal_d[j] = h2[i]; break; }
        if (j == n && n < max) { real_d[n] = r2[i]; hal_d[n] = h2[i]; ++n; }
    }
    for (i = 1; i < n; ++i)                        /* keep it sorted               */
        for (j = i; j > 0 && real_d[j] < real_d[j - 1]; --j) {
            float t = real_d[j]; real_d[j] = real_d[j - 1]; real_d[j - 1] = t;
            t = hal_d[j]; hal_d[j] = hal_d[j - 1]; hal_d[j - 1] = t;
        }
    return n;
}

/* Writes the assistant's calibration file for one camera (whole file, one
 * camera per file is enough: the file is ours and tiny). */
static int write_focus_calibration(const char *id, const float *real_d, const float *hal_d, int n)
{
    char dir[1024], file[1100], part[1110];
    FILE *f;
    int i;
    if (profile_dir(dir, sizeof dir) != 0 || mkdir_p(dir) != 0) return -1;
    snprintf(file, sizeof file, "%s/focus-calibration.json", dir);
    snprintf(part, sizeof part, "%s.part", file);
    f = fopen(part, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"_comment\": \"written by 'rawfish-ir-cli calib': manual focus distance -> the HAL's lens value; merged over override-hal.json\",\n"
               "  \"cameras\": [\n    {\n      \"camera_id\": \"%s\",\n      \"focus_calibration\": [\n", id);
    for (i = 0; i < n; ++i) {
        if (real_d[i] > 0) fprintf(f, "        { \"real_m\": %.3f, \"hal_diopters\": %.3f }%s\n", 1.0 / real_d[i], hal_d[i], i + 1 < n ? "," : "");
        else fprintf(f, "        { \"real_m\": null, \"hal_diopters\": %.3f }%s\n", hal_d[i], i + 1 < n ? "," : "");
    }
    fprintf(f, "      ]\n    }\n  ]\n}\n");
    if (fclose(f) != 0 || rename(part, file) != 0) { unlink(part); return -1; }
    return 0;
}

/* Builds an output path: ~/Pictures/RAWfish-ir/<prefix>_<YYYYMMDD>_<HHMMSS>_<ms>.<ext>
 * The command's filename argument becomes <prefix>; indraw always appends the
 * timestamp (with milliseconds, so a burst never collides) and the extension
 * for the command. Creates ~/Pictures/RAWfish-ir/ if missing. Returns 0, or -1 if
 * the path does not fit or HOME is unset. (Note: ~/Pictures is NOT one of the
 * three dirs Sailjail auto-whitelists; the CLI tool runs unsandboxed, but the
 * future Qt app will need a Pictures permission to write here.) */
/* Output folders: ~/Pictures/RAWfish-ir by default for everything; the app (or
 * the caller) can put JPEGs and DNGs elsewhere with --jpgdir / --dngdir, a
 * "~/" prefix standing for HOME. Nothing else decides where files go. */
static char g_jpgdir[1024], g_dngdir[1024];
static int expand_home(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!in || !in[0]) return -1;
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        if (!home || !home[0]) return -1;
        return snprintf(out, n, "%s%s", home, in + 1) >= (int)n ? -1 : 0;
    }
    return snprintf(out, n, "%s", in) >= (int)n ? -1 : 0;
}
static int build_output_path(const char *prefix, const char *ext,
                             const struct timespec *ts, char *buf, size_t n)
{
    const char *home = getenv("HOME");
    const char *want = (!strcmp(ext, "dng") || !strcmp(ext, "raw16")) ? g_dngdir : g_jpgdir;
    char dir[1024];
    struct tm tm;
    time_t t = ts->tv_sec;
    int len, ms;
    if (want[0]) { snprintf(dir, sizeof dir, "%s", want); }
    else {
        if (!home || !home[0]) return -1;
        if (snprintf(dir, sizeof dir, "%s/Pictures/RAWfish-ir", home) >= (int)sizeof dir) return -1;
    }
    if (mkdir_p(dir) != 0) return -1;             /* ensure the folder exists     */
    localtime_r(&t, &tm);                         /* the SAME instant as the EXIF */
    ms = (int)(ts->tv_nsec / 1000000);            /* milliseconds 0..999          */
    len = snprintf(buf, n, "%s/%s_%04d%02d%02d_%02d%02d%02d_%03d.%s",
                   dir, prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec, ms, ext);
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* Loads libhybris-common and its android_dlopen/android_dlsym. */
static int load_hybris(void)
{
    void *h = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL); /* usual name */
    if (!h) h = dlopen("libhybris-common.so", RTLD_NOW | RTLD_LOCAL); /* fallback   */
    if (!h) { fprintf(stderr, "rawfish-ir-cli: %s\n", dlerror()); return 0; }
    a_dlopen = (android_dlopen_fn)dlsym(h, "android_dlopen");
    a_dlsym = (android_dlsym_fn)dlsym(h, "android_dlsym");
    if (!a_dlopen || !a_dlsym) { fprintf(stderr, "rawfish-ir-cli: no android_dlopen\n"); return 0; }
    return 1;
}

/* Starts the process's binder thread pool by initialising libdroidmedia.
 * Without it the camera service cannot call us back: no image ever arrives. */
static int start_binder_pool(void)
{
    void *dm = a_dlopen("libdroidmedia.so", RTLD_NOW);
    void (*init)(void);
    if (!dm) { fprintf(stderr, "rawfish-ir-cli: libdroidmedia.so not loaded\n"); return 0; }
    init = (void (*)(void))a_dlsym(dm, "_droid_media_init");
    if (!init) { fprintf(stderr, "rawfish-ir-cli: _droid_media_init missing\n"); return 0; }
    init();
    return 1;
}

/* Parses "4096x3072" into w and h. Returns 1 on success. */
/* "WxH", or "max" (= -1 x -1: the engine picks the largest listed size; for
 * the preview, the largest no wider than 1280). */
static int parse_size(const char *s, int *w, int *h)
{
    char extra;
    if (s && !strcmp(s, "max")) { *w = *h = -1; return 1; }
    return s && sscanf(s, "%dx%d%c", w, h, &extra) == 2 && *w > 0 && *h > 0;
}

/* Name -> Camera2 value in a table; -1 for "default", -2 if unknown. */
typedef struct { const char *name; int value; } name_value_t;
/* "default_*" = leave the template's value (-1). */
static const name_value_t k_noise[] = { /* NOISE_REDUCTION_MODE_* */
    { "default_noise", -1 }, { "off_noise", 0 }, { "fast_noise", 1 }, { "hq_noise", 2 },
    { "minimal_noise", 3 }, { "zsl_noise", 4 }, { NULL, 0 } };
static const name_value_t k_edge[] = {  /* EDGE_MODE_*            */
    { "default_edge", -1 }, { "off_edge", 0 }, { "fast_edge", 1 }, { "hq_edge", 2 },
    { "zsl_edge", 3 }, { NULL, 0 } };

/* Returns 1 and sets *value if `s` is a name in table `t`, else 0. */
static int lookup(const name_value_t *t, const char *s, int *value)
{
    for (; t->name; ++t)
        if (!strcmp(t->name, s)) { *value = t->value; return 1; }
    return 0;
}

/* Parses an exposure in seconds ("16", "0.5", "1/125") into nanoseconds.
 * Returns 1 on success. */
/* Parses an exposure in DECIMAL seconds ("16", "0.5", "0.008") into
 * nanoseconds. Fractions like 1/125 are not accepted here (the CLI stays
 * decimal, so no "/" ever appears in a command); a UI can offer fractions and
 * pass the decimal value. Returns 1 on success. */
static int parse_exposure(const char *s, int64_t *ns)
{
    double sec;
    char extra;
    if (sscanf(s, "%lf%c", &sec, &extra) != 1 || sec <= 0) return 0; /* seconds   */
    *ns = (int64_t)(sec * 1e9 + 0.5);
    return 1;
}

/* Focus mode codes, same values as IR_FOCUS_* on the Android side. */
#define F_KEEP 0
#define F_AUTO 1
#define F_CONTINUOUS 2
#define F_CONTINUOUS_VIDEO 3
#define F_MACRO 4
#define F_EDOF 5
#define F_MANUAL 6
#define F_HOLD 7
#define F_RAW 8        /* the HAL's own lens value, no focus calibration */

/* Parses a focus= option into a mode and (for manual) a distance in diopters.
 * Names stay close to Camera2 (the UI renames for display):
 *   focus=auto | continuous | continuous_video | macro | edof | inf
 *   focus=<metres>m   e.g. focus=0.5m  -> AF off, lens at 1/0.5 = 2 diopters
 * Returns 1 on success, filling *mode and *diopters (0 unless manual). */
static int parse_focus(const char *v, int *mode, float *diopters)
{
    *diopters = 0.0f;
    if (!strcmp(v, "auto")) { *mode = F_AUTO; return 1; }
    if (!strcmp(v, "continuous")) { *mode = F_CONTINUOUS; return 1; }
    if (!strcmp(v, "continuous_video")) { *mode = F_CONTINUOUS_VIDEO; return 1; }
    if (!strcmp(v, "macro")) { *mode = F_MACRO; return 1; }
    if (!strcmp(v, "edof")) { *mode = F_EDOF; return 1; }
    if (!strcmp(v, "inf")) { *mode = F_MANUAL; *diopters = 0.0f; return 1; } /* infinity */
    if (!strcmp(v, "hold")) { *mode = F_HOLD; return 1; }   /* the lens where it is now */
    if (!strncmp(v, "raw", 3)) {   /* "rawD" -> the HAL's lens value D, no calibration */
        char *end = NULL;
        double d = strtod(v + 3, &end);
        if (end && *end == '\0' && d >= 0) { *mode = F_RAW; *diopters = (float)d; return 1; }
    }
    {   /* "<metres>m" -> manual distance */
        char *end = NULL;
        double m = strtod(v, &end);
        if (end && *end == 'm' && end[1] == '\0' && m > 0) {
            *mode = F_MANUAL;
            *diopters = (float)(1.0 / m);          /* diopters = 1 / metres        */
            return 1;
        }
    }
    return 0;
}

/* Recognises a variant option; returns its flag, or 0 if `tok` is not one. */
static int parse_variant(const char *tok)
{
    if (!strcmp(tok, "preview=pause")) return V_PAUSE;
    if (!strcmp(tok, "fps=low")) return V_FPS_LOW;
    if (!strcmp(tok, "control=off")) return V_CONTROL_OFF;
    return 0;
}

/* Max wait for a photo: 10 s of margin plus twice the exposure. */
static int timeout_for(int64_t exposure_ns)
{
    return 10000 + (int)(exposure_ns / 500000);       /* 2 x exposure, in ms */
}

static void usage(void)
{
    fprintf(stderr,
            "usage: rawfish-ir-cli [--log FILE] [--jpgdir DIR] [--dngdir DIR] caps ID\n"
            "       rawfish-ir-cli [--log FILE] cameras\n"
            "       rawfish-ir-cli [--log FILE] probe          (write generated-hal.json)\n"
            "       rawfish-ir-cli [--log FILE] streamtest ID  (which streams this camera survives; the override to use)\n"
            "       indraw [--log FILE] run ID JPEG_WxH [PREVIEW_WxH] [raw=WxH]   (default preview 1280x960)\n"
            "       indraw [--log FILE] stream ID JPEG_WxH [PREVIEW_WxH] [raw=WxH]   (SF2P frames on stdout)\n"
            "         any size may be 'max' (largest listed; preview: widest <= 1280)\n"
            "  files are written to ~/Pictures/RAWfish-ir/ (or --jpgdir / --dngdir) as NAME_YYYYMMDD_HHMMSS_mmm.EXT\n"
            "  stdin: jpeg NAME [q=95] [default_noise|off_noise|fast_noise|hq_noise|minimal_noise|zsl_noise]\n"
            "                   [default_edge|off_edge|fast_edge|hq_edge|zsl_edge]\n"
            "                   [focus=auto|continuous|continuous_video|macro|edof|inf|<m>m] [easy]\n"
            "                   [exp=T] [iso=N]\n"
            "         raw NAME|- [focus=auto|continuous|...|inf|<m>m] [exp=SEC] [iso=N]   (- = measure only)\n"
            "         stack NAME film=T [sub=T] [stk_denoise=1|2|4] [iso=N]\n"
            "         noiseprofile FILE.json [sub=T]   (darktable noise profile)\n"
            "         focus MODE|hold|rawD [nowait]   (set preview focus live; hold = the lens where it is; rawD = the HAL lens value D)\n"
            "         preview [off_noise|...] [off_edge|...]   (live viewfinder noise/edge)\n"
            "         preview-frame FILE.ppm   (save one preview frame, phase-1 diagnostic)\n"
            "         exposure auto | exp=SEC iso=N   (live preview exposure: AE, or a manual pair)\n"
            "         ev STEPS   (AE compensation in HAL steps, see the caps line)\n"
            "         meter on|off [rate=HZ]   (RAW16 exposure meter -> SF2M meter lines)\n"
            "         zoom FACTOR   (digital zoom, crop region on preview and photos; RAW stays full)\n"
            "         tap NX NY [ae] [af] [clear]   (metering/focus region at a point of the viewfinder frame 0..1)\n"
            "         wb auto|incandescent|fluorescent|warm_fluorescent|daylight|cloudy|twilight|shade\n"
            "         wb shift=S [tint=T]   (manual, from the AWB: S x5%% red/blue -20..20, T green..magenta -100..100)\n"
            "         scene off|N   (Camera2 scene mode number from caps; the HAL then drives 3A)\n"
            "         afwait MS   (a photo's 3A wait with the flash or an AF scan; 0 = 1500; otherwise <= 500)\n"
            "         calib D|inf|clear   (focus calibration assistant: record 'lens now' for distance D m)\n"
            "         burst NAME [n=2..8] [q=Q] [NOISE] [EDGE] [exp=SEC iso=N]   (JPEGs back to back, one exposure)\n"
            "         flash off|auto|on|torch\n"
            "         aelock on|off   (hold / free the automatic exposure)\n"
            "         orient DEG      (the phone's physical rotation: photos come out upright on the ground)\n"
            "         viewwin X Y W H (the window the interface shows, 0..1: what the histograms cover)\n"
            "         wb pick | wb fixed R,GE,GO,B | wb fixed off   (a white measured on the spot / centre, kept)\n"
            "         shading balance=B strength=S maxgain=M   (the measured lens-shading map's dosing)\n"
            "         shading calibrate full cct=K lens=D1,D2,...   (measure a calibration; flat field, diffuser)\n"
            "         shading calibrate light cct=K                 (add a light to the existing calibration)\n"
            "         loupe FACTOR [CX CY]   (focus loupe: crops the preview only; 1 = off)\n"
            "         variants for jpeg/raw: [preview=pause] [fps=low] [control=off]\n"
            "         # TEXT   (comment: logged, ignored -- the app writes its diagnostics this way)\n"
            "         quit\n");
}

/* Handles one "noiseprofile ..." line. */
static int cmd_noiseprofile(indraw_noiseprofile_fn np, char **save, char *out)
{
    char *file = strtok_r(NULL, " \t\r\n", save);
    char *tok;
    int64_t sub_ns = 0;
    if (!file) { emit("{\"ok\":false,\"error\":\"noiseprofile: missing FILE\"}"); return 1; }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strncmp(tok, "sub=", 4)) {
            if (!parse_exposure(tok + 4, &sub_ns)) { emit("{\"ok\":false,\"error\":\"noiseprofile: bad sub\"}"); return 1; }
        } else { emit("{\"ok\":false,\"error\":\"noiseprofile: unknown option\"}"); return 1; }
    }
    if (!np) { emit("{\"ok\":false,\"error\":\"noiseprofile: bridge has no indraw_noiseprofile\"}"); return 1; }
    if (np(file, sub_ns, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "preview ..." line: set the live preview (and photo default)
 * noise/edge modes. Accepts the same mode names as jpeg (off_noise, hq_edge...).
 * The viewfinder changes at once; this is the lever behind the UI menu. */
static int cmd_preview(indraw_preview_settings_fn ps, char **save, char *out)
{
    char *tok;
    int nr = -1, edge = -1, v;
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (lookup(k_noise, tok, &v)) nr = v;          /* a *_noise name          */
        else if (lookup(k_edge, tok, &v)) edge = v;    /* a *_edge name           */
        else { emit("{\"ok\":false,\"error\":\"preview: unknown option (use *_noise / *_edge)\"}"); return 1; }
    }
    if (nr < 0 && edge < 0) { emit("{\"ok\":false,\"error\":\"preview: give a noise and/or edge mode\"}"); return 1; }
    if (!ps) { emit("{\"ok\":false,\"error\":\"preview: bridge has no indraw_preview_settings\"}"); return 1; }
    if (ps(nr, edge, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "preview-frame ..." line: save one live preview frame as a PPM
 * (phase-1 diagnostic, to check the viewfinder content and framing). */
static int cmd_preview_frame(indraw_preview_frame_fn pf, char **save, char *out)
{
    char *file = strtok_r(NULL, " \t\r\n", save);
    if (!file) { emit("{\"ok\":false,\"error\":\"preview-frame: missing FILE\"}"); return 1; }
    if (!pf) { emit("{\"ok\":false,\"error\":\"preview-frame: bridge has no indraw_preview_frame\"}"); return 1; }
    if (pf(file, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "focus ..." line: set the preview focus live, take no photo. */
static indraw_focus_opt_fn g_focus_opt;         /* resolved at open: focus without the wait */
static int cmd_focus(indraw_focus_fn focusfn, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), *opt = strtok_r(NULL, " \t\r\n", save);
    int mode = F_KEEP, wait = !(opt && !strcmp(opt, "nowait"));
    float diopters = 0.0f;
    if (!v) { emit("{\"ok\":false,\"error\":\"focus: missing MODE\"}"); return 1; }
    if (!parse_focus(v, &mode, &diopters)) {
        emit("{\"ok\":false,\"error\":\"focus: bad MODE\"}");
        return 1;
    }
    if (!focusfn) { emit("{\"ok\":false,\"error\":\"focus: bridge has no indraw_focus\"}"); return 1; }
    /* "nowait": a manual distance returns at once (live stepping). */
    if (!wait && g_focus_opt) { if (g_focus_opt(mode, diopters, 0, out, OUT_SIZE)) { emit(out); return 1; } }
    else if (focusfn(mode, diopters, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "exposure ..." line: live preview exposure. `exposure auto` hands
 * back to the HAL's AE; `exposure exp=SEC iso=N` sets a manual pair (both are
 * required: the engine takes fixed values, the UI decides what to pin). */
static int cmd_exposure(indraw_preview_exposure_fn pe, char **save, char *out)
{
    char *tok;
    int64_t exposure_ns = 0;
    int iso = 0, is_auto = 0;
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strcmp(tok, "auto")) is_auto = 1;
        else if (!strncmp(tok, "exp=", 4)) {
            if (!parse_exposure(tok + 4, &exposure_ns)) { emit("{\"ok\":false,\"error\":\"exposure: bad exp\"}"); return 1; }
        } else if (!strncmp(tok, "iso=", 4)) {
            iso = atoi(tok + 4);
            if (iso <= 0) { emit("{\"ok\":false,\"error\":\"exposure: bad iso\"}"); return 1; }
        } else { emit("{\"ok\":false,\"error\":\"exposure: unknown option\"}"); return 1; }
    }
    if (!is_auto && (exposure_ns <= 0 || iso <= 0)) {
        emit("{\"ok\":false,\"error\":\"exposure: give auto, or exp=SEC and iso=N\"}"); return 1;
    }
    if (!pe) { emit("{\"ok\":false,\"error\":\"exposure: bridge has no indraw_preview_exposure\"}"); return 1; }
    if (pe(is_auto ? 0 : exposure_ns, is_auto ? 0 : iso, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "ev STEPS" line: AE compensation in the HAL's steps. */
static int cmd_ev(indraw_ev_fn evfn, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    int steps;
    if (!v || sscanf(v, "%d%c", &steps, &extra) != 1) { emit("{\"ok\":false,\"error\":\"ev: bad STEPS\"}"); return 1; }
    if (!evfn) { emit("{\"ok\":false,\"error\":\"ev: bridge has no indraw_ev\"}"); return 1; }
    if (evfn(steps, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "meter on|off [rate=HZ]" line: the RAW16 exposure meter. */
static int cmd_meter(indraw_meter_fn mt, char **save, char *out)
{
    char *tok;
    int on = -1, interval_ms = 0;
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strcmp(tok, "on")) on = 1;
        else if (!strcmp(tok, "off")) on = 0;
        else if (!strncmp(tok, "rate=", 5)) {
            double hz = atof(tok + 5);
            if (hz <= 0 || hz > 20) { emit("{\"ok\":false,\"error\":\"meter: bad rate (0 < HZ <= 20)\"}"); return 1; }
            interval_ms = (int)(1000.0 / hz + 0.5);
        } else { emit("{\"ok\":false,\"error\":\"meter: unknown option\"}"); return 1; }
    }
    if (on < 0) { emit("{\"ok\":false,\"error\":\"meter: give on or off\"}"); return 1; }
    if (!mt) { emit("{\"ok\":false,\"error\":\"meter: bridge has no indraw_meter\"}"); return 1; }
    if (mt(on, interval_ms, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "tap NX NY [ae] [af] [clear]" line: a metering/focus region at
 * the normalised sensor point (NX, NY), 0..1. Default ae+af; "clear" removes
 * both regions. The UI maps the touch to sensor coords before calling. */
static int cmd_tap(indraw_tap_fn tf, char **save, char *out)
{
    char *xs = strtok_r(NULL, " \t\r\n", save), *ys = strtok_r(NULL, " \t\r\n", save), *tok, ex;
    float nx, ny;
    int do_ae = -1, do_af = -1;
    if (!xs || !ys || sscanf(xs, "%f%c", &nx, &ex) != 1 || sscanf(ys, "%f%c", &ny, &ex) != 1) {
        emit("{\"ok\":false,\"error\":\"tap: need NX NY (0..1)\"}"); return 1;
    }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strcmp(tok, "ae")) { if (do_ae < 0) do_ae = 0; do_ae = 1; }
        else if (!strcmp(tok, "af")) { if (do_af < 0) do_af = 0; do_af = 1; }
        else if (!strcmp(tok, "clear")) { do_ae = 0; do_af = 0; }
        else { emit("{\"ok\":false,\"error\":\"tap: unknown option (ae|af|clear)\"}"); return 1; }
    }
    if (do_ae < 0 && do_af < 0) { do_ae = 1; do_af = 1; }   /* default: both       */
    if (do_ae < 0) do_ae = 0;
    if (do_af < 0) do_af = 0;
    if (!tf) { emit("{\"ok\":false,\"error\":\"tap: bridge has no indraw_tap\"}"); return 1; }
    if (tf(nx, ny, do_ae, do_af, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "wb MODE" line: auto | incandescent | fluorescent |
 * warm_fluorescent | daylight | cloudy | twilight | shade (Camera2 names). */
static int cmd_wb(indraw_wb_fn wf, indraw_wb_manual_fn wm, char **save, char *out)
{
    static const name_value_t k_awb[] = {
        { "auto", 1 }, { "incandescent", 2 }, { "fluorescent", 3 }, { "warm_fluorescent", 4 },
        { "daylight", 5 }, { "cloudy", 6 }, { "twilight", 7 }, { "shade", 8 }, { NULL, 0 } };
    char *v = strtok_r(NULL, " \t\r\n", save), *v2, extra;
    int mode, shift, tint = 0;
    if (!v) { emit("{\"ok\":false,\"error\":\"wb: give a MODE or shift=S [tint=T]\"}"); return 1; }
    /* Manual: "wb shift=S [tint=T]": S steps of 5 % on red/blue from the AWB
     * (-20..20, + = warmer), T percent of green (-100..100, + = magenta). */
    if (!strncmp(v, "shift=", 6)) {
        if (sscanf(v + 6, "%d%c", &shift, &extra) != 1) { emit("{\"ok\":false,\"error\":\"wb: bad shift\"}"); return 1; }
        if ((v2 = strtok_r(NULL, " \t\r\n", save)) && (strncmp(v2, "tint=", 5) || sscanf(v2 + 5, "%d%c", &tint, &extra) != 1)) {
            emit("{\"ok\":false,\"error\":\"wb: bad tint\"}"); return 1;
        }
        if (!wm) { emit("{\"ok\":false,\"error\":\"wb: bridge has no indraw_wb_manual\"}"); return 1; }
        if (wm(shift, tint, out, OUT_SIZE)) { emit(out); return 1; }
        emit(out);
        return 0;
    }
    if (!lookup(k_awb, v, &mode)) { emit("{\"ok\":false,\"error\":\"wb: bad MODE\"}"); return 1; }
    if (!wf) { emit("{\"ok\":false,\"error\":\"wb: bridge has no indraw_wb\"}"); return 1; }
    if (wm) wm(0, 0, out, OUT_SIZE);                 /* leave manual first, if on */
    if (wf(mode, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "scene MODE" line: off, or a Camera2 scene mode number
 * (caps scene_modes), e.g. 5 night, 18 hdr. */
static int cmd_scene(indraw_scene_fn sf, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    int mode;
    if (!v) { emit("{\"ok\":false,\"error\":\"scene: give off or a mode number\"}"); return 1; }
    if (!strcmp(v, "off")) mode = 0;
    else if (sscanf(v, "%d%c", &mode, &extra) != 1 || mode < 0) { emit("{\"ok\":false,\"error\":\"scene: bad MODE\"}"); return 1; }
    if (!sf) { emit("{\"ok\":false,\"error\":\"scene: bridge has no indraw_scene\"}"); return 1; }
    if (sf(mode, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* --- Aborting a running stack ---
 * Two ways reach indraw_request_abort() while the command loop is busy inside
 * a long stack: SIGUSR1 from the app (its "abort" button), and the end of
 * stdin -- the app closed or died, so nothing must stay running: the stack
 * stops at the next sub-frame, writes what it accumulated, and the loop's
 * getline() then returns EOF and the engine closes the camera. The EOF is
 * noticed by a small thread polling stdin for hang-up while a command runs. */
static indraw_request_abort_fn g_abort_fn;
static void on_sigusr1(int sig) { (void)sig; if (g_abort_fn) g_abort_fn(); }
static void *eof_watch(void *arg)
{
    struct pollfd pfd = { 0, POLLHUP | POLLERR, 0 };
    (void)arg;
    for (;;) {
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & (POLLHUP | POLLERR))) { if (g_abort_fn) g_abort_fn(); break; }
        if (!g_abort_fn) break;
    }
    return NULL;
}

/* Handles one "calib D" line (D in metres, or "inf"): the focus calibration
 * assistant. The user has just focused (circle) on a target at the known
 * distance D; the HAL's current lens value is recorded as the value D maps
 * to, in focus-calibration.json, replacing an earlier point at the same
 * distance; the running engine applies the merged table at once. "calib
 * clear" forgets the assistant's points (override-hal.json ones stay). */
static int cmd_calib(indraw_lens_position_fn lp, indraw_set_focus_cal_fn setcal, const char *id, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    char dir[1024], file[1100];
    float real_d[16], hal_d[16], real, lens;
    int n, i;
    if (!v) { emit("{\"ok\":false,\"error\":\"calib: give a distance in metres, inf, or clear\"}"); return 1; }
    if (profile_dir(dir, sizeof dir) != 0) { emit("{\"ok\":false,\"error\":\"calib: no profile directory\"}"); return 1; }
    if (!strcmp(v, "clear")) {
        snprintf(file, sizeof file, "%s/focus-calibration.json", dir);
        unlink(file);
        emit("{\"ok\":true,\"cleared\":true}");
        return 0;
    }
    if (!strcmp(v, "inf")) real = 0.0f;
    else {
        float m;
        if (sscanf(v, "%f%c", &m, &extra) != 1 || m <= 0) { emit("{\"ok\":false,\"error\":\"calib: bad distance\"}"); return 1; }
        real = 1.0f / m;
    }
    if (!lp || !setcal) { emit("{\"ok\":false,\"error\":\"calib: bridge has no lens position\"}"); return 1; }
    lens = lp();
    if (!(lens >= 0.0f)) { emit("{\"ok\":false,\"error\":\"calib: lens position unknown yet\"}"); return 1; }
    /* Current assistant points (not the override's), updated, written back. */
    snprintf(file, sizeof file, "%s/focus-calibration.json", dir);
    n = read_focus_calibration_file(file, id, real_d, hal_d, 16);
    for (i = 0; i < n; ++i) if (fabsf(real_d[i] - real) < 1e-4f) break;
    if (i == n) { if (n >= 16) { emit("{\"ok\":false,\"error\":\"calib: 16 points max\"}"); return 1; } n++; }
    real_d[i] = real; hal_d[i] = lens;
    if (write_focus_calibration(id, real_d, hal_d, n) != 0) { emit("{\"ok\":false,\"error\":\"calib: cannot write focus-calibration.json\"}"); return 1; }
    /* Apply the merged table to the running engine right away. */
    n = read_focus_calibration(id, real_d, hal_d, 16);
    if (n > 0) setcal(real_d, hal_d, n, out, OUT_SIZE);
    snprintf(out, OUT_SIZE, "{\"ok\":true,\"real_m\":%s%.3f,\"hal_diopters\":%.3f,\"points\":%d}",
             real > 0 ? "" : "null,\"infinity\":true,\"x\":", real > 0 ? 1.0 / real : 0.0, lens, n);
    emit(out);
    return 0;
}

/* Handles one "afwait MS" line: how long a photo waits for its AF scan /
 * AE precapture (0 = the default 2000 ms). */
static int cmd_afwait(indraw_set_af_wait_fn fn, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    int ms;
    if (!v || sscanf(v, "%d%c", &ms, &extra) != 1 || ms < 0) { emit("{\"ok\":false,\"error\":\"afwait: bad MS\"}"); return 1; }
    if (!fn) { emit("{\"ok\":false,\"error\":\"afwait: bridge has no indraw_set_af_wait\"}"); return 1; }
    if (fn(ms, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "zoom FACTOR" line. */
static int cmd_zoom(indraw_zoom_fn zf, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    float f;
    if (!v || sscanf(v, "%f%c", &f, &extra) != 1 || f <= 0) { emit("{\"ok\":false,\"error\":\"zoom: bad FACTOR\"}"); return 1; }
    if (!zf) { emit("{\"ok\":false,\"error\":\"zoom: bridge has no indraw_zoom\"}"); return 1; }
    if (zf(f, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "stack ..." line. */
static int cmd_stack(indraw_stack_fn stack, char **save, char *out)
{
    char *prefix = strtok_r(NULL, " \t\r\n", save); /* filename prefix (1st)   */
    char file[1200];                                  /* built timestamped path  */
    char *tok;
    int iso = 0, stk_denoise = 1;                 /* denoise x1 by default         */
    int64_t film_ns = 0, sub_ns = 0;
    struct timespec now;
    char dt[24];
    if (!prefix) { emit("{\"ok\":false,\"error\":\"stack: missing NAME\"}"); return 1; }
    if (!valid_name(prefix)) {
        emit("{\"ok\":false,\"error\":\"stack: invalid name (letters, digits, - _ only)\"}");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &now);          /* name and EXIF share this     */
    exif_from_ts(&now, dt, sizeof dt);
    if (build_output_path(prefix, "dng", &now, file, sizeof file)) {
        emit("{\"ok\":false,\"error\":\"stack: cannot build output path\"}");
        return 1;
    }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strncmp(tok, "film=", 5)) {
            if (!parse_exposure(tok + 5, &film_ns)) { emit("{\"ok\":false,\"error\":\"stack: bad film\"}"); return 1; }
        } else if (!strncmp(tok, "sub=", 4)) {
            if (!parse_exposure(tok + 4, &sub_ns)) { emit("{\"ok\":false,\"error\":\"stack: bad sub\"}"); return 1; }
        } else if (!strncmp(tok, "stk_denoise=", 12)) stk_denoise = atoi(tok + 12);
        else if (!strncmp(tok, "iso=", 4)) iso = atoi(tok + 4);
        else { emit("{\"ok\":false,\"error\":\"stack: unknown option\"}"); return 1; }
    }
    if (!film_ns) { emit("{\"ok\":false,\"error\":\"stack: film= required\"}"); return 1; }
    if (!stack) { emit("{\"ok\":false,\"error\":\"stack: bridge has no indraw_stack\"}"); return 1; }
    if (stack(file, dt, film_ns, sub_ns, stk_denoise, iso, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "raw ..." line. */
static int cmd_raw(indraw_raw_fn raw, char **save, char *out)
{
    char *prefix = strtok_r(NULL, " \t\r\n", save); /* filename prefix (1st)   */
    char file[1200];                                  /* built timestamped path  */
    char *tok;
    int iso = 0, variant = 0, v;
    int focus_mode = F_KEEP, focus_set = 0;    /* default: keep the auto focus   */
    float focus_diopters = 0.0f;
    int64_t exposure_ns = 0;
    /* "-" keeps its measure-only meaning (writes nothing); any other value is a
     * filename prefix. raw writes a DNG, so the auto name uses .dng. */
    struct timespec now;
    char dt[24] = "";
    if (!prefix) { emit("{\"ok\":false,\"error\":\"raw: missing NAME\"}"); return 1; }
    if (!strcmp(prefix, "-")) {
        snprintf(file, sizeof file, "-");          /* measure only              */
    } else {
        if (!valid_name(prefix)) {
            emit("{\"ok\":false,\"error\":\"raw: invalid name (letters, digits, - _ only)\"}");
            return 1;
        }
        clock_gettime(CLOCK_REALTIME, &now);       /* name and EXIF share this   */
        exif_from_ts(&now, dt, sizeof dt);
        if (build_output_path(prefix, "dng", &now, file, sizeof file)) {
            emit("{\"ok\":false,\"error\":\"raw: cannot build output path\"}");
            return 1;
        }
    }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strncmp(tok, "exp=", 4)) {
            if (!parse_exposure(tok + 4, &exposure_ns)) {
                emit("{\"ok\":false,\"error\":\"raw: bad exp\"}");
                return 1;
            }
        }
        else if (!strncmp(tok, "iso=", 4)) iso = atoi(tok + 4);
        else if ((v = parse_variant(tok))) variant |= v;
        else if (!strncmp(tok, "focus=", 6)) {
            if (!parse_focus(tok + 6, &focus_mode, &focus_diopters)) {
                emit("{\"ok\":false,\"error\":\"raw: bad focus\"}");
                return 1;
            }
            focus_set = 1;
        }
        else { emit("{\"ok\":false,\"error\":\"raw: unknown option\"}"); return 1; }
    }
    if (!raw) { emit("{\"ok\":false,\"error\":\"raw: bridge has no indraw_raw\"}"); return 1; }
    /* Default (nothing set): all-auto -- keep the session's continuous focus and,
     * when exposure is auto, run an AE precapture. A manual exposure (exp=/iso=)
     * turns AE off, so no precapture then. Focus set explicitly overrides. */
    {
        int ae_precapture = (exposure_ns <= 0 && iso <= 0);
        (void)focus_set;
        if (raw(file, dt, focus_mode, focus_diopters, ae_precapture, exposure_ns, iso,
                variant, timeout_for(exposure_ns), out, OUT_SIZE)) {
            emit(out);
            return 1;
        }
    }
    emit(out);
    return 0;
}

/* Handles one "jpeg ..." line (tokens after "jpeg" come from strtok_r). */
static int cmd_jpeg(indraw_jpeg_fn jpeg, char **save, char *out)
{
    char *prefix = strtok_r(NULL, " \t\r\n", save); /* filename prefix (1st)   */
    char file[1200];                                  /* built timestamped path  */
    char *tok;
    int quality = 95, nr = -1, edge = -1, iso = 0, variant = 0, v;
    int focus_mode = F_KEEP, easy = 0;         /* default: keep the auto focus   */
    float focus_diopters = 0.0f;
    int64_t exposure_ns = 0;
    struct timespec now;
    if (!prefix) { emit("{\"ok\":false,\"error\":\"jpeg: missing NAME\"}"); return 1; }
    if (!valid_name(prefix)) {
        emit("{\"ok\":false,\"error\":\"jpeg: invalid name (letters, digits, - _ only)\"}");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &now);          /* one instant for the name     */
    if (build_output_path(prefix, "jpg", &now, file, sizeof file)) {
        emit("{\"ok\":false,\"error\":\"jpeg: cannot build output path\"}");
        return 1;
    }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {   /* key=value options    */
        if (!strncmp(tok, "q=", 2)) quality = atoi(tok + 2);
        else if (!strncmp(tok, "exp=", 4)) {
            if (!parse_exposure(tok + 4, &exposure_ns)) {
                emit("{\"ok\":false,\"error\":\"jpeg: bad exp\"}");
                return 1;
            }
        }
        else if (!strncmp(tok, "iso=", 4)) iso = atoi(tok + 4);
        else if ((v = parse_variant(tok))) variant |= v;
        else if (lookup(k_noise, tok, &nr)) continue;   /* a *_noise name       */
        else if (lookup(k_edge, tok, &edge)) continue;  /* a *_edge name        */
        else if (!strncmp(tok, "focus=", 6)) {
            if (!parse_focus(tok + 6, &focus_mode, &focus_diopters)) {
                emit("{\"ok\":false,\"error\":\"jpeg: bad focus\"}");
                return 1;
            }
        }
        else if (!strcmp(tok, "easy")) easy = 1;   /* combined AF + AE (one call) */
        else { emit("{\"ok\":false,\"error\":\"jpeg: unknown option\"}"); return 1; }
    }
    if (quality < 1 || quality > 100) {
        emit("{\"ok\":false,\"error\":\"jpeg: bad value\"}");
        return 1;
    }
    /* "easy" = the combined convenience path: one-shot AF + AE precapture. It is
     * the only command allowed to combine; everything else stays atomic. Without
     * it, the default is all-auto (keep continuous focus; AE precapture when the
     * exposure is auto). */
    {
        int fmode = easy ? F_AUTO : focus_mode;
        int ae_precapture = easy || (exposure_ns <= 0 && iso <= 0);
        if (jpeg(file, quality, nr, edge, fmode, focus_diopters, ae_precapture,
                 exposure_ns, iso, variant, timeout_for(exposure_ns), out, OUT_SIZE)) {
            emit(out);
            return 1;
        }
    }
    emit(out);
    return 0;
}

/* Handles one "burst NAME [n=N] [q=Q] [NOISE] [EDGE] [exp=SEC iso=N]" line:
 * N JPEGs (2..8, default 3) back to back at one exposure (the given manual
 * pair, or the live automatic one frozen). Each file gets its own name, the
 * timestamp's milliseconds advanced by one per frame. */
static int cmd_burst(indraw_burst_fn bf, char **save, char *out)
{
    char *prefix = strtok_r(NULL, " \t\r\n", save), *tok;
    static char files[8][1200];
    const char *paths[8];
    int n = 3, quality = 95, nr = -1, edge = -1, iso = 0, k;
    int64_t exposure_ns = 0;
    struct timespec now;
    if (!prefix || !valid_name(prefix)) { emit("{\"ok\":false,\"error\":\"burst: missing or invalid NAME\"}"); return 1; }
    while ((tok = strtok_r(NULL, " \t\r\n", save))) {
        if (!strncmp(tok, "n=", 2)) n = atoi(tok + 2);
        else if (!strncmp(tok, "q=", 2)) quality = atoi(tok + 2);
        else if (!strncmp(tok, "exp=", 4)) { if (!parse_exposure(tok + 4, &exposure_ns)) { emit("{\"ok\":false,\"error\":\"burst: bad exp\"}"); return 1; } }
        else if (!strncmp(tok, "iso=", 4)) iso = atoi(tok + 4);
        else if (lookup(k_noise, tok, &nr) || lookup(k_edge, tok, &edge)) continue;
        else { emit("{\"ok\":false,\"error\":\"burst: unknown option\"}"); return 1; }
    }
    if (n < 2 || n > 8 || quality < 1 || quality > 100) { emit("{\"ok\":false,\"error\":\"burst: n 2..8, q 1..100\"}"); return 1; }
    clock_gettime(CLOCK_REALTIME, &now);
    for (k = 0; k < n; ++k) {
        struct timespec t = now;
        t.tv_nsec += (long)k * 1000000L;              /* one ms apart: unique names  */
        if (t.tv_nsec >= 1000000000L) { t.tv_sec += 1; t.tv_nsec -= 1000000000L; }
        if (build_output_path(prefix, "jpg", &t, files[k], sizeof files[k])) {
            emit("{\"ok\":false,\"error\":\"burst: cannot build output path\"}"); return 1;
        }
        paths[k] = files[k];
    }
    if (!bf) { emit("{\"ok\":false,\"error\":\"burst: bridge has no indraw_burst\"}"); return 1; }
    if (bf(paths, n, quality, nr, edge, exposure_ns, iso, timeout_for(exposure_ns), out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "loupe FACTOR [CX CY]" line: the focus loupe on the preview
 * only (CX, CY: its centre in the sensor's active array, 0..1; default the
 * middle). FACTOR 1 closes it. */
static int cmd_loupe(indraw_loupe_fn fn, char **save, char *out)
{
    char *a = strtok_r(NULL, " \t\r\n", save), *b = strtok_r(NULL, " \t\r\n", save), *c = strtok_r(NULL, " \t\r\n", save), extra;
    float f, cx = 0.5f, cy = 0.5f;
    if (!a || sscanf(a, "%f%c", &f, &extra) != 1 || f < 1.0f ||
        (b && sscanf(b, "%f%c", &cx, &extra) != 1) || (c && sscanf(c, "%f%c", &cy, &extra) != 1)) {
        emit("{\"ok\":false,\"error\":\"loupe: FACTOR >= 1 [CX CY 0..1]\"}"); return 1;
    }
    if (!fn) { emit("{\"ok\":false,\"error\":\"loupe: bridge has no indraw_loupe\"}"); return 1; }
    if (fn(f, cx, cy, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "orient DEG" line: the phone's physical rotation (0/90/180/270
 * clockwise from its natural portrait), read at each shot for the photo's
 * orientation. */
static int cmd_orient(indraw_orient_fn fn, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save), extra;
    int d;
    if (!v || sscanf(v, "%d%c", &d, &extra) != 1) { emit("{\"ok\":false,\"error\":\"orient: DEG (0, 90, 180, 270)\"}"); return 1; }
    if (!fn) { emit("{\"ok\":false,\"error\":\"orient: bridge has no indraw_orient\"}"); return 1; }
    if (fn(d, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* "shading balance=B strength=S maxgain=M": the user's dosing of the measured
 * lens-shading map (pivot and strength 0..100, maximum gain >= 1); any field
 * left out keeps its value. */
static indraw_shading_calibrate_fn g_shading_calibrate;
/* "shading calibrate full cct=K lens=D1,D2,... [shots=N]" measures a whole
 * calibration (luminance at the given HAL lens values, colour at light K);
 * "shading calibrate light cct=K [shots=N]" adds light K to the existing
 * file. The file goes to device-profiles/lens-shading/. Otherwise the dosing
 * (balance / strength / maxgain). */
static int cmd_shading(char **save, char *out)
{
    static double bal = 50, str = 100, mg = 5;
    char *tok, *first = strtok_r(NULL, " \t\r\n", save);
    if (first && (!strcmp(first, "calibrate"))) {
        char *mode = strtok_r(NULL, " \t\r\n", save);
        double cct = 0;
        int shots = 3, nlens = 0, full;
        float lens[16];
        char pdir[1024], udir[1100];
        if (!mode || (strcmp(mode, "full") && strcmp(mode, "light"))) { emit("{\"ok\":false,\"error\":\"shading calibrate full|light ...\"}"); return 1; }
        full = !strcmp(mode, "full");
        while ((tok = strtok_r(NULL, " \t\r\n", save)) != NULL) {
            if (!strncmp(tok, "cct=", 4)) cct = atof(tok + 4);
            else if (!strncmp(tok, "shots=", 6)) shots = atoi(tok + 6);
            else if (!strncmp(tok, "lens=", 5)) {
                char *p = tok + 5, *e;
                while (nlens < 16 && *p) { double d = strtod(p, &e); if (e == p) break; lens[nlens++] = (float)d; p = (*e == ',') ? e + 1 : e; }
            }
        }
        if (cct < 1000 || cct > 20000) { emit("{\"ok\":false,\"error\":\"shading calibrate: cct=K (1000..20000), the light's colour temperature\"}"); return 1; }
        if (full && nlens < 1) { emit("{\"ok\":false,\"error\":\"shading calibrate full: lens=D1,D2,... (HAL lens values)\"}"); return 1; }
        if (!g_shading_calibrate) { emit("{\"ok\":false,\"error\":\"shading: bridge has no indraw_shading_calibrate\"}"); return 1; }
        if (profile_dir(pdir, sizeof pdir) != 0) { emit("{\"ok\":false,\"error\":\"shading: no profile dir\"}"); return 1; }
        snprintf(udir, sizeof udir, "%s/lens-shading", pdir);
        mkdir(udir, 0755);
        ctl_engine_busy(1);
        {
            int bad = g_shading_calibrate(udir, full, cct, lens, nlens, shots, out, OUT_SIZE);
            ctl_engine_busy(0);
            emit(out);
            return bad ? 1 : 0;
        }
    }
    /* not "calibrate": the dosing. `first` may be the first field. */
    for (tok = first; tok; tok = strtok_r(NULL, " \t\r\n", save)) {
        if (!strncmp(tok, "balance=", 8)) bal = atof(tok + 8);
        else if (!strncmp(tok, "strength=", 9)) str = atof(tok + 9);
        else if (!strncmp(tok, "maxgain=", 8)) mg = atof(tok + 8);
    }
    if (!g_shading_params) { emit("{\"ok\":false,\"error\":\"shading: bridge has no indraw_shading_params\"}"); return 1; }
    if (g_shading_params(bal, str, mg, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* "wb pick": measure the white on the spot (or the middle of what is shown)
 * and fix it; "wb fixed R,GE,GO,B": fix a white measured before (kept across
 * restarts by the interface); "wb fixed off": back to the AWB as the base.
 * The outcome of a pick also goes to the interface as a "wbpick" event. */
static int cmd_wb_fixed(indraw_wb_pick_fn pick, indraw_wb_fixed_fn fixed, const char *arg, char *out)
{
    float gv[4];
    if (!strcmp(arg, "pick")) {
        if (!pick) { emit("{\"ok\":false,\"error\":\"wb: bridge has no indraw_wb_pick\"}"); return 1; }
        if (pick(out, OUT_SIZE)) { emit(out); return 1; }
        emit(out);
        return 0;
    }
    if (!fixed) { emit("{\"ok\":false,\"error\":\"wb: bridge has no indraw_wb_fixed\"}"); return 1; }
    if (!strcmp(arg, "off")) { if (fixed(NULL, out, OUT_SIZE)) { emit(out); return 1; } emit(out); return 0; }
    if (sscanf(arg, "%f,%f,%f,%f", &gv[0], &gv[1], &gv[2], &gv[3]) != 4) { emit("{\"ok\":false,\"error\":\"wb fixed: R,GE,GO,B or off\"}"); return 1; }
    if (fixed(gv, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "viewwin X Y W H" line: the window the interface shows,
 * normalised in the active array, which the histograms cover. */
static int cmd_viewwin(indraw_view_window_fn fn, char **save, char *out)
{
    char *a = strtok_r(NULL, " \t\r\n", save), *b = strtok_r(NULL, " \t\r\n", save);
    char *c = strtok_r(NULL, " \t\r\n", save), *d = strtok_r(NULL, " \t\r\n", save);
    if (!a || !b || !c || !d) { emit("{\"ok\":false,\"error\":\"viewwin: X Y W H\"}"); return 1; }
    if (!fn) { emit("{\"ok\":false,\"error\":\"viewwin: bridge has no indraw_view_window\"}"); return 1; }
    if (fn(atof(a), atof(b), atof(c), atof(d), out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "aelock on|off" line: hold or free the automatic exposure. */
static int cmd_aelock(indraw_ae_lock_fn fn, char **save, char *out)
{
    char *v = strtok_r(NULL, " \t\r\n", save);
    if (!v || (strcmp(v, "on") && strcmp(v, "off"))) { emit("{\"ok\":false,\"error\":\"aelock: on or off\"}"); return 1; }
    if (!fn) { emit("{\"ok\":false,\"error\":\"aelock: bridge has no indraw_ae_lock\"}"); return 1; }
    if (fn(!strcmp(v, "on"), out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* Handles one "flash off|auto|on|torch" line. */
static int cmd_flash(indraw_flash_fn ff, char **save, char *out)
{
    static const name_value_t k_flash[] = { { "off", 0 }, { "auto", 1 }, { "on", 2 }, { "torch", 3 }, { NULL, 0 } };
    char *v = strtok_r(NULL, " \t\r\n", save);
    int mode;
    if (!v || !lookup(k_flash, v, &mode)) { emit("{\"ok\":false,\"error\":\"flash: off, auto, on or torch\"}"); return 1; }
    if (!ff) { emit("{\"ok\":false,\"error\":\"flash: bridge has no indraw_flash\"}"); return 1; }
    if (ff(mode, out, OUT_SIZE)) { emit(out); return 1; }
    emit(out);
    return 0;
}

/* The three engine commands the controller issues itself (exposure, ev,
 * meter), run exactly like a stdin line; the controller calls this under
 * its lock, possibly from its reader thread. Logged with a "c> " prefix so
 * the log tells the controller's commands from the interface's. */
static indraw_preview_exposure_fn g_pe;
static indraw_ev_fn g_evfn;
static indraw_meter_fn g_mt;
static indraw_tap_fn g_tf;
static indraw_focus_fn g_focusfn;
static indraw_zoom_fn g_zf;
static indraw_loupe_fn g_lf;
static indraw_view_window_fn g_vwf;
static void controller_engine_line(const char *text)
{
    static char out[OUT_SIZE];
    char line[256], *save = NULL, *cmd;
    snprintf(line, sizeof line, "%s", text);
    line[strcspn(line, "\r\n")] = '\0';
    log_line("c> ", line);
    cmd = strtok_r(line, " \t", &save);
    if (!cmd) return;
    if (!strcmp(cmd, "exposure")) cmd_exposure(g_pe, &save, out);
    else if (!strcmp(cmd, "ev")) cmd_ev(g_evfn, &save, out);
    else if (!strcmp(cmd, "meter")) cmd_meter(g_mt, &save, out);
    else if (!strcmp(cmd, "tap")) cmd_tap(g_tf, &save, out);
    else if (!strcmp(cmd, "focus")) cmd_focus(g_focusfn, &save, out);
    else if (!strcmp(cmd, "zoom")) cmd_zoom(g_zf, &save, out);
    else if (!strcmp(cmd, "loupe")) cmd_loupe(g_lf, &save, out);
    else if (!strcmp(cmd, "viewwin")) cmd_viewwin(g_vwf, &save, out);
}

int main(int argc, char **argv)
{
    /* If the interface dies between two reads, a camera thread writing a
     * frame or an event would be killed by SIGPIPE before close_all():
     * ignore it, the writes then fail with EPIPE (handled: write_all returns
     * -1, emit_event stops) and the engine closes the camera on its way out. */
    signal(SIGPIPE, SIG_IGN);
    static char out[OUT_SIZE];                 /* JSON from the bridge          */
    const char *lib = getenv("INDRAW_LIB");     /* override for tests            */
    char cmdline[512] = "";
    void *bridge;
    int jw, jh, pw = 1280, ph = 960, rw = 0, rh = 0, rc, i;  /* viewfinder-grade default */

    /* Options before the command, in any order: --log FILE, --jpgdir DIR,
     * --dngdir DIR ("~/" = HOME). A folder that cannot be used is an error
     * now, not a lost photo later. */
    while (argc > 2 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--log")) {
            g_log = fopen(argv[2], "a");
            if (!g_log) { perror(argv[2]); return EXIT_USAGE; }
        } else if (!strcmp(argv[1], "--jpgdir") || !strcmp(argv[1], "--dngdir")) {
            char *dst = argv[1][2] == 'j' ? g_jpgdir : g_dngdir;
            if (expand_home(argv[2], dst, 1024) != 0 || mkdir_p(dst) != 0) {
                fprintf(stderr, "rawfish-ir-cli: cannot use folder %s\n", argv[2]); return EXIT_USAGE;
            }
        } else { usage(); return EXIT_USAGE; }
        argv += 2;
        argc -= 2;
    }
    /* "probe" and "cameras" take no ID; everything else needs at least one. */
    if (argc < 2 || (argc < 3 && strcmp(argv[1], "probe") && strcmp(argv[1], "cameras"))) {
        usage(); return EXIT_USAGE;
    }
    for (i = 1; i < argc; ++i) {               /* record how we were called     */
        size_t len = strlen(cmdline);          /* bytes already in the buffer   */
        size_t room;                           /* free bytes minus the NUL      */
        if (len + 2 >= sizeof cmdline) break;  /* full: stop (no size_t wrap)   */
        room = sizeof cmdline - len - 2;       /* leave room for ' ' + NUL      */
        strncat(cmdline, argv[i], room);       /* append the argument           */
        len = strlen(cmdline);                 /* its new length                */
        cmdline[len] = ' ';                    /* a separating space            */
        cmdline[len + 1] = '\0';               /* keep it a valid string        */
    }
    log_line("$ rawfish-ir-cli ", cmdline);

    if (!lib) lib = "librawfish-ir.so";
    if (!load_hybris()) return EXIT_HYBRIS;

    /* `run` opens the camera: start the binder pool BEFORE loading the bridge. */
    if ((!strcmp(argv[1], "run") || !strcmp(argv[1], "stream")) && !start_binder_pool())
        return EXIT_DROIDMEDIA;   /* the binder pool is needed by BOTH run and stream */

    bridge = a_dlopen(lib, RTLD_NOW);
    if (!bridge) { fprintf(stderr, "rawfish-ir-cli: cannot load %s\n", lib); return EXIT_BRIDGE; }
    {
        indraw_version_fn ver = (indraw_version_fn)a_dlsym(bridge, "indraw_version");
        const char *vs = ver ? ver() : "rawfish-ir (version symbol missing)";
        fprintf(stderr, "rawfish-ir-cli: %s\n", vs);    /* report the loaded library     */
        log_line("# ", vs);
        /* The library and this CLI are built and installed separately; a
         * mismatch used to fail silently (an old library refusing new sizes).
         * Refuse to go on and say so, in the log and on stderr. */
        if (strcmp(vs, "rawfish-ir " IR_VERSION) != 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "version mismatch: CLI %s, library '%s' -- reinstall rawfish-ir-cli",
                     IR_VERSION, vs);
            fprintf(stderr, "rawfish-ir-cli: %s\n", msg);
            log_line("# ", msg);
            return EXIT_BRIDGE;
        }
    }

    if (!strcmp(argv[1], "caps")) {
        indraw_caps_fn caps = (indraw_caps_fn)a_dlsym(bridge, "indraw_caps");
        if (!caps) return EXIT_BRIDGE;
        rc = caps(argv[2], out, sizeof out);
        emit(out);
        return rc ? EXIT_FAILED : 0;
    }

    if (!strcmp(argv[1], "cameras")) {              /* enumerate camera ids       */
        indraw_cameras_fn cams = (indraw_cameras_fn)a_dlsym(bridge, "indraw_cameras");
        if (!cams) return EXIT_BRIDGE;
        rc = cams(out, sizeof out);
        emit(out);
        return rc ? EXIT_FAILED : 0;
    }

    if (!strcmp(argv[1], "streamtest")) {
        /* Which combinations of streams this camera's driver survives: the
         * app's (JPEG max + preview + RAW), then without RAW, then RAW with a
         * small JPEG, then RAW with an 8 MP JPEG. One open each, closed at
         * once; after a failure, the camera service is given up to 10 s to
         * come back. Ends with the override-hal.json lines to use, if any.
         * For a phone where the app shows "engine stopped" (Xperia 10 III). */
        indraw_open_fn op = (indraw_open_fn)a_dlsym(bridge, "indraw_open");
        indraw_close_fn cl = (indraw_close_fn)a_dlsym(bridge, "indraw_close");
        indraw_cameras_fn cams = (indraw_cameras_fn)a_dlsym(bridge, "indraw_cameras");
        typedef int (*lim_fn)(int, int, int);
        lim_fn lf = (lim_fn)a_dlsym(bridge, "indraw_set_stream_limits");
        struct { const char *name; int jw, jh, rw, rh, raw_off, lim_jw, lim_jh; } tests[] = {
            { "jpeg max + preview + raw max (what the app asks)", -1, -1, -1, -1, 0, 0, 0 },
            { "jpeg max + preview, no raw",                        -1, -1,  0,  0, 1, 0, 0 },
            { "jpeg 640x480 + preview + raw max",                 640, 480, -1, -1, 0, 0, 0 },
            { "jpeg within 3264x2448 + preview + raw max",         -1, -1, -1, -1, 0, 3264, 2448 },
        };
        int ok[4] = { 0, 0, 0, 0 }, i, needle_len;
        char needle[40];
        if (argc < 3) { usage(); return EXIT_USAGE; }
        if (!op || !cl || !cams || !lf) return EXIT_BRIDGE;
        needle_len = snprintf(needle, sizeof needle, "\"%s\"", argv[2]);
        (void)needle_len;
        emit("{\"streamtest\":\"start\",\"note\":\"four opens, a few seconds each; nothing is written\"}");
        for (i = 0; i < 4; ++i) {
            int rc;
            lf(tests[i].raw_off, tests[i].lim_jw, tests[i].lim_jh);
            rc = op(argv[2], tests[i].jw, tests[i].jh, -1, -1, tests[i].rw, tests[i].rh, out, sizeof out);
            ok[i] = rc == 0;
            log_line("# streamtest ", tests[i].name);
            emit(out);
            cl(out, sizeof out);
            if (!ok[i]) {                       /* let the camera service come back */
                int t;
                for (t = 0; t < 10; ++t) {
                    sleep(1);
                    if (cams(out, sizeof out) == 0 && strstr(out, needle)) break;
                }
            }
        }
        lf(0, 0, 0);
        if (ok[0])
            snprintf(out, sizeof out, "{\"streamtest\":\"done\",\"camera_id\":\"%s\",\"verdict\":\"the app's streams work on this camera: no override needed\"}", argv[2]);
        else if (ok[1] && (ok[2] || ok[3]))
            snprintf(out, sizeof out, "{\"streamtest\":\"done\",\"camera_id\":\"%s\",\"verdict\":\"RAW fails with the largest JPEG, not with a smaller one\","
                     "\"override\":{\"camera_id\":\"%s\",\"overrides\":{\"jpeg_max\":\"3264x2448\"}}}", argv[2], argv[2]);
        else if (ok[1])
            snprintf(out, sizeof out, "{\"streamtest\":\"done\",\"camera_id\":\"%s\",\"verdict\":\"the RAW stream kills the camera driver; JPEG works\","
                     "\"override\":{\"camera_id\":\"%s\",\"overrides\":{\"raw_stream\":false}}}", argv[2], argv[2]);
        else
            snprintf(out, sizeof out, "{\"streamtest\":\"done\",\"camera_id\":\"%s\",\"verdict\":\"even JPEG + preview fails: not a matter of streams; the log says at which stage\"}", argv[2]);
        emit(out);
        return 0;
    }

    if (!strcmp(argv[1], "probe")) {
        /* Snapshot what the HAL claims into generated-hal.json, read-only
         * reference per the device-profiles design (nature 1). The file lists
         * the cameras and, for each, the full caps JSON. It is NEVER edited by
         * indraw; corrections go in override-hal.json (nature 2). */
        indraw_cameras_fn cams = (indraw_cameras_fn)a_dlsym(bridge, "indraw_cameras");
        indraw_caps_fn caps = (indraw_caps_fn)a_dlsym(bridge, "indraw_caps");
        char dir[1024], file[1100];
        FILE *jf;
        int err, i, ncam = 0;
        char ids[16][16];                          /* up to 16 camera ids        */
        if (!cams || !caps) return EXIT_BRIDGE;
        if (profile_dir(dir, sizeof dir) != 0) {
            emit("{\"ok\":false,\"error\":\"probe: cannot resolve profile dir\"}");
            return EXIT_FAILED;
        }
        if ((err = mkdir_p(dir)) != 0) {
            fprintf(stderr, "rawfish-ir-cli: mkdir %s: %s\n", dir, strerror(err));
            emit("{\"ok\":false,\"error\":\"probe: cannot create profile dir\"}");
            return EXIT_FAILED;
        }
        if (snprintf(file, sizeof file, "%s/generated-hal.json", dir) >= (int)sizeof file)
            return EXIT_FAILED;
        /* Parse the camera ids out of the cameras JSON (simple, quoted strings
         * inside the "cameras":[...] array). */
        if (cams(out, sizeof out)) { emit(out); return EXIT_FAILED; }
        {
            char *pb = strstr(out, "\"cameras\":[");
            if (pb) {
                pb += 11;
                while (*pb && *pb != ']' && ncam < 16) {
                    char *q1 = strchr(pb, '"');
                    char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                    if (!q1 || !q2) break;
                    { size_t l = (size_t)(q2 - q1 - 1);
                      if (l >= sizeof ids[0]) l = sizeof ids[0] - 1;
                      memcpy(ids[ncam], q1 + 1, l); ids[ncam][l] = '\0'; }
                    ++ncam;
                    pb = q2 + 1;
                }
            }
        }
        jf = fopen(file, "w");
        if (!jf) { emit("{\"ok\":false,\"error\":\"probe: cannot write generated-hal.json\"}"); return EXIT_FAILED; }
        fprintf(jf, "{\n  \"generated_by\": \"%s\",\n  \"cameras\": [",
                ((indraw_version_fn)a_dlsym(bridge, "indraw_version"))());
        {
            /* While writing generated-hal.json, keep each camera's HAL values
             * that the override fields correct, to put them next to those
             * fields in the starter below (as "_hal_..." references). */
            long long hal_exp_max[16], hal_frame_min[16];
            int ok_cam[16];
            for (i = 0; i < ncam; ++i) {
                char *e;
                ok_cam[i] = 0; hal_exp_max[i] = -1; hal_frame_min[i] = -1;
                if (caps(ids[i], out, sizeof out)) continue;   /* skip a bad camera   */
                fprintf(jf, "%s\n    %s", i ? "," : "", out);  /* caps is a JSON obj  */
                ok_cam[i] = 1;
                if ((e = strstr(out, "\"exposure_ns_range\":["))) {
                    e = strchr(e, ',');
                    if (e) hal_exp_max[i] = strtoll(e + 1, NULL, 10);
                }
                if ((e = strstr(out, "\"min_frame_ns\":{\"jpeg\":"))) hal_frame_min[i] = strtoll(e + 23, NULL, 10);
            }
            fprintf(jf, "\n  ]\n}\n");
            if (fclose(jf) != 0) { emit("{\"ok\":false,\"error\":\"probe: write failed\"}"); return EXIT_FAILED; }
            /* A starter override-hal.json, only if there is none: one block per
             * camera, every field the engine reads with its built-in default,
             * and the HAL's own value alongside ("_hal_...", never read) so the
             * correction is made knowingly. The engine checks the file at each
             * open and logs unknown keys and out-of-range values. */
            {
                char ov[1100];
                FILE *of;
                int first = 1;
                snprintf(ov, sizeof ov, "%s/override-hal.json", dir);
                if (access(ov, F_OK) != 0 && (of = fopen(ov, "w"))) {
                    fprintf(of, "{\n  \"_comment\": \"Measured corrections to what the HAL claims (docs/DEVICE-PROFILES.md). "
                                "Values are the built-in defaults; _hal_ fields are the HAL's claims, for reference only. "
                                "Delete a camera block or a field to use the defaults.\",\n  \"cameras\": [");
                    for (i = 0; i < ncam; ++i) {
                        if (!ok_cam[i]) continue;
                        fprintf(of, "%s\n    {\n      \"camera_id\": \"%s\",\n      \"overrides\": {\n"
                                    "        \"_hal_exposure_max_ns\": %lld,\n"
                                    "        \"observed_shutter_max_ns\": 500000000,\n"
                                    "        \"_hal_min_frame_ns\": %lld,\n"
                                    "        \"min_frame_ns\": 33333333,\n"
                                    "        \"peak_edge\": 40,\n        \"peak_width\": 2,\n"
                                    "        \"zebra_jpeg\": 250,\n        \"zebra_raw\": 992\n"
                                    "      },\n      \"focus_calibration\": []\n    }",
                                first ? "" : ",", ids[i], hal_exp_max[i], hal_frame_min[i]);
                        first = 0;
                    }
                    fprintf(of, "\n  ]\n}\n");
                    fclose(of);
                    log_line("# ", "probe: wrote a starter override-hal.json, one block per camera (edit it; the Focus cal. menu fills focus-calibration.json)");
                }
            }
        }
        snprintf(out, sizeof out, "{\"ok\":true,\"path\":\"%s\",\"cameras\":%d}", file, ncam);
        emit(out);
        return 0;
    }

    if (!strcmp(argv[1], "run") || !strcmp(argv[1], "stream")) {
        int streaming = !strcmp(argv[1], "stream");  /* stdout = frames if so    */
        indraw_open_fn op = (indraw_open_fn)a_dlsym(bridge, "indraw_open");
        indraw_jpeg_fn jpeg = (indraw_jpeg_fn)a_dlsym(bridge, "indraw_jpeg");
        indraw_close_fn cl = (indraw_close_fn)a_dlsym(bridge, "indraw_close");
        indraw_raw_fn raw = (indraw_raw_fn)a_dlsym(bridge, "indraw_raw");
        indraw_stack_fn stack = (indraw_stack_fn)a_dlsym(bridge, "indraw_stack");
        indraw_noiseprofile_fn np = (indraw_noiseprofile_fn)a_dlsym(bridge, "indraw_noiseprofile");
        indraw_focus_fn focusfn = (indraw_focus_fn)a_dlsym(bridge, "indraw_focus");
        indraw_preview_frame_fn pf = (indraw_preview_frame_fn)a_dlsym(bridge, "indraw_preview_frame");
        indraw_preview_settings_fn ps = (indraw_preview_settings_fn)a_dlsym(bridge, "indraw_preview_settings");
        indraw_preview_exposure_fn pe = (indraw_preview_exposure_fn)a_dlsym(bridge, "indraw_preview_exposure");
        indraw_ev_fn evfn = (indraw_ev_fn)a_dlsym(bridge, "indraw_ev");
        indraw_meter_fn mt = (indraw_meter_fn)a_dlsym(bridge, "indraw_meter");
        indraw_zoom_fn zf = (indraw_zoom_fn)a_dlsym(bridge, "indraw_zoom");
        indraw_tap_fn tf = (indraw_tap_fn)a_dlsym(bridge, "indraw_tap");
        indraw_wb_fn wf = (indraw_wb_fn)a_dlsym(bridge, "indraw_wb");
        indraw_scene_fn sf = (indraw_scene_fn)a_dlsym(bridge, "indraw_scene");
        indraw_wb_manual_fn wm = (indraw_wb_manual_fn)a_dlsym(bridge, "indraw_wb_manual");
        indraw_set_af_wait_fn aw = (indraw_set_af_wait_fn)a_dlsym(bridge, "indraw_set_af_wait");
        indraw_lens_position_fn lp = (indraw_lens_position_fn)a_dlsym(bridge, "indraw_lens_position");
        indraw_burst_fn bf = (indraw_burst_fn)a_dlsym(bridge, "indraw_burst");
        indraw_flash_fn ff = (indraw_flash_fn)a_dlsym(bridge, "indraw_flash");
        indraw_ae_lock_fn al = (indraw_ae_lock_fn)a_dlsym(bridge, "indraw_ae_lock");
        indraw_orient_fn orf = (indraw_orient_fn)a_dlsym(bridge, "indraw_orient");
        indraw_view_window_fn vwf = (indraw_view_window_fn)a_dlsym(bridge, "indraw_view_window");
        indraw_wb_pick_fn wbp = (indraw_wb_pick_fn)a_dlsym(bridge, "indraw_wb_pick");
        indraw_wb_fixed_fn wbx = (indraw_wb_fixed_fn)a_dlsym(bridge, "indraw_wb_fixed");
        indraw_loupe_fn lf = (indraw_loupe_fn)a_dlsym(bridge, "indraw_loupe");
        indraw_set_focus_cal_fn setcal_cmd = (indraw_set_focus_cal_fn)a_dlsym(bridge, "indraw_set_focus_calibration");
        {   /* abort paths: SIGUSR1 from the app, stdin hang-up (app gone) */
            struct sigaction sa;
            pthread_t t;
            g_abort_fn = (indraw_request_abort_fn)a_dlsym(bridge, "indraw_request_abort");
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = on_sigusr1;
            sigaction(SIGUSR1, &sa, NULL);
            if (g_abort_fn && pthread_create(&t, NULL, eof_watch, NULL) == 0) pthread_detach(t);
        }
        char *line = NULL;                      /* getline() buffer              */
        size_t cap = 0;
        int failures = 0;

        if (!op || !jpeg || !cl) return EXIT_BRIDGE;
        if (argc < 4 || !parse_size(argv[3], &jw, &jh)) { usage(); return EXIT_USAGE; }
        for (i = 4; i < argc; ++i) {            /* [PREVIEW_WxH] [raw=WxH]       */
            if (!strncmp(argv[i], "raw=", 4)) {
                if (!parse_size(argv[i] + 4, &rw, &rh)) { usage(); return EXIT_USAGE; }
            } else if (!parse_size(argv[i], &pw, &ph)) { usage(); return EXIT_USAGE; }
        }

        /* In stream mode, stdout is the binary frame channel from the very start:
         * route ALL JSON (open report, overrides, errors) to the log only, so
         * stdout contains nothing but SF2P frames. */
        if (streaming) g_streaming = 1;

        /* The shipped device profile for this phone, and the user's override
         * checked, BEFORE open: some corrections apply to the session's setup. */
        find_shipped_profile(bridge, argv[2]);
        g_ov_user_ok = check_override(argv[2]);
        apply_session_requests(bridge, argv[2]);   /* session parameters, before open */
        apply_stream_limits(bridge, argv[2]);      /* raw_stream / jpeg_max, before open */
        {   /* device-profile switch, read BEFORE open: the photo/RAW template */
            typedef int (*set_tpl_fn)(int);
            set_tpl_fn stp = (set_tpl_fn)a_dlsym(bridge, "indraw_set_still_template");
            int tpl = read_override_still_template(argv[2]);
            if (stp) stp(tpl);
            if (tpl) log_line("# still_template: ", "preview (override)");
        }
        rc = op(argv[2], jw, jh, pw, ph, rw, rh, out, sizeof out);
        emit(out);
        if (rc) return EXIT_FAILED;

        /* Apply device corrections (nature 2): read override-hal.json for this
         * camera and pass the typed values to the bridge. Today: the real
         * single-exposure ceiling, which replaces the built-in stacking max. */
        {
            indraw_set_override_fn setov =
                (indraw_set_override_fn)a_dlsym(bridge, "indraw_set_override");
            /* The override file is checked first; a broken one is not used. */
            int ov_ok = 1;                         /* each reader consults both sources */
            long long shutter_max = ov_ok ? read_override_checked(argv[2], "observed_shutter_max_ns") : 0;
            long long min_frame = ov_ok ? read_override_checked(argv[2], "min_frame_ns") : 0;
            if (setov && (shutter_max > 0 || min_frame > 0)) {
                setov((int64_t)shutter_max, (int64_t)min_frame, out, sizeof out);
                emit(out);                         /* report what was applied      */
            }
            {   /* a pause before each capture (device profile; absent: none) */
                typedef int (*set_delay_fn)(int);
                set_delay_fn sd = (set_delay_fn)a_dlsym(bridge, "indraw_set_capture_delay");
                long long cd = ov_ok ? read_override_checked(argv[2], "capture_delay_ms") : 0;
                if (sd && cd > 0) {
                    char m[64];
                    sd((int)cd);
                    snprintf(m, sizeof m, "%lld ms (override)", cd);
                    log_line("# capture_delay: ", m);
                }
            }
            /* Preview-aid thresholds (peaking/zebra), also tunable per device. */
            {
                indraw_set_aids_override_fn setaids =
                    (indraw_set_aids_override_fn)a_dlsym(bridge, "indraw_set_aids_override");
                long long pe = ov_ok ? read_override_checked(argv[2], "peak_edge") : 0;
                long long zj = ov_ok ? read_override_checked(argv[2], "zebra_jpeg") : 0;
                long long zr = ov_ok ? read_override_checked(argv[2], "zebra_raw") : 0;
                long long pw = ov_ok ? read_override_checked(argv[2], "peak_width") : 0;
                if (setaids && (pe > 0 || zj > 0 || zr > 0 || pw > 0)) {
                    setaids((int64_t)pe, (int64_t)zj, (int64_t)zr, (int64_t)pw, out, sizeof out);
                    emit(out);
                }
            }
            /* The measured lens-shading calibration for this phone and camera
             * (shading_<maker>-<model>_cam<ID>_<W>x<H>.json): the user's
             * device-profiles/lens-shading/ first, then the one shipped with
             * the engine in /usr/share/rawfish-ir (visible in the app's sandbox);
             * unless the override says "hal" or "none". */
            {
                indraw_shading_load_fn sl = (indraw_shading_load_fn)a_dlsym(bridge, "indraw_shading_load");
                char pdir[1024], udir[1100];
                g_shading_params = (indraw_shading_params_fn)a_dlsym(bridge, "indraw_shading_params");
                g_shading_calibrate = (indraw_shading_calibrate_fn)a_dlsym(bridge, "indraw_shading_calibrate");
                if (sl && profile_dir(pdir, sizeof pdir) == 0) {
                    snprintf(udir, sizeof udir, "%s/lens-shading", pdir);
                    mkdir(udir, 0755);                   /* where to drop a calibration */
                    {   /* the calibration file name the device profile gives, if any */
                        typedef int (*sname_fn)(const char *);
                        sname_fn sn = (sname_fn)a_dlsym(bridge, "indraw_shading_name");
                        char nm[200] = "";
                        ov_string(argv[2], "shading_file", nm, sizeof nm);
                        if (sn) sn(nm);
                    }
                    sl(udir, "/usr/share/rawfish-ir/lens-shading", read_override_shading(argv[2]), out, sizeof out);
                    log_line("# shading: ", out);
                }
            }
            {   /* read-only: what the preview / photo / RAW requests carry
                 * (AF mode, intent, vendor keys with type and value) */
                typedef int (*tpl_rep_fn)(char *, size_t);
                tpl_rep_fn tr = (tpl_rep_fn)a_dlsym(bridge, "indraw_template_report");
                if (tr && tr(out, sizeof out) == 0) log_line("# templates: ", out);
            }
            /* Vendor (chipset) request keys the device profile asks for. */
            g_bridge_for_vendor = bridge;
            if (ov_ok) apply_vendor_requests((indraw_vendor_set_fn)a_dlsym(bridge, "indraw_vendor_set"), argv[2], out);
            /* Focus calibration table (maps real metres -> HAL diopters). */
            {
                indraw_set_focus_cal_fn setcal =
                    (indraw_set_focus_cal_fn)a_dlsym(bridge, "indraw_set_focus_calibration");
                float real_d[16], hal_d[16];
                int ncal = read_focus_calibration(argv[2], real_d, hal_d, 16);
                if (setcal && ncal > 0) {
                    setcal(real_d, hal_d, ncal, out, sizeof out);
                    emit(out);
                } else {
                    /* No table: manual focus distances use the uncalibrated 1:1
                     * mapping. Warn in the log so test results are interpreted
                     * correctly (the device reports focus as uncalibrated). */
                    log_line("# ", "focus: no calibration table; manual distances are uncalibrated (1:1)");
                }
            }
        }

        /* In stream mode, stdout becomes the binary SF2P frame channel: route all
         * JSON (reports, errors) to the log only, and start streaming to fd 1. */
        if (streaming) {
            indraw_stream_start_fn sstart =
                (indraw_stream_start_fn)a_dlsym(bridge, "indraw_stream_start");
            /* The controller (docs/ARCHITECTURE.md) takes the engine's state
             * events through a pipe and forwards what the interface sees;
             * started before the stream so the first events (caps) go through
             * it too. Without it (older library) the engine streams them itself. */
            g_pe = pe; g_evfn = evfn; g_mt = mt; g_tf = tf; g_focusfn = focusfn; g_zf = zf; g_lf = lf; g_vwf = vwf;
            g_focus_opt = (indraw_focus_opt_fn)a_dlsym(bridge, "indraw_focus_opt");
            ctl_start((indraw_emit_fn)a_dlsym(bridge, "indraw_emit"),
                      (indraw_set_event_fd_fn)a_dlsym(bridge, "indraw_set_event_fd"),
                      controller_engine_line);
            if (sstart) { sstart(1, out, sizeof out); emit(out); }  /* fd 1 = stdout */
        } else {
            /* run mode: no controller, so send the engine's events (progress,
             * diagnostics) to stdout, where they appear in the terminal. */
            indraw_set_event_fd_fn sef = (indraw_set_event_fd_fn)a_dlsym(bridge, "indraw_set_event_fd");
            indraw_set_state_events_fn sse = (indraw_set_state_events_fn)a_dlsym(bridge, "indraw_set_state_events");
            if (sef) sef(1);                     /* events to stdout */
            if (sse) sse(0);                     /* but not the continuous "ae" line */
        }

        /* Command loop: the camera stays open (and the preview running) between photos. */
        while (getline(&line, &cap, stdin) > 0) {
            char *save = NULL, *cmd;
            line[strcspn(line, "\r\n")] = '\0'; /* drop the newline for the log  */
            log_line("", "### ----------------------------------------"); /* separate captures */
            log_line("> ", line);
            if (!strncmp(line, "xp ", 3) || !strncmp(line, "fc ", 3) || !strncmp(line, "vw ", 3) ||
                !strncmp(line, "sh ", 3)) {       /* the controller's own commands */
                char cap[256];
                ctl_lock(); ctl_command(line); ctl_unlock();
                /* a shutter press: the capture lines the controller decided, run
                 * here like typed ones (long: without the lock) */
                while (ctl_pop_capture(cap, sizeof cap)) {
                    char *csave = NULL, *ccmd;
                    log_line("c> ", cap);
                    ccmd = strtok_r(cap, " \t", &csave);
                    if (!ccmd) continue;
                    ctl_engine_busy(1);
                    if (!strcmp(ccmd, "jpeg")) failures += cmd_jpeg(jpeg, &csave, out);
                    else if (!strcmp(ccmd, "raw")) failures += cmd_raw(raw, &csave, out);
                    else if (!strcmp(ccmd, "stack")) failures += cmd_stack(stack, &csave, out);
                    else if (!strcmp(ccmd, "burst")) failures += cmd_burst(bf, &csave, out);
                    ctl_engine_busy(0);
                }
                continue;
            }
            cmd = strtok_r(line, " \t", &save); /* first word                    */
            if (!cmd) continue;                 /* empty line                    */
            if (cmd[0] == '#') continue;        /* comment: logged above, ignored (UI diagnostics) */
            if (!strcmp(cmd, "quit")) break;
            /* Long commands (a photo, a burst, a stack) run with the lock
             * released and the controller told: its events keep flowing and
             * its own engine commands are deferred until the capture is over
             * (held, the lock would have stalled the stack's progress events,
             * then the engine's event writes). Short ones run under the lock:
             * one engine caller at a time. */
            /* A white-balance pick or restore waits for a RAW frame or the
             * AWB's first result (up to 2.5 s): run like a capture, without
             * the controller's lock, so the events keep flowing. */
            if (!strcmp(cmd, "shading") && save && !strncmp(save, "calibrate", 9)) {
                failures += cmd_shading(&save, out);   /* drives captures; ctl_engine_busy inside */
                continue;
            }
            if (!strcmp(cmd, "wb") && save && (!strncmp(save, "pick", 4) || !strncmp(save, "fixed", 5))) {
                char *a1 = strtok_r(NULL, " \t\r\n", &save), *a2 = strtok_r(NULL, " \t\r\n", &save);
                ctl_engine_busy(1);
                failures += cmd_wb_fixed(wbp, wbx, !strcmp(a1, "pick") ? "pick" : (a2 ? a2 : ""), out);
                ctl_engine_busy(0);
                continue;
            }
            if (!strcmp(cmd, "jpeg") || !strcmp(cmd, "raw") || !strcmp(cmd, "stack") ||
                !strcmp(cmd, "noiseprofile") || !strcmp(cmd, "burst")) {
                ctl_engine_busy(1);
                if (!strcmp(cmd, "jpeg")) failures += cmd_jpeg(jpeg, &save, out);
                else if (!strcmp(cmd, "raw")) failures += cmd_raw(raw, &save, out);
                else if (!strcmp(cmd, "stack")) failures += cmd_stack(stack, &save, out);
                else if (!strcmp(cmd, "noiseprofile")) failures += cmd_noiseprofile(np, &save, out);
                else failures += cmd_burst(bf, &save, out);
                ctl_engine_busy(0);
                continue;
            }
            ctl_lock();                         /* one engine caller at a time   */
            if (!strcmp(cmd, "focus")) failures += cmd_focus(focusfn, &save, out);
            else if (!strcmp(cmd, "preview-frame")) failures += cmd_preview_frame(pf, &save, out);
            else if (!strcmp(cmd, "preview")) failures += cmd_preview(ps, &save, out);
            else if (!strcmp(cmd, "exposure")) failures += cmd_exposure(pe, &save, out);
            else if (!strcmp(cmd, "ev")) failures += cmd_ev(evfn, &save, out);
            else if (!strcmp(cmd, "meter")) failures += cmd_meter(mt, &save, out);
            else if (!strcmp(cmd, "zoom")) failures += cmd_zoom(zf, &save, out);
            else if (!strcmp(cmd, "tap")) failures += cmd_tap(tf, &save, out);
            else if (!strcmp(cmd, "wb")) failures += cmd_wb(wf, wm, &save, out);
            else if (!strcmp(cmd, "scene")) failures += cmd_scene(sf, &save, out);
            else if (!strcmp(cmd, "afwait")) failures += cmd_afwait(aw, &save, out);
            else if (!strcmp(cmd, "calib")) failures += cmd_calib(lp, setcal_cmd, argv[2], &save, out);
            else if (!strcmp(cmd, "flash")) failures += cmd_flash(ff, &save, out);
            else if (!strcmp(cmd, "aelock")) failures += cmd_aelock(al, &save, out);
            else if (!strcmp(cmd, "orient")) failures += cmd_orient(orf, &save, out);
            else if (!strcmp(cmd, "viewwin")) failures += cmd_viewwin(vwf, &save, out);
            else if (!strcmp(cmd, "loupe")) failures += cmd_loupe(lf, &save, out);
            else if (!strcmp(cmd, "shading")) failures += cmd_shading(&save, out);
            else emit("{\"ok\":false,\"error\":\"unknown command\"}");
            ctl_unlock();
        }
        free(line);
        cl(out, sizeof out);                    /* always release the camera     */
        emit(out);
        ctl_stop();                             /* after close: the last events are through */
        return failures ? EXIT_FAILED : 0;
    }

    usage();
    return EXIT_USAGE;
}
