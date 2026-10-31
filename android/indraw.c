/*
 * indraw.c -- Android side of indraw (built with the NDK, loaded through libhybris).
 *
 * This library owns ONE camera session that stays open between photos:
 *   - a small YUV preview stream runs continuously (repeating request), so
 *     auto-exposure, auto-white-balance and autofocus keep converging;
 *   - a JPEG stream is attached to the same session, so a photo is a single
 *     capture request on an already running pipeline ("warm" capture).
 *
 * Every exported function writes a JSON object into a caller-provided buffer
 * and returns 0 on success, or one of the RF_E* codes below on failure.
 * No callback ever crosses the libhybris boundary: the Sailfish side only
 * calls functions and reads the JSON they produce.
 *
 * Threads and locks (the contract; scripts/lockcheck.awk checks the parts a
 * line-by-line reading can, docs/ARCHITECTURE.md "Threads and locks"):
 *   - the caller's thread: the exported functions (the CLI's commands, one at
 *     a time under the controller's lock);
 *   - Camera2's threads: the image listeners (on_*_image) and the capture
 *     callbacks (on_preview_result, on_capture_completed/failed,
 *     on_meter_result/failed), which the HAL calls on threads of its own;
 *   - meter_main: the RAW exposure meter's thread, when the meter is on.
 * Two mutexes. g.lock guards the session state in g (everything but the
 * output descriptors); it is NOT recursive: never taken twice on one path.
 * g.out_lock guards the output: g.stream_fd, g.event_fd, the order of the
 * SF2P frames and SF2M lines on the stream, events_dropped. The only
 * nesting order is g.lock -> out_lock (emit_event may be called with g.lock
 * held); never out_lock -> g.lock. No callback blocks on the controller:
 * events go into a non-blocking pipe (state lines dropped when it is full,
 * one-off events wait at most 300 ms). ThreadSanitizer cannot run this file
 * (it needs the HAL); the controller, which can, runs under TSan and
 * Helgrind in check-host.sh.
 */

#include <camera/NdkCameraCaptureSession.h> /* ACameraCaptureSession_*          */
#include <camera/NdkCameraDevice.h>         /* ACameraDevice_*                  */
#include <camera/NdkCameraManager.h>        /* ACameraManager_*                 */
#include <camera/NdkCameraMetadata.h>       /* ACameraMetadata_getConstEntry    */
#include <camera/NdkCameraMetadataTags.h>   /* ACAMERA_* tag numbers and values */
#include <camera/NdkCaptureRequest.h>       /* ACaptureRequest_*                */

#include "dngwrite.h"
#include "exifwrite.h"  /* our Exif APP1 for the HAL's JPEG       */
#include <sys/system_properties.h> /* __system_property_get: ro.product.* (make, model) */
#include <media/NdkImage.h>                 /* AImage_*                         */
#include <media/NdkImageReader.h>           /* AImageReader_*                   */

#include <errno.h>    /* errno after a failed write/rename        */
#include <fcntl.h>    /* open() flags                             */
#include <pthread.h>  /* mutex + condition variable               */
#include <dlfcn.h>    /* dlsym: NDK functions newer than the base   */
#include <poll.h>     /* waiting for room on the event pipe       */
#include <signal.h>   /* sig_atomic_t for the abort flag          */
#include <sys/stat.h> /* mkdir for the previews folder             */
#include <stdarg.h>   /* va_list for the JSON writer              */
#include <stdint.h>   /* fixed-size integer types                 */
#include <stdio.h>    /* vsnprintf, snprintf, FILE (RAW sidecar)  */
#include <stdlib.h>   /* calloc, free (stacking accumulator)      */
#include <string.h>   /* memset, strerror                         */
#include <time.h>     /* clock_gettime                            */
#include <math.h>     /* isfinite: never emit nan/inf in JSON     */
#include <unistd.h>   /* write, close, unlink                     */

/* The exported entry points. On 32-bit ARM, Android (armeabi-v7a) passes
 * float / double arguments in the core registers ("softfp") while the caller,
 * the indraw CLI built for Sailfish armv7hl, passes them in the VFP registers
 * ("hard-float"): every exported function taking or returning a float read
 * its arguments from the wrong place (on the Xperia XA2 "focus continuous"
 * took the buffer's size for its address: SIGSEGV). The entry points are
 * therefore declared with the hard-float convention there; the library's
 * inside and its calls into Android keep the platform's. 64-bit ARM has one
 * convention only: unchanged. */
#if defined(__arm__) && !defined(__aarch64__)
#define IR_API __attribute__((visibility("default"), pcs("aapcs-vfp")))
#else
#define IR_API __attribute__((visibility("default"))) /* exported symbol */
#endif
#include "../indraw_version.h"   /* IR_VERSION, shared with the CLI */
#include "colortemp.h"           /* the white balance as a colour temperature */
#include "shading.h"             /* the measured lens-shading calibration     */
#include "shading_build.h"       /* building one from flat-field captures     */
#include "yuv.h"                 /* the preview's YUV -> RGB conversion       */

static int g_screen_notch = -1;   /* the screen's top cutout, from the device profile (-1 unknown) */

/* Return codes shared by every exported function. */
enum {
    IR_OK = 0,
    IR_EARG = 1,      /* invalid argument (size, quality, mode...)     */
    IR_ESTATE = 2,    /* camera not open / already open                */
    IR_ESETUP = 3,    /* a Camera2 or ImageReader setup call failed    */
    IR_ECAPTURE = 4,  /* capture failed, device error or timeout       */
    IR_EFILE = 5,     /* the JPEG could not be written                 */
};

/* Capture variants (bit flags), to find what lets long exposures through.
 * Each one is set or restored on every capture, so none of them sticks. */
/* Focus modes, kept close to Camera2 (the UI renames for display). */
enum {
    IR_FOCUS_KEEP = 0,      /* leave focus as the request has it (default = auto) */
    IR_FOCUS_AUTO,          /* AF_MODE_AUTO: one scan, then hold                   */
    IR_FOCUS_CONTINUOUS,    /* AF_MODE_CONTINUOUS_PICTURE                          */
    IR_FOCUS_CONTINUOUS_VIDEO, /* AF_MODE_CONTINUOUS_VIDEO                         */
    IR_FOCUS_MACRO,         /* AF_MODE_MACRO                                       */
    IR_FOCUS_EDOF,          /* AF_MODE_EDOF (extended depth of field)              */
    IR_FOCUS_MANUAL,        /* AF_MODE_OFF + LENS_FOCUS_DISTANCE (real distance,
                             * through the focus calibration)                     */
    IR_FOCUS_HOLD,          /* AF_MODE_OFF at the lens position the HAL reports now */
    IR_FOCUS_RAW,           /* AF_MODE_OFF + LENS_FOCUS_DISTANCE set to the HAL's
                             * OWN value, no focus calibration (the lens-shading
                             * assistant walks lens positions this way)           */
};

enum {
    IR_V_PAUSE = 1,        /* stop the repeating preview during the capture      */
    IR_V_FPS_LOW = 2,      /* AE target FPS range = the lowest one listed        */
    IR_V_CONTROL_OFF = 4,  /* CONTROL_MODE OFF: no 3A at all for that request    */
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Fills `buf` with the current local wall-clock time as an EXIF datetime
 * string, "YYYY:MM:DD HH:MM:SS". Uses the system clock (not the sensor's
 * monotonic timestamp, which is not calendar time). buf must hold >= 20 bytes. */
static void exif_datetime(char *buf, size_t n)
{
    time_t t = time(NULL);                /* calendar time now                 */
    struct tm tm;
    localtime_r(&t, &tm);                 /* broken down, local zone           */
    strftime(buf, n, "%Y:%m:%d %H:%M:%S", &tm); /* EXIF format                 */
}

/* Monotonic clock in milliseconds (never jumps with wall-clock changes). */
static double now_ms(void)
{
    struct timespec ts;                                  /* seconds + nanoseconds  */
    clock_gettime(CLOCK_MONOTONIC, &ts);                 /* read monotonic clock   */
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;  /* convert to ms          */
}

/* Absolute deadline "now + ms" on CLOCK_MONOTONIC, for pthread_cond_timedwait. */
static struct timespec deadline_in(int ms)
{
    struct timespec ts;                        /* result                            */
    clock_gettime(CLOCK_MONOTONIC, &ts);       /* start from now                    */
    ts.tv_sec += ms / 1000;                    /* add whole seconds                 */
    ts.tv_nsec += (long)(ms % 1000) * 1000000; /* add the remaining milliseconds    */
    if (ts.tv_nsec >= 1000000000L) {           /* carry nanoseconds into seconds    */
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* JSON writer into a fixed buffer: never overflows, remembers truncation. */
typedef struct {
    char *buf;      /* caller's buffer              */
    size_t size;    /* its capacity                 */
    size_t limit;   /* usable size (= size - tail reserve) */
    size_t len;     /* bytes written so far         */
    int truncated;  /* set once the buffer overflowed */
} jw_t;

#define JW_TAIL 24             /* bytes kept free for a closing marker */
static void jw_init(jw_t *w, char *buf, size_t size)
{
    w->buf = buf;              /* where to write                     */
    w->size = size;            /* total capacity                     */
    w->limit = size > JW_TAIL ? size - JW_TAIL : size; /* reserve a tail */
    w->len = 0;                /* nothing written yet                */
    w->truncated = 0;          /* nothing dropped yet                */
    if (buf && size) buf[0] = '\0'; /* start with an empty string    */
}

/* printf-style append; silently truncates when the buffer is full. */
static void jw_add(jw_t *w, const char *fmt, ...)
{
    va_list ap;                                       /* the "..." arguments          */
    int n;                                            /* chars vsnprintf wanted       */
    if (!w->buf || w->len + 1 >= w->limit) { w->truncated = 1; return; } /* no room: drop */
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->len, w->limit - w->len, fmt, ap); /* append             */
    va_end(ap);
    if (n < 0) return;                                /* formatting error: ignore     */
    if ((size_t)n >= w->limit - w->len) { w->len = w->limit - 1; w->truncated = 1; } /* full */
    else w->len += (size_t)n;                         /* normal case                  */
}

/* Appends a JSON string VALUE (with surrounding quotes), escaping the
 * characters JSON requires, so a path with a quote, backslash or control
 * character can never break the report. */
static void jw_str(jw_t *w, const char *s)
{
    jw_add(w, "\"");                      /* opening quote                     */
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') jw_add(w, "\\%c", c);  /* escape " and \   */
        else if (c == '\n') jw_add(w, "\\n");               /* newline          */
        else if (c == '\r') jw_add(w, "\\r");               /* carriage return  */
        else if (c == '\t') jw_add(w, "\\t");               /* tab              */
        else if (c < 0x20) jw_add(w, "\\u%04x", c);          /* other controls   */
        else jw_add(w, "%c", c);                              /* plain byte       */
    }
    jw_add(w, "\"");                      /* closing quote                     */
}

/* Appends a closing ,"truncated":true/false} using the reserved tail, so the
 * flag is always present even when the body filled the buffer. */
static void jw_finish(jw_t *w)
{
    const char *tail = ",\"truncated\":false}";
    size_t need = strlen(tail);
    if (!w->buf || w->size < 96) return;
    if (w->truncated) {
        /* A body cut anywhere (inside a string, a number...) is not JSON
         * whatever flag follows it: replace it by a valid error reply that
         * says so (0.47.62). The caller's buffer is what it is; a caps on a
         * very talkative HAL would hit this. */
        snprintf(w->buf, w->size, "{\"ok\":false,\"stage\":\"json\",\"error\":\"reply too long for the %zu-byte buffer\",\"status\":0,\"truncated\":true}", w->size);
        w->len = strlen(w->buf);
        return;
    }
    if (w->len > w->size - need - 1) w->len = w->size - need - 1; /* make room    */
    memcpy(w->buf + w->len, tail, need + 1);          /* includes the NUL         */
    w->len += need;
}

/* Writes {"ok":false,"stage":...,"error":...,"status":N} and returns code. */
static int fail(char *out, size_t out_size, int code, const char *stage,
                const char *error, int status)
{
    jw_t w;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":false,\"stage\":\"%s\",\"error\":", stage);
    jw_str(&w, error);                       /* quoted: a path or a quote in the text stays valid JSON */
    jw_add(&w, ",\"status\":%d}", status);
    return code;
}

/* Name <-> value table for noise-reduction modes (used on both directions). */
static const struct { const char *name; int value; } k_nr_modes[] = {
    { "off_noise", ACAMERA_NOISE_REDUCTION_MODE_OFF },
    { "fast_noise", ACAMERA_NOISE_REDUCTION_MODE_FAST },
    { "hq_noise", ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY },
    { "minimal_noise", ACAMERA_NOISE_REDUCTION_MODE_MINIMAL },
    { "zsl_noise", ACAMERA_NOISE_REDUCTION_MODE_ZERO_SHUTTER_LAG },
};

/* Edge (sharpening) mode value -> name. Values: OFF 0, FAST 1, HIGH_QUALITY 2, ZSL 3. */
static const char *edge_name(int value)
{
    switch (value) {
    case ACAMERA_EDGE_MODE_OFF:              return "off_edge";
    case ACAMERA_EDGE_MODE_FAST:             return "fast_edge";
    case ACAMERA_EDGE_MODE_HIGH_QUALITY:     return "hq_edge";
    case ACAMERA_EDGE_MODE_ZERO_SHUTTER_LAG: return "zsl_edge";
    default:                                 return "unknown";
    }
}

/* Value -> short name (or "unknown"). */
static const char *nr_name(int value)
{
    size_t i;
    for (i = 0; i < sizeof k_nr_modes / sizeof k_nr_modes[0]; ++i)
        if (k_nr_modes[i].value == value) return k_nr_modes[i].name;
    return "unknown";
}

/* Scene-mode value -> name. Camera2 defines these; what each DOES is left to
 * the vendor, so we only list what the device declares (no interpretation). */
static const char *scene_name(int v)
{
    switch (v) {
    case ACAMERA_CONTROL_SCENE_MODE_DISABLED:       return "disabled";
    case ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY:  return "face_priority";
    case ACAMERA_CONTROL_SCENE_MODE_ACTION:         return "action";
    case ACAMERA_CONTROL_SCENE_MODE_PORTRAIT:       return "portrait";
    case ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE:      return "landscape";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT:          return "night";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT: return "night_portrait";
    case ACAMERA_CONTROL_SCENE_MODE_THEATRE:        return "theatre";
    case ACAMERA_CONTROL_SCENE_MODE_BEACH:          return "beach";
    case ACAMERA_CONTROL_SCENE_MODE_SNOW:           return "snow";
    case ACAMERA_CONTROL_SCENE_MODE_SUNSET:         return "sunset";
    case ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO:    return "steadyphoto";
    case ACAMERA_CONTROL_SCENE_MODE_FIREWORKS:      return "fireworks";
    case ACAMERA_CONTROL_SCENE_MODE_SPORTS:         return "sports";
    case ACAMERA_CONTROL_SCENE_MODE_PARTY:          return "party";
    case ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT:    return "candlelight";
    case ACAMERA_CONTROL_SCENE_MODE_BARCODE:        return "barcode";
    case ACAMERA_CONTROL_SCENE_MODE_HDR:            return "hdr";
    default:                                        return "unknown";
    }
}

/* Effect-mode value -> name (colour effects; cosmetic, vendor-rendered). */
static const char *effect_name(int v)
{
    switch (v) {
    case ACAMERA_CONTROL_EFFECT_MODE_OFF:        return "off";
    case ACAMERA_CONTROL_EFFECT_MODE_MONO:       return "mono";
    case ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE:   return "negative";
    case ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE:   return "solarize";
    case ACAMERA_CONTROL_EFFECT_MODE_SEPIA:      return "sepia";
    case ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE:  return "posterize";
    case ACAMERA_CONTROL_EFFECT_MODE_AQUA:       return "aqua";
    case ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD: return "blackboard";
    case ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD: return "whiteboard";
    default:                                     return "unknown";
    }
}

/* AF-mode value -> name (the modes indraw exposes; the UI renames for display). */
static const char *af_mode_name(int v)
{
    switch (v) {
    case ACAMERA_CONTROL_AF_MODE_OFF:                return "off";
    case ACAMERA_CONTROL_AF_MODE_AUTO:               return "auto";
    case ACAMERA_CONTROL_AF_MODE_MACRO:              return "macro";
    case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO:   return "continuous_video";
    case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE: return "continuous";
    case ACAMERA_CONTROL_AF_MODE_EDOF:               return "edof";
    default:                                         return "unknown";
    }
}

/* AF state value -> name, so reports are readable without the NDK headers. */
static const char *af_state_name(int s)
{
    switch (s) {
    case ACAMERA_CONTROL_AF_STATE_INACTIVE:           return "inactive";
    case ACAMERA_CONTROL_AF_STATE_PASSIVE_SCAN:       return "passive_scan";
    case ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED:    return "passive_focused";
    case ACAMERA_CONTROL_AF_STATE_ACTIVE_SCAN:        return "active_scan";
    case ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED:     return "focused_locked";
    case ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED: return "not_focused_locked";
    case ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED:  return "passive_unfocused";
    default:                                          return "unknown";
    }
}

/* AE state value -> name. */
static const char *ae_state_name(int s)
{
    switch (s) {
    case ACAMERA_CONTROL_AE_STATE_INACTIVE:        return "inactive";
    case ACAMERA_CONTROL_AE_STATE_SEARCHING:       return "searching";
    case ACAMERA_CONTROL_AE_STATE_CONVERGED:       return "converged";
    case ACAMERA_CONTROL_AE_STATE_LOCKED:          return "locked";
    case ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED:  return "flash_required";
    case ACAMERA_CONTROL_AE_STATE_PRECAPTURE:      return "precapture";
    default:                                       return "unknown";
    }
}

/* True if the u8 list `tag` of `meta` contains `value`. */
static int u8_list_has(const ACameraMetadata *meta, uint32_t tag, int value)
{
    ACameraMetadata_const_entry e;                         /* the list            */
    uint32_t i;
    if (ACameraMetadata_getConstEntry(meta, tag, &e) != ACAMERA_OK) return 0; /* absent */
    if (e.type != ACAMERA_TYPE_BYTE) return 0;             /* unexpected type     */
    for (i = 0; i < e.count; ++i)
        if (e.data.u8[i] == value) return 1;               /* found               */
    return 0;
}

/* True if the camera lists an OUTPUT stream of `format` at exactly w x h. */
static int has_output_size(const ACameraMetadata *chars, int format, int w, int h)
{
    ACameraMetadata_const_entry e;   /* flat int32 list: format, width, height, isInput */
    uint32_t i;
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) != ACAMERA_OK || e.type != ACAMERA_TYPE_INT32)
        return 0;
    for (i = 0; i + 3 < e.count; i += 4)                    /* one entry = 4 ints */
        if (e.data.i32[i] == format && e.data.i32[i + 1] == w &&
            e.data.i32[i + 2] == h && e.data.i32[i + 3] == 0) /* 0 = output         */
            return 1;
    return 0;
}

/* Largest OUTPUT size of `format` by pixel count, with width <= max_w when
 * max_w > 0 (preview pick). Returns 1 if one was found. */
static int largest_output_size_within(const ACameraMetadata *chars, int format, int max_w, int max_h, int *w, int *h)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    long best = 0;
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) != ACAMERA_OK || e.type != ACAMERA_TYPE_INT32)
        return 0;
    for (i = 0; i + 3 < e.count; i += 4) {
        long px = (long)e.data.i32[i + 1] * e.data.i32[i + 2];
        if (e.data.i32[i] != format || e.data.i32[i + 3] != 0) continue;
        if (e.data.i32[i + 1] > max_w || e.data.i32[i + 2] > max_h) continue;
        if (px > best) { best = px; *w = e.data.i32[i + 1]; *h = e.data.i32[i + 2]; }
    }
    return best > 0;
}
static int largest_output_size(const ACameraMetadata *chars, int format, int max_w, int *w, int *h)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    long best = 0;
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) != ACAMERA_OK || e.type != ACAMERA_TYPE_INT32)
        return 0;
    for (i = 0; i + 3 < e.count; i += 4) {
        long px = (long)e.data.i32[i + 1] * e.data.i32[i + 2];
        if (e.data.i32[i] != format || e.data.i32[i + 3] != 0) continue;
        if (max_w > 0 && e.data.i32[i + 1] > max_w) continue;
        if (px > best) { best = px; *w = e.data.i32[i + 1]; *h = e.data.i32[i + 2]; }
    }
    return best > 0;
}

/* The preview size: the largest output of `format` no wider than max_w whose
 * proportions are the photo's (jw x jh, within 2 %), so that the viewfinder
 * shows the photo's frame; if none matches, the largest no wider than max_w.
 * (The largest alone picked a square 1200x1200 on the Xperia XA2, whose
 * photos are 16:9.) */
static int preview_output_size(const ACameraMetadata *chars, int format, int max_w, int jw, int jh, int *w, int *h)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    long best = 0;
    double want = jh > 0 ? (double)jw / jh : 0;
    if (want > 0 && ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT32)
        for (i = 0; i + 3 < e.count; i += 4) {
            int cw = e.data.i32[i + 1], ch = e.data.i32[i + 2];
            long px = (long)cw * ch;
            if (e.data.i32[i] != format || e.data.i32[i + 3] != 0 || ch <= 0) continue;
            if (max_w > 0 && cw > max_w) continue;
            if (fabs((double)cw / ch / want - 1.0) > 0.02) continue;
            if (px > best) { best = px; *w = cw; *h = ch; }
        }
    if (best > 0) return 1;
    return largest_output_size(chars, format, max_w, w, h);
}

/* Appends "[[w,h],...]" for every OUTPUT size of `format`. */
static void add_sizes(jw_t *w, const ACameraMetadata *chars, int format)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    int first = 1;                                           /* comma management */
    jw_add(w, "[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) == ACAMERA_OK && e.type == ACAMERA_TYPE_INT32) {
        for (i = 0; i + 3 < e.count; i += 4) {
            if (e.data.i32[i] != format || e.data.i32[i + 3] != 0) continue; /* skip */
            jw_add(w, "%s[%d,%d]", first ? "" : ",", e.data.i32[i + 1], e.data.i32[i + 2]);
            first = 0;
        }
    }
    jw_add(w, "]");
}

/* Appends "[[w,h,ns],...]" from a per-stream duration table (min frame
 * duration or stall duration: int64 format, width, height, ns). */
static void add_durations(jw_t *w, const ACameraMetadata *chars, uint32_t tag, int format)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    int first = 1;
    jw_add(w, "[");
    if (ACameraMetadata_getConstEntry(chars, tag, &e) == ACAMERA_OK && e.type == ACAMERA_TYPE_INT64)
        for (i = 0; i + 3 < e.count; i += 4) {
            if (e.data.i64[i] != format) continue;
            jw_add(w, "%s[%lld,%lld,%lld]", first ? "" : ",", (long long)e.data.i64[i + 1],
                   (long long)e.data.i64[i + 2], (long long)e.data.i64[i + 3]);
            first = 0;
        }
    jw_add(w, "]");
}

/* ------------------------------------------------------------------------- */
/* Session state (one camera at a time)                                      */
/* ------------------------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;   /* protects every field written by callbacks      */
    pthread_cond_t cond;    /* signalled whenever such a field changes        */
    int sync_ready;         /* lock/cond initialised                          */

    /* Camera2 objects, created by indraw_open, released by close_all(). */
    ACameraManager *manager;
    ACameraMetadata *chars;                 /* static characteristics           */
    ACameraDevice *device;
    ACameraCaptureSession *session;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *preview_output, *jpeg_output, *raw_output;
    ACameraOutputTarget *preview_target, *jpeg_target, *raw_target;
    ACaptureRequest *preview_request;       /* repeating request (viewfinder)   */
    ACaptureRequest *still_request;         /* reused for every photo           */
    ACaptureRequest *trigger_request;       /* one-shot AF/AE triggers (preview) */
    ACaptureRequest *raw_request;           /* RAW16 + preview, reused per RAW   */
    AImageReader *preview_reader, *jpeg_reader, *raw_reader;
    ANativeWindow *preview_window, *jpeg_window, *raw_window; /* owned by readers */
    int af_continuous;                      /* CONTINUOUS_PICTURE is available  */
    int tpl_nr, tpl_edge;                   /* STILL_CAPTURE template values    */
    int tpl_control;                        /* template CONTROL_MODE, or -1     */
    int pv_nr, pv_edge;                     /* the PREVIEW's noise/edge (-1 = untouched); photos
                                             * never inherit them: each shot says its own, or the
                                             * template default applies */
    int32_t tpl_fps[2];                     /* template AE FPS range, or {-1,-1} */
    int32_t low_fps[2];                     /* lowest listed AE FPS range       */
    char camera_id[16];
    char make[64], model[64];               /* ro.product.manufacturer / .model  */
    int64_t override_shutter_max_ns;        /* 0 = none; else the tested ceiling */
    int lim_raw_off;                        /* device profile: no RAW stream      */
    int lim_jw, lim_jh;                     /* device profile: largest JPEG asked (0 = none) */
    int64_t override_min_frame_ns;          /* 0 = default 1/30 s floor; else set */
    int64_t override_peak_edge;             /* 0 = default 40; peaking gradient threshold */
    int64_t override_zebra_jpeg;            /* 0 = default 250; zebra level, jpg (0..255) */
    int64_t override_zebra_raw;             /* 0 = default 992; zebra level, raw (per-mille of white) */
    int64_t override_peak_width;            /* 0 = default 2; peaking line width (px) */
    /* Focus calibration: maps a REAL focus diopter (1/metres, 0 = infinity) to
     * the diopter to SEND the HAL, since LENS_FOCUS_DISTANCE is uncalibrated.
     * Points are kept sorted by real_d; 0 points = no table (use 1:1). */
    int focus_cal_n;                        /* number of points (0 = none)       */
    float focus_cal_real[16];              /* real diopters, ascending          */
    float focus_cal_hal[16];               /* HAL diopters to send              */
    float max_diopters;                    /* physical max (min focus distance) */
    int jpeg_w, jpeg_h;
    int preview_w, preview_h;               /* preview stream size (for SF2P)    */
    int sensor_orientation;                 /* SENSOR_ORIENTATION degrees (JP2: 90) */
    int lens_front;                         /* LENS_FACING_FRONT: the rotation adds the other way */
    int device_rot;                         /* the phone's physical rotation, clockwise from its
                                             * natural portrait (indraw_orient): 0/90/180/270 */
    int raw_w, raw_h;                       /* 0 x 0 = no RAW stream             */
    int shading_map;                        /* lens shading map can be reported  */
    /* Vendor keys set by vendor_request: their value as the preview RESULTS
     * report it (does the HAL apply what we send?), read on each result. */
    /* The measured lens-shading calibration (shading.h), when one is loaded
     * for this camera, and how a DNG's gain map is chosen: 1 the measured
     * one (when loaded, else the HAL's), 0 the HAL's, 2 none. The user's
     * dosing (pivot, strength, maximum gain) comes from the app. */
    shading_cal_t shade;
    int shade_have, shade_mode;
    double shade_last_d, shade_last_k;      /* the last photo's lens pos / light (preview) */
    int shade_last_measured;                /* the last DNG used the measured map */
    double shade_balance, shade_strength, shade_max_gain;
    float *shade_buf;                       /* the composed map of the photo in progress */
    char shade_path[600];
    uint32_t vw_tag[8];
    char vw_value[8][64];
    int vw_n;

    /* Written by callbacks (under lock). */
    long preview_frames;                    /* frames seen on the preview stream */
    char pf_path_buf[1100];                 /* the path of a requested frame save: the engine's
                                             * OWN copy (the caller's buffer may be gone by the
                                             * time the listener writes, after a timeout) */
    const char *pf_path;                    /* non-NULL = save next preview frame here */
    int pf_done;                            /* set by the listener when written  */
    int pf_w, pf_h;                         /* size it wrote                     */
    int pf_err;                             /* 0 ok, else an error occurred      */
    int stream_fd;                          /* >=0 = stream SF2P frames to this fd */
    int state_events;                       /* the continuous "ae" line is emitted (1) */
    unsigned events_dropped;                /* event lines dropped on a full pipe (out_lock) */
    int event_fd;                           /* >=0 = state events go to the controller
                                             * through this pipe (plain lines) instead
                                             * of the stream (indraw_set_event_fd)   */
    pthread_mutex_t out_lock;               /* serialises SF2P frames and SF2M events */

    long live_results;                      /* preview results seen since trigger */
    int trigger_seen;                       /* a result echoed our trigger       */
    int live_ae, live_af;                   /* 3A states of the latest result    */
    int live_lens_state;                    /* LENS_STATE: 0 stationary, 1 moving */
    float live_focus;                       /* LENS_FOCUS_DISTANCE in the preview  */
    int device_error;                       /* 0, or the onError code / -1       */
    AImage *jpeg_image;                     /* latest JPEG waiting to be saved   */
    AImage *raw_image;                      /* latest RAW16 waiting to be saved  */
    ACameraMetadata *r_meta;                /* full copy of the last still result */
    /* "captured kind=..." still owed for the photo in progress (NULL: none).
     * It tells the interface the exposure is over (its white flash: "you may
     * move"). Sent at the FIRST of: the capture result (the sensor has read
     * the frame -- before any encoding) or the image itself (should the
     * result come late or never); once. (Before 0.47.32 only at the image:
     * a JPEG's flash waited for the encoding, 0.3-0.4 s after the pose.) */
    const char *captured_owed;
    int capture_failed;                     /* still request reported failure    */
    int capture_fail_reason;
    int result_ready;                       /* capture result copied below       */
    int64_t r_timestamp, r_exposure_ns;     /* from the capture result           */
    int64_t r_frame_ns;                     /* frame duration really used        */
    int32_t r_fps[2];                       /* AE target FPS range in the result */
    int r_control;                          /* CONTROL_MODE in the result        */
    int scene_mode;                         /* the scene in force (0 none): captures keep USE_SCENE_MODE */
    int32_t r_iso;
    int r_ae_state, r_af_state;
    int r_nr, r_edge;                       /* modes the camera reports applied  */
    float r_focus_diopters;

    /* Live exposure of the preview (UI-facing, see indraw_preview_exposure):
     * what the repeating request asks for, mirrored on the meter request. */
    int64_t pv_exposure_ns;                 /* 0 = AE on; else manual exposure   */
    int pv_iso;                             /* manual ISO (with pv_exposure_ns)  */
    int ev_steps;                           /* AE compensation, in HAL steps     */
    long ae_results;                        /* preview results seen (for throttling) */

    /* Exposure meter: periodic RAW16 frames measured in place (never written),
     * so the UI can run its own AE loop on linear sensor data. */
    ACaptureRequest *meter_request;         /* RAW16 + preview, preview template  */
    pthread_t meter_thread;
    int meter_thread_up;                    /* thread created                    */
    int meter_on;                           /* 1 = periodic captures running     */
    int meter_stop;                         /* ask the thread to exit            */
    int meter_interval_ms;                  /* period between measure captures   */
    int meter_inflight;                     /* measure captures not yet returned */
    int capture_busy;                       /* a photo is using the RAW stream   */
    int64_t meter_ts, meter_exposure_ns;    /* result of the latest meter frame  */
    int meter_iso;
    float zoom;                             /* digital zoom factor (1 = none)    */
    /* The focus area while zoomed (0.47.31). Without a region of ours the
     * JP2's HAL focuses on a default area of the WHOLE sensor, wider than a
     * zoomed view: it took an object just outside the frame, its sharp zone
     * sat low in the image. So, zoomed and no point tapped for focus, the AF
     * region is the middle third of the zoom window; at zoom 1 the entry is
     * removed again (the HAL's own default, as before). */
    int af_tapped;                          /* a tap set the AF region (until cleared) */
    /* Which Camera2 template the photo and RAW requests are built from: 0
     * (default, every phone so far) TEMPLATE_STILL_CAPTURE; 1 TEMPLATE_PREVIEW.
     * Set by a device profile only (override 'still_template': 'preview'), to
     * test whether a HAL's still template carries something of its own -- on
     * the Xperia XA2 the lens moved at the shutter although the capture asked
     * for AF off at the preview's distance. Read at open; JP2 untouched. */
    int still_template_preview;
    /* A fixed pause before every capture, after the focus has been set on
     * the preview and the capture request (ms; 0 = none, the default).
     * From a device profile only ("capture_delay_ms"): for a HAL whose lens
     * is still travelling when the capture is sent (the XA2). */
    int capture_delay_ms;
    /* Session parameters a device profile asks for ("session_request"):
     * keys the HAL reads only when the session is configured (the JP2's
     * MediaTek HDR mode 0x80030000, EIS...). Set before indraw_open; the
     * session is then created with
     * ACameraDevice_createCaptureSessionWithSessionParameters (API 28,
     * resolved at run time: absent on an Android 8.1 base such as the XA2's,
     * where they are reported unsupported and the session is plain). */
    struct { uint32_t tag; int type; double value; } session_req[8];
    int n_session_req;
    int session_params_used;                /* -1 unsupported, 0 none, 1 applied */
    int trigger_pending;                    /* an AF scan / AE precapture was started */
    int af_zoom_on;                         /* our zoom AF region is on the requests */
    int spot_on;                            /* the meter measures the tap's spot */
    float spot_hw, spot_hh;                 /* the spot half-size, sensor-normalised (follows the loupe) */
    int64_t live_exp_ns;                    /* the preview's exposure and ISO, last result */
    int live_iso_v;
    int burst_on;                           /* JPEG callback queues instead of replacing */
    AImage *jpeg_q[8];                      /* the burst's images, in arrival order */
    int jpeg_qn;
    int flash_mode;                         /* 0 off, 1 auto, 2 on, 3 torch (indraw_flash) */
    int ae_lock;                            /* automatic exposure held (indraw_ae_lock)    */
    struct { int64_t ts; int32_t crop[4]; int iso; int64_t exp_ns; } crop_ring[8];
                                            /* preview results by timestamp: crop and pair */
    int crop_ring_pos;
    int32_t sent_crop[4];                   /* the last crop announced with a frame (fcrop) */
    double view_win[4];                     /* what the interface shows (x, y, w, h, normalised in
                                             * the active array; the controller sets it): the
                                             * histograms cover it, not the HAL's frame */
    int have_view_win;
    float loupe;                            /* the focus loupe on the PREVIEW only (1 = off) */
    float loupe_cx, loupe_cy;               /* its centre, normalised in the active array    */
    float spot_x, spot_y;                   /* its centre, sensor-normalised 0..1 */
    int awb_mode;                           /* CONTROL_AWB_MODE set by indraw_wb (0 = auto default) */
    float awb_gains[4];                     /* last gains the HAL's AWB applied (R, Geven, Godd, B) */
    double ct_m1[9], ct_m2[9];              /* the sensor's two XYZ->camera calibration matrices */
    double ct_t1, ct_t2;                    /* their illuminants' temperatures (K); t2 0 = one only */
    int have_ct;                            /* (colortemp.h: the white balance in kelvin) */
    int live_cct;                           /* the white balance in force: correlated colour temperature */
    double live_duv;                        /* and its distance from the Planckian locus (tint, + green) */
    ACameraMetadata_rational awb_transform[9]; /* and its colour transform; for manual WB */
    int awb_seen;                           /* 1 once gains/transform were captured  */
    /* A white balance measured on a neutral object ("wb pick"): fixed gains
     * that replace the AWB's as the base of the manual white balance (the
     * shifts apply on top), until cleared ("wb fixed off"). */
    int wb_fixed;
    float wb_fixed_gains[4];                /* R, Ge, Go, B */
    int wb_shift, wb_tint;                  /* the last manual shift / tint applied */
    int wbpick_pending, wbpick_done, wbpick_status;   /* status 0 ok, 1 clipped, 2 dark */
    double wbpick_rgb[3];                   /* the measured white, camera RGB (black-subtracted) */
    int af_wait_ms;                         /* how long a capture waits for 3A (0 = 1500) */
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .out_lock = PTHREAD_MUTEX_INITIALIZER, .stream_fd = -1, .event_fd = -1, .state_events = 1 };

/* Initialise the condition variable on CLOCK_MONOTONIC, once. */
static void sync_init(void)
{
    pthread_condattr_t a;
    if (g.sync_ready) return;                       /* already done              */
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC); /* match deadline_in()       */
    pthread_cond_init(&g.cond, &a);
    pthread_condattr_destroy(&a);
    g.sync_ready = 1;
}

/* ------------------------------------------------------------------------- */
/* Callbacks (run on Android threads owned by the camera framework)          */
/* ------------------------------------------------------------------------- */

/* Device lost (another app took it, or the service died). */
static void on_disconnected(void *ctx, ACameraDevice *dev)
{
    (void)ctx; (void)dev;
    pthread_mutex_lock(&g.lock);
    g.device_error = -1;                 /* -1 = disconnected               */
    pthread_cond_broadcast(&g.cond);     /* wake any waiting photo          */
    pthread_mutex_unlock(&g.lock);
}

/* Fatal device error; `err` is an ERROR_CAMERA_* code. */
static void on_device_error(void *ctx, ACameraDevice *dev, int err)
{
    (void)ctx; (void)dev;
    pthread_mutex_lock(&g.lock);
    g.device_error = err ? err : -2;     /* keep it non-zero                */
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* Session state changes: nothing to do yet, but non-NULL to be safe. */
static void on_session_noop(void *ctx, ACameraCaptureSession *s) { (void)ctx; (void)s; }

/* Preview frame available: count it and release it at once.
 * If nobody drained this reader it would fill up and stall the pipeline. */
static int ae_done(int s);   /* defined later; used by the preview listener */
static int af_done(int s);

/* Writes exactly `len` bytes to fd, retrying short writes; returns 0 on success
 * or -1 if the pipe broke (reader gone). */
static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Writes one RGB frame to the stream fd in the SF2P framing (RAWfish-compatible):
 * a 16-byte little-endian header "SF2P" + width + height + byte count, then the
 * width*height*3 RGB bytes. Returns 0, or -1 if the pipe broke. */
static int write_sf2p(int fd, const uint8_t *rgb, int w, int h)
{
    uint8_t hdr[16];
    uint32_t bytes;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;   /* a false header never goes out */
    bytes = (uint32_t)w * (uint32_t)h * 3u;
    hdr[0]='S'; hdr[1]='F'; hdr[2]='2'; hdr[3]='P';
    hdr[4]=(uint8_t)w; hdr[5]=(uint8_t)(w>>8); hdr[6]=(uint8_t)(w>>16); hdr[7]=(uint8_t)(w>>24);
    hdr[8]=(uint8_t)h; hdr[9]=(uint8_t)(h>>8); hdr[10]=(uint8_t)(h>>16); hdr[11]=(uint8_t)(h>>24);
    hdr[12]=(uint8_t)bytes; hdr[13]=(uint8_t)(bytes>>8); hdr[14]=(uint8_t)(bytes>>16); hdr[15]=(uint8_t)(bytes>>24);
    if (write_all(fd, hdr, sizeof hdr) != 0) return -1;
    return write_all(fd, rgb, bytes);
}

/* Writes one SF2M metadata/event packet: an 8-byte header "SF2M" + little-endian
 * byte count, then `len` bytes of text (one line). Same channel as the frames,
 * distinguished by the magic, so the reader never confuses the two. */
static int write_sf2m(int fd, const char *text, int len)
{
    uint8_t hdr[8];
    hdr[0]='S'; hdr[1]='F'; hdr[2]='2'; hdr[3]='M';
    hdr[4]=(uint8_t)len; hdr[5]=(uint8_t)(len>>8); hdr[6]=(uint8_t)(len>>16); hdr[7]=(uint8_t)(len>>24);
    if (write_all(fd, hdr, sizeof hdr) != 0) return -1;
    return write_all(fd, text, (size_t)len);
}

/* Emits an SF2M event line to the stream, if streaming. Thread-safe: takes the
 * output lock so it never interleaves with a frame written by the preview
 * thread. No-op when not streaming. */
/* A state event (caps, ae, meter, captured, written, progress, stack_done).
 * With a controller attached (indraw_set_event_fd) it goes to the controller
 * through its pipe, as a plain line: the controller decides what the
 * interface sees and forwards with indraw_emit(). Without one, straight to
 * the stream as SF2M (the CLI used alone, older interfaces). Frame-bound
 * events ("fcrop") never come here: they are written with their frame.
 * Camera2 calls this on Android threads; a write() on a pipe is safe there. */
/* Events go to the controller's pipe from the CAMERA threads (preview
 * results, images, capture completions), under out_lock. The pipe's write
 * end is non-blocking (the controller sets it so): the reader thread may
 * be busy for a second or more (a focus command waiting for the lens) while
 * 'ae' lines (30/s) and 'vfmeter' lines (2.5 KB, 10/s) keep coming, and a
 * blocking write here would stall the preview and every writer behind
 * out_lock. When the pipe is full: a STATE line (ae, vfmeter, meter -- the
 * next one supersedes it) is dropped; a one-off EVENT (captured, written,
 * wbpick, cal_*) waits up to 300 ms for room, then is dropped and counted
 * (0.47.58). A blocking fd (run mode: the terminal) never returns EAGAIN. */
static int event_is_state(const char *text)
{
    return !strncmp(text, "ae ", 3) || !strncmp(text, "vfmeter ", 8) || !strncmp(text, "meter ", 6);
}
static void emit_event(const char *text)
{
    int fd, len = (int)strlen(text);
    pthread_mutex_lock(&g.out_lock);
    fd = g.event_fd;
    if (fd >= 0) {
        const char *p = text;
        int state = event_is_state(text), waited_ms = 0;
        while (len > 0) {
            ssize_t n = write(fd, p, (size_t)len);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (state || waited_ms >= 300) { g.events_dropped++; break; }
                    { struct pollfd pf; pf.fd = fd; pf.events = POLLOUT; poll(&pf, 1, 50); }
                    waited_ms += 50;
                    continue;
                }
                break;                                   /* EPIPE and the rest: the reader is gone */
            }
            p += n; len -= (int)n;
        }
    } else if (g.stream_fd >= 0)
        write_sf2m(g.stream_fd, text, len);
    pthread_mutex_unlock(&g.out_lock);
}

/* Is a stream attached? stream_fd and event_fd live under out_lock (the
 * writers' lock); g.lock may be held when this is called (g -> out is the
 * only nesting order in this file, never out -> g). */
static int streaming(void)
{
    int on;
    pthread_mutex_lock(&g.out_lock);
    on = g.stream_fd >= 0;
    pthread_mutex_unlock(&g.out_lock);
    return on;
}

/* The controller's way to the interface: one SF2M line on the stream, in
 * order with the frames (same lock, same writer). */
IR_API int indraw_emit(const char *text)
{
    int fd;
    if (!text) return IR_EARG;
    pthread_mutex_lock(&g.out_lock);
    fd = g.stream_fd;
    if (fd >= 0) write_sf2m(fd, text, (int)strlen(text));
    pthread_mutex_unlock(&g.out_lock);
    return fd >= 0 ? IR_OK : IR_ESTATE;
}

/* Attaches the controller: state events go to fd (a pipe the controller
 * reads on its own thread); -1 detaches (events back to the stream). */
IR_API int indraw_set_event_fd(int fd)
{
    pthread_mutex_lock(&g.out_lock);
    g.event_fd = fd;
    pthread_mutex_unlock(&g.out_lock);
    return IR_OK;
}
/* In run mode the CLI routes events to the terminal but does not want the
 * continuous preview-state ("ae ...") lines, only one-off events. It sets
 * this to 0; a controller (stream mode) leaves it 1. */
IR_API int indraw_set_state_events(int on)
{
    pthread_mutex_lock(&g.out_lock);
    g.state_events = on ? 1 : 0;
    pthread_mutex_unlock(&g.out_lock);
    return IR_OK;
}

/* "written kind=KIND path=PATH": the file is on disk (paths never contain
 * spaces: names are validated by the CLI). The UI uses it to save a preview
 * next to a DNG, and to refresh its gallery. */
static void emit_written(const char *kind, const char *path)
{
    char line[1200];
    snprintf(line, sizeof line, "written kind=%s path=%s\n", kind, path);
    emit_event(line);
}

/* Converts one Camera2 YUV_420_888 image to a freshly malloc'd RGB888 buffer
 * (width by height by 3 bytes, row-major): the planes are read here, the
 * arithmetic is yuv420_to_rgb in yuv.h (bounds once per row, chroma terms
 * once per pair of pixels, pixel-exact with the former per-pixel loop and
 * tested against it on the host). Returns the buffer and fills out_w and
 * out_h, or NULL on any error (caller frees). */
static uint8_t *yuv_to_rgb(AImage *img, int *out_w, int *out_h)
{
    uint8_t *py = NULL, *pu = NULL, *pv = NULL;
    int ly = 0, lu = 0, lv = 0, ry = 0, ru = 0, rv = 0, xy = 0, xu = 0, xv = 0;
    int w = 0, h = 0, fmt = 0, planes = 0;
    uint8_t *rgb;
    if (AImage_getWidth(img, &w) != AMEDIA_OK || AImage_getHeight(img, &h) != AMEDIA_OK ||
        AImage_getFormat(img, &fmt) != AMEDIA_OK || fmt != AIMAGE_FORMAT_YUV_420_888 ||
        AImage_getNumberOfPlanes(img, &planes) != AMEDIA_OK || planes < 3 ||
        AImage_getPlaneData(img, 0, &py, &ly) != AMEDIA_OK ||
        AImage_getPlaneData(img, 1, &pu, &lu) != AMEDIA_OK ||
        AImage_getPlaneData(img, 2, &pv, &lv) != AMEDIA_OK ||
        AImage_getPlaneRowStride(img, 0, &ry) != AMEDIA_OK ||
        AImage_getPlaneRowStride(img, 1, &ru) != AMEDIA_OK ||
        AImage_getPlaneRowStride(img, 2, &rv) != AMEDIA_OK ||
        AImage_getPlanePixelStride(img, 0, &xy) != AMEDIA_OK ||
        AImage_getPlanePixelStride(img, 1, &xu) != AMEDIA_OK ||
        AImage_getPlanePixelStride(img, 2, &xv) != AMEDIA_OK || w <= 0 || h <= 0)
        return NULL;
    rgb = malloc((size_t)w * h * 3);
    if (!rgb) return NULL;
    yuv420_to_rgb(py, ly, ry, xy, pu, lu, ru, xu, pv, lv, rv, xv, w, h, rgb);
    *out_w = w;
    *out_h = h;
    return rgb;
}

static int photo_orientation_deg(void);     /* with indraw_orient, below */
static int apply_session_request(ACaptureRequest *req);   /* session parameters, below */
static int photo_orientation_tiff(void);
static int active_array(int32_t *ax, int32_t *ay, int32_t *aw, int32_t *ah, float *max_zoom);

/* The viewfinder meter (step 2 of docs/ARCHITECTURE.md): what the app's C++
 * item used to measure on the RGB frame it received, now measured here on
 * the same frame before it leaves, and sent as one event:
 *   vfmeter mean=0.xxxx clip_lo=PCT clip_hi=PCT hist=r0..r63;g0..g63;b0..b63
 * - mean: linear (gamma 2.2 undone) green; centre-weighted (x3 in the middle
 *   quarter), or, with a light reference, only the spot -- the tap's box on
 *   the scene (sensor-normalised), mapped into THIS frame through the crop it
 *   was taken with, sampled densely enough for ~1000 points;
 * - histogram: 64 bins per channel over every 4th pixel, peak 1000; clipping
 *   percentages at 0 and 255.
 * At most every 100 ms. The same measure for every interface. */
static void vf_meter(const uint8_t *rgb, int w, int h, const int32_t crop[4], int iso, int64_t exp_ns)
{
    static double lut[256];
    static int lut_ready = 0;
    static double last_ms = 0;
    long hist[3][64] = {{0}}, n = 0, lo = 0, hi = 0, peak = 1;
    long shist[3][64] = {{0}}, speak = 1;              /* the spot's own histogram */
    double sum = 0, wsum = 0, now = now_ms();
    int x, y, c, i, spot_on;
    int hx0, hy0, hx1, hy1, hstep;
    double sx, sy, shw, shh;
    char line[2600], *p, *end;
    int32_t ax, ay, aw, ah;
    if (now - last_ms < 100) return;
    last_ms = now;
    if (!lut_ready) { for (i = 0; i < 256; ++i) lut[i] = pow(i / 255.0, 2.2); lut_ready = 1; }
    pthread_mutex_lock(&g.lock);
    spot_on = g.spot_on; sx = g.spot_x; sy = g.spot_y;
    shw = g.spot_hw > 0 ? g.spot_hw : 0.06; shh = g.spot_hh > 0 ? g.spot_hh : 0.06;
    pthread_mutex_unlock(&g.lock);
    if (spot_on && crop && crop[2] > 0 && crop[3] > 0 && !active_array(&ax, &ay, &aw, &ah, NULL)) {
        /* the spot's box, scene -> this frame */
        double fx0 = ((sx - shw) * aw + ax - crop[0]) / crop[2], fx1 = ((sx + shw) * aw + ax - crop[0]) / crop[2];
        double fy0 = ((sy - shh) * ah + ay - crop[1]) / crop[3], fy1 = ((sy + shh) * ah + ay - crop[1]) / crop[3];
        int bx0 = (int)(fx0 * w), bx1 = (int)(fx1 * w), by0 = (int)(fy0 * h), by1 = (int)(fy1 * h), bstep;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > w) bx1 = w;
        if (by1 > h) by1 = h;
        if (bx1 - bx0 >= 2 && by1 - by0 >= 2) {
            bstep = (int)sqrt((double)(bx1 - bx0) * (by1 - by0) / 1000.0);
            if (bstep < 1) bstep = 1;
            for (y = by0; y < by1; y += bstep)
                for (x = bx0; x < bx1; x += bstep) {
                    const uint8_t *px = rgb + (y * w + x) * 3;
                    sum += lut[px[1]]; wsum += 1.0;
                    for (c = 0; c < 3; ++c) shist[c][px[c] >> 2]++;
                }
        } else spot_on = 0;                          /* the spot is out of this frame: centre-weighted */
    } else spot_on = 0;
    /* The histogram (and the centre-weighted mean) over what the interface
     * shows: the display window mapped into this frame through its crop. */
    {
        double vw[4];
        int have;
        pthread_mutex_lock(&g.lock);
        have = g.have_view_win;
        memcpy(vw, g.view_win, sizeof vw);
        pthread_mutex_unlock(&g.lock);
        hx0 = 0; hy0 = 0; hx1 = w; hy1 = h;
        if (have && crop && crop[2] > 0 && crop[3] > 0 && !active_array(&ax, &ay, &aw, &ah, NULL)) {
            hx0 = (int)((vw[0] * aw + ax - crop[0]) / crop[2] * w);
            hy0 = (int)((vw[1] * ah + ay - crop[1]) / crop[3] * h);
            hx1 = (int)(((vw[0] + vw[2]) * aw + ax - crop[0]) / crop[2] * w);
            hy1 = (int)(((vw[1] + vw[3]) * ah + ay - crop[1]) / crop[3] * h);
            if (hx0 < 0) hx0 = 0;
            if (hy0 < 0) hy0 = 0;
            if (hx1 > w) hx1 = w;
            if (hy1 > h) hy1 = h;
            if (hx1 - hx0 < 4 || hy1 - hy0 < 4) { hx0 = 0; hy0 = 0; hx1 = w; hy1 = h; }
        }
        hstep = (int)sqrt((double)(hx1 - hx0) * (hy1 - hy0) / 80000.0);   /* ~80k samples */
        if (hstep < 1) hstep = 1;
    }
    for (y = hy0; y < hy1; y += hstep) {
        const uint8_t *row = rgb + (size_t)y * w * 3;
        int cy = y >= hy0 + (hy1 - hy0) / 4 && y < hy0 + 3 * (hy1 - hy0) / 4;
        for (x = hx0; x < hx1; x += hstep) {
            const uint8_t *px = row + x * 3;
            if (!spot_on) {
                double weight = (cy && x >= hx0 + (hx1 - hx0) / 4 && x < hx0 + 3 * (hx1 - hx0) / 4) ? 3.0 : 1.0;
                sum += weight * lut[px[1]];
                wsum += weight;
            }
            for (c = 0; c < 3; ++c) {
                hist[c][px[c] >> 2]++;
                if (px[c] == 0) lo++; else if (px[c] == 255) hi++;
                n++;
            }
        }
    }
    if (wsum <= 0 || n == 0) return;
    for (c = 0; c < 3; ++c) for (i = 0; i < 64; ++i) if (hist[c][i] > peak) peak = hist[c][i];
    for (c = 0; c < 3; ++c) for (i = 0; i < 64; ++i) if (shist[c][i] > speak) speak = shist[c][i];
    p = line; end = line + sizeof line;
    /* iso / exp_ns: the pair THIS frame was exposed with when its result
     * is known (looked up by timestamp), else 0 -- the reader then uses the
     * live values. Tagging the measure with its own pair lets the exposure
     * loop know at once whether a frame already shows the pair it sent. */
    p += snprintf(p, (size_t)(end - p), "vfmeter mean=%.5f clip_lo=%.3f clip_hi=%.3f iso=%d exp_ns=%lld hist=",
                  sum / wsum, 100.0 * lo / n, 100.0 * hi / n, iso, (long long)exp_ns);
    for (c = 0; c < 3 && p < end; ++c) {
        for (i = 0; i < 64 && p < end; ++i)
            p += snprintf(p, (size_t)(end - p), "%s%ld", i ? "," : "", hist[c][i] * 1000 / peak);
        if (c < 2 && p < end) p += snprintf(p, (size_t)(end - p), ";");
    }
    if (spot_on && p < end) {                        /* the spot's tones, to highlight */
        p += snprintf(p, (size_t)(end - p), " shist=");
        for (c = 0; c < 3 && p < end; ++c) {
            for (i = 0; i < 64 && p < end; ++i)
                p += snprintf(p, (size_t)(end - p), "%s%ld", i ? "," : "", shist[c][i] * 1000 / speak);
            if (c < 2 && p < end) p += snprintf(p, (size_t)(end - p), ";");
        }
    }
    if (p < end - 1) { *p++ = '\n'; *p = '\0'; emit_event(line); }
}

static void on_preview_image(void *ctx, AImageReader *reader)
{
    AImage *img = NULL;
    const char *want_path;
    (void)ctx;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img)
        return;

    /* Is a diagnostic frame save pending? Read the request under the lock;
     * the path is the engine's copy (pf_path_buf), valid whatever the
     * requesting call did meanwhile. */
    char want_buf[sizeof g.pf_path_buf];
    pthread_mutex_lock(&g.lock);
    want_path = NULL;
    if (g.pf_path) { snprintf(want_buf, sizeof want_buf, "%s", g.pf_path_buf); want_path = want_buf; }
    pthread_mutex_unlock(&g.lock);

    if (want_path) {
        /* Convert this frame and write it as a PPM, then signal completion.
         * Doing it HERE (in the listener that owns the image) avoids fighting
         * the listener for buffers, which is why a parallel acquire failed. */
        int w = 0, h = 0, err = 0;
        uint8_t *rgb = yuv_to_rgb(img, &w, &h);
        if (!rgb) {
            err = 1;
        } else {
            FILE *f = fopen(want_path, "wb");
            if (!f) {
                err = 1;
            } else {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                if (fwrite(rgb, 1, (size_t)w * h * 3, f) != (size_t)w * h * 3) err = 1;
                if (fclose(f) != 0) err = 1;
            }
            free(rgb);
        }
        pthread_mutex_lock(&g.lock);
        g.pf_path = NULL;                /* request consumed                 */
        g.pf_w = w; g.pf_h = h; g.pf_err = err;
        g.pf_done = 1;
        pthread_cond_broadcast(&g.cond); /* wake the waiting indraw_preview_frame */
        pthread_mutex_unlock(&g.lock);
    }

    /* If streaming is on, send this frame as SF2P -- but only once the preview
     * has settled (first frames are mid-convergence). The gate is latched once
     * so later frames always go. A broken pipe (reader gone) stops streaming. */
    {
        /* Stream every frame immediately, from the very first one. Early frames
         * are mid-convergence (off exposure, high-ISO watercolour) but showing
         * an image at once reads far better than a blank, "stuck" viewfinder; it
         * settles within ~1 s on its own. No gate, no delay. */
        int fd, ready;
        pthread_mutex_lock(&g.out_lock);        /* stream_fd lives under out_lock */
        fd = g.stream_fd;
        ready = (fd >= 0);
        pthread_mutex_unlock(&g.out_lock);
        if (fd >= 0 && ready) {
            int w = 0, h = 0;
            uint8_t *rgb = yuv_to_rgb(img, &w, &h);
            if (rgb) {
                int broke, k, found = 0;
                int32_t crop[4];
                int64_t ts = 0;
                char line[96];
                /* The crop this frame was taken with (its capture result, found
                 * by timestamp): announced as "fcrop" right BEFORE the frame when
                 * it changes, so the interface maps the frame exactly -- the
                 * HAL applies a new crop some frames after it is asked to, and
                 * a display computed from the request would jump meanwhile. */
                AImage_getTimestamp(img, &ts);
                int f_iso = 0;
                int64_t f_exp = 0;
                pthread_mutex_lock(&g.lock);
                for (k = 0; k < 8; ++k)
                    if (g.crop_ring[k].ts == ts) {
                        memcpy(crop, g.crop_ring[k].crop, sizeof crop);
                        f_iso = g.crop_ring[k].iso; f_exp = g.crop_ring[k].exp_ns;
                        found = 1;
                        break;
                    }
                pthread_mutex_unlock(&g.lock);
                line[0] = '\0';
                /* g.sent_crop is read and written by this listener thread
                 * only (no lock needed, although it lives in g). */
                if (found && memcmp(crop, g.sent_crop, sizeof crop)) {
                    int32_t ax, ay, aw, ah;
                    memcpy(g.sent_crop, crop, sizeof crop);
                    if (!active_array(&ax, &ay, &aw, &ah, NULL) && aw > 0 && ah > 0)
                        snprintf(line, sizeof line, "fcrop x=%.5f y=%.5f w=%.5f h=%.5f\n",
                                 (double)(crop[0] - ax) / aw, (double)(crop[1] - ay) / ah,
                                 (double)crop[2] / aw, (double)crop[3] / ah);
                }
                vf_meter(rgb, w, h, found ? crop : g.sent_crop, f_iso, f_exp);   /* the viewfinder meter */
                pthread_mutex_lock(&g.out_lock);        /* serialise with SF2M     */
                fd = g.stream_fd;                       /* re-read: a stop may have landed */
                broke = 0;
                if (fd >= 0) {
                    if (line[0]) write_sf2m(fd, line, (int)strlen(line));
                    broke = (write_sf2p(fd, rgb, w, h) != 0);
                    if (broke) g.stream_fd = -1;        /* pipe broke: stop        */
                }
                pthread_mutex_unlock(&g.out_lock);
                free(rgb);
            }
        }
    }

    AImage_delete(img);                  /* done with this frame             */
    pthread_mutex_lock(&g.lock);
    g.preview_frames++;                  /* proves the stream is alive       */
    pthread_cond_broadcast(&g.cond);     /* indraw_open waits for the first one  */
    pthread_mutex_unlock(&g.lock);
}

/* JPEG available: keep it for indraw_jpeg(), which writes it from its own thread. */
static void on_jpeg_image(void *ctx, AImageReader *reader)
{
    AImage *img = NULL;
    (void)ctx;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;
    pthread_mutex_lock(&g.lock);
    if (g.burst_on) {                              /* a burst: keep every image, in order */
        if (g.jpeg_qn < 8) g.jpeg_q[g.jpeg_qn++] = img; else AImage_delete(img);
        pthread_cond_broadcast(&g.cond);
        pthread_mutex_unlock(&g.lock);
        return;
    }
    if (g.jpeg_image) AImage_delete(g.jpeg_image); /* stale one (late photo): drop */
    g.jpeg_image = img;                            /* hand over to indraw_jpeg()       */
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* Measures one RAW16 frame for the exposure meter and emits the SF2M line:
 *   meter ts=NS iso=N exp_ns=NS mean=0.xxxx clip_lo=PCT clip_hi=PCT
 *         hist=r0,..,r63;g0,..,g63;b0,..,b63
 * mean   = centre-weighted (central quarter x3) green mean, black-subtracted
 *          and normalised to the white level (0 = black, 1 = saturation),
 *          LINEAR -- this is what the UI's own AE loop regulates.
 * clip_* = percentage of samples at/under black (lo) or at/over white (hi).
 * hist   = 64 bins per colour on a 1/2.2 power of the normalised value (so it
 *          reads like a usual histogram), counts scaled so the peak is 1000.
 * Sampling: 2x2 CFA quads on a grid, so every colour is read at every sample;
 * ~50k quads per frame, cheap enough to run in the callback. */
/* Histogram bin edges on the 1/2.2 power scale: edge[k] = (k/64)^2.2 of the
 * normalised linear value. A fixed table so the hot loop (and the library)
 * (a table, older than the libm link of 0.47.1; kept: it is also faster). */
static const float k_hist_edge[65] = {
    0.0000000f, 0.0001063f, 0.0004883f, 0.0011914f, 0.0022436f,
    0.0036655f, 0.0054744f, 0.0076846f, 0.0103087f, 0.0133579f,
    0.0168424f, 0.0207715f, 0.0251538f, 0.0299971f, 0.0353090f,
    0.0410965f, 0.0473661f, 0.0541242f, 0.0613767f, 0.0691293f,
    0.0773874f, 0.0861562f, 0.0954408f, 0.1052459f, 0.1155763f,
    0.1264364f, 0.1378305f, 0.1497629f, 0.1622377f, 0.1752587f,
    0.1888299f, 0.2029550f, 0.2176376f, 0.2328814f, 0.2486896f,
    0.2650658f, 0.2820133f, 0.2995352f, 0.3176347f, 0.3363150f,
    0.3555789f, 0.3754296f, 0.3958699f, 0.4169027f, 0.4385306f,
    0.4607566f, 0.4835833f, 0.5070133f, 0.5310492f, 0.5556936f,
    0.5809491f, 0.6068180f, 0.6333028f, 0.6604059f, 0.6881297f,
    0.7164765f, 0.7454486f, 0.7750482f, 0.8052776f, 0.8361390f,
    0.8676345f, 0.8997662f, 0.9325364f, 0.9659469f, 1.0000000f
};

/* Bin (0..63) of a normalised value in (0,1): binary search in k_hist_edge. */
static int hist_bin(double nv)
{
    int lo = 0, hi = 64;                 /* invariant: edge[lo] <= nv < edge[hi] */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (nv >= k_hist_edge[mid]) lo = mid; else hi = mid;
    }
    return lo;
}

static void meter_frame(AImage *img, int64_t ts, int iso, int64_t exposure_ns)
{
    int spot_on;                                   /* read once per frame, under the lock */
    double spot_x, spot_y, spot_hw, spot_hh;
    pthread_mutex_lock(&g.lock);
    spot_on = g.spot_on; spot_x = g.spot_x; spot_y = g.spot_y;
    spot_hw = g.spot_hw > 0 ? g.spot_hw : 0.06; spot_hh = g.spot_hh > 0 ? g.spot_hh : 0.06;
    pthread_mutex_unlock(&g.lock);
    ACameraMetadata_const_entry e;
    uint8_t *data = NULL;
    int len = 0, w = 0, h = 0, stride = 0, cfa = 3, step, x, y, c;
    int black = 64, white = 1023;
    double range, sum_g = 0, wsum = 0;
    long hist[3][64], peak = 1, n = 0, lo = 0, hi = 0;
    long shist[3][64] = {{0}}, speak = 1;              /* the spot's own histogram */
    int vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;            /* the DISPLAYED area, for the histogram */
    char line[2400], *p = line, *end = line + sizeof line;

    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    if (AImage_getPlaneData(img, 0, &data, &len) != AMEDIA_OK ||
        AImage_getPlaneRowStride(img, 0, &stride) != AMEDIA_OK ||
        w < 4 || h < 4 || stride < w * 2) return;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count == 4)
        black = (e.data.i32[0] + e.data.i32[1] + e.data.i32[2] + e.data.i32[3]) / 4;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count)
        white = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count)
        cfa = e.data.u8[0];
    range = (double)(white - black);
    if (range <= 0) return;
    memset(hist, 0, sizeof hist);

    /* A white-balance pick: the camera RGB of the spot (a light reference) or
     * of the middle of what is shown, on the RAW data itself. Refused when
     * clipped (a white that saturates has no colour left) or too dark. */
    {
        int pick, meter_on;
        pthread_mutex_lock(&g.lock);
        pick = g.wbpick_pending;
        meter_on = g.meter_on;
        pthread_mutex_unlock(&g.lock);
        if (pick) {
            double cx = spot_on ? spot_x : 0.5, cy = spot_on ? spot_y : 0.5, hw = spot_on ? spot_hw : 0.06, hh = spot_on ? spot_hh : 0.06;
            double sr = 0, sg = 0, sb = 0, cnt = 0, clipped = 0;
            int x0, x1, y0, y1, pstep, status = 0;
            if (!spot_on) {                              /* the middle of the displayed window */
                double vw[4];
                int have;
                pthread_mutex_lock(&g.lock);
                have = g.have_view_win;
                memcpy(vw, g.view_win, sizeof vw);
                pthread_mutex_unlock(&g.lock);
                if (have) { cx = vw[0] + vw[2] / 2; cy = vw[1] + vw[3] / 2; hw = 0.06 * vw[2]; hh = 0.06 * vw[3]; }
            }
            x0 = (int)((cx - hw) * w) & ~1; x1 = (int)((cx + hw) * w);
            y0 = (int)((cy - hh) * h) & ~1; y1 = (int)((cy + hh) * h);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w - 1) x1 = w - 1;
            if (y1 > h - 1) y1 = h - 1;
            pstep = (int)sqrt((double)(x1 - x0 + 2) * (y1 - y0 + 2) / 16000.0);   /* ~4000 quads */
            if (pstep < 2) pstep = 2;
            pstep &= ~1;
            for (y = y0; y + 1 < y1; y += pstep) {
                const uint16_t *r0 = (const uint16_t *)(data + (size_t)y * stride);
                const uint16_t *r1 = (const uint16_t *)(data + (size_t)(y + 1) * stride);
                for (x = x0; x + 1 < x1; x += pstep) {
                    int q[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] }, rv, gv, bv;
                    switch (cfa) {
                    case 0:  rv = q[0]; gv = q[1] + q[2]; bv = q[3]; break;
                    case 1:  rv = q[1]; gv = q[0] + q[3]; bv = q[2]; break;
                    case 2:  rv = q[2]; gv = q[0] + q[3]; bv = q[1]; break;
                    default: rv = q[3]; gv = q[1] + q[2]; bv = q[0]; break;
                    }
                    if (rv >= white - 16 || gv / 2 >= white - 16 || bv >= white - 16) clipped += 1;
                    sr += rv - black; sg += gv / 2.0 - black; sb += bv - black; cnt += 1;
                }
            }
            if (cnt < 1 || clipped > 0.02 * cnt) status = 1;
            else if (sg / cnt < 0.03 * range || sr <= 0 || sb <= 0) status = 2;
            pthread_mutex_lock(&g.lock);
            g.wbpick_status = status;
            if (cnt > 0) { g.wbpick_rgb[0] = sr / cnt; g.wbpick_rgb[1] = sg / cnt; g.wbpick_rgb[2] = sb / cnt; }
            g.wbpick_pending = 0;
            g.wbpick_done = 1;
            pthread_cond_broadcast(&g.cond);
            pthread_mutex_unlock(&g.lock);
            if (!meter_on) return;                       /* a frame for the pick only: no meter line */
        }
    }

    /* The spot, when a light reference is set: its own dense pass over the
     * box (the RAW frame is the whole active array), with a step chosen for
     * about a thousand quads however small the loupe made the spot. The
     * whole-frame pass below then only builds the histogram. */
    if (spot_on) {
        int x0 = (int)((spot_x - spot_hw) * w) & ~1, x1 = (int)((spot_x + spot_hw) * w);
        int y0 = (int)((spot_y - spot_hh) * h) & ~1, y1 = (int)((spot_y + spot_hh) * h);
        int sstep;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        if (y1 > h - 1) y1 = h - 1;
        sstep = (int)sqrt((double)(x1 - x0) * (y1 - y0) / 4000.0);   /* ~1000 quads */
        if (sstep < 2) sstep = 2;
        sstep &= ~1;
        for (y = y0; y + 1 < y1; y += sstep) {
            const uint16_t *r0 = (const uint16_t *)(data + (size_t)y * stride);
            const uint16_t *r1 = (const uint16_t *)(data + (size_t)(y + 1) * stride);
            for (x = x0; x + 1 < x1; x += sstep) {
                int q[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] }, gsum, rv, bv, v[3];
                /* the two greens of the quad, whatever the CFA order */
                gsum = (cfa == 0 || cfa == 3) ? q[1] + q[2] : q[0] + q[3];
                sum_g += gsum / 2.0 - black;
                wsum += 1.0;
                /* ... and the spot's own histogram (R, G, B of the quad) */
                switch (cfa) {
                case 0:  rv = q[0]; bv = q[3]; break;
                case 1:  rv = q[1]; bv = q[2]; break;
                case 2:  rv = q[2]; bv = q[1]; break;
                default: rv = q[3]; bv = q[0]; break;
                }
                v[0] = rv; v[1] = gsum / 2; v[2] = bv;
                for (c = 0; c < 3; ++c) {
                    double nv = (v[c] - black) / range;
                    shist[c][nv <= 0 ? 0 : nv >= 1 ? 63 : hist_bin(nv)]++;
                }
            }
        }
    }

    /* The histogram covers what the interface SHOWS (the controller's display
     * window, indraw_view_window: zoom, loupe, pan), not the whole sensor and
     * not the HAL's frame either: what you see is what is measured. The RAW
     * frame ignores crops, so the window is applied here, in frame pixels
     * (the frame taken as the active array). */
    {
        double vw[4];
        int have;
        pthread_mutex_lock(&g.lock);
        have = g.have_view_win;
        memcpy(vw, g.view_win, sizeof vw);
        pthread_mutex_unlock(&g.lock);
        vx0 = 0; vy0 = 0; vx1 = w; vy1 = h;
        if (have) {
            vx0 = (int)(vw[0] * w) & ~1; vy0 = (int)(vw[1] * h) & ~1;
            vx1 = (int)((vw[0] + vw[2]) * w); vy1 = (int)((vw[1] + vw[3]) * h);
            if (vx0 < 0) vx0 = 0;
            if (vy0 < 0) vy0 = 0;
            if (vx1 > w) vx1 = w;
            if (vy1 > h) vy1 = h;
            if (vx1 - vx0 < 4 || vy1 - vy0 < 4) { vx0 = 0; vy0 = 0; vx1 = w; vy1 = h; }
        }
        /* keep ~256 columns of samples across the displayed area */
        step = (vx1 - vx0) / 256; if (step < 2) step = 2; step &= ~1;
    }

    for (y = vy0; y + 1 < vy1; y += step) {
        const uint16_t *r0 = (const uint16_t *)(data + (size_t)y * stride);
        const uint16_t *r1 = (const uint16_t *)(data + (size_t)(y + 1) * stride);
        int centre_y = y >= h / 4 && y < 3 * h / 4;
        for (x = vx0; x + 1 < vx1; x += step) {
            /* The quad's four values, mapped to R, G, G, B by the CFA order:
             * 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR. */
            int q[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] };
            int rv, g1, g2, bv, v[3], k;
            /* Centre-weighted (x3 in the central quarter) when there is no
             * light reference; with one, the mean comes from the spot pass
             * above and this pass only feeds the histogram. */
            double weight = spot_on ? 0.0 : ((centre_y && x >= w / 4 && x < 3 * w / 4) ? 3.0 : 1.0);
            switch (cfa) {
            case 0:  rv = q[0]; g1 = q[1]; g2 = q[2]; bv = q[3]; break;
            case 1:  g1 = q[0]; rv = q[1]; bv = q[2]; g2 = q[3]; break;
            case 2:  g1 = q[0]; bv = q[1]; rv = q[2]; g2 = q[3]; break;
            default: bv = q[0]; g1 = q[1]; g2 = q[2]; rv = q[3]; break;
            }
            v[0] = rv; v[1] = (g1 + g2) / 2; v[2] = bv;
            sum_g += weight * (v[1] - black);
            wsum += weight;
            for (c = 0; c < 3; ++c) {
                double nv = (v[c] - black) / range;
                if (nv <= 0) { lo++; k = 0; }
                else if (nv >= 1) { hi++; k = 63; }
                else k = hist_bin(nv);
                hist[c][k]++;
                n++;
            }
        }
    }
    if (wsum <= 0 || n == 0) return;
    for (c = 0; c < 3; ++c) for (x = 0; x < 64; ++x) if (hist[c][x] > peak) peak = hist[c][x];
    for (c = 0; c < 3; ++c) for (x = 0; x < 64; ++x) if (shist[c][x] > speak) speak = shist[c][x];

    p += snprintf(p, (size_t)(end - p),
                  "meter ts=%lld iso=%d exp_ns=%lld mean=%.5f clip_lo=%.3f clip_hi=%.3f hist=",
                  (long long)ts, iso, (long long)exposure_ns,
                  sum_g / wsum / range, 100.0 * lo / n, 100.0 * hi / n);
    for (c = 0; c < 3 && p < end; ++c) {
        for (x = 0; x < 64 && p < end; ++x)
            p += snprintf(p, (size_t)(end - p), "%s%ld", x ? "," : "", hist[c][x] * 1000 / peak);
        if (c < 2 && p < end) p += snprintf(p, (size_t)(end - p), ";");
    }
    /* the spot's histogram (its own scale: its tones, highlighted on the main one) */
    if (spot_on && p < end) {
        p += snprintf(p, (size_t)(end - p), " shist=");
        for (c = 0; c < 3 && p < end; ++c) {
            for (x = 0; x < 64 && p < end; ++x)
                p += snprintf(p, (size_t)(end - p), "%s%ld", x ? "," : "", shist[c][x] * 1000 / speak);
            if (c < 2 && p < end) p += snprintf(p, (size_t)(end - p), ";");
        }
    }
    if (p < end - 1) { *p++ = '\n'; *p = '\0'; }
    emit_event(line);
}

/* RAW16 available: a photo's frame is handed over to indraw_raw() (when a
 * capture is in progress); otherwise it is a meter frame, measured here and
 * released at once (never stored, never written). */
static void on_raw_image(void *ctx, AImageReader *reader)
{
    AImage *img = NULL;
    int busy, iso;
    int64_t ts = -1, mts, mexp;
    (void)ctx;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;
    pthread_mutex_lock(&g.lock);
    busy = g.capture_busy;
    if (busy && g.meter_inflight > 0) {
        /* A photo is waiting for the RAW stream (meter_quiesce) and this is
         * the meter frame still in flight: it frees the slot. Up to 0.47.71
         * it was taken for the photo's image and the slot never freed, so
         * meter_quiesce ran into its 1 s timeout before EVERY RAW with the
         * meter on: the RAW flashed ~1 s later than the JPEG (log, 26 Oct:
         * command to reply 1511 ms, the capture itself 511 ms). The photo's
         * own capture is sent only after meter_quiesce returns. */
        g.meter_inflight--;
        pthread_cond_broadcast(&g.cond);
        pthread_mutex_unlock(&g.lock);
        AImage_delete(img);
        return;
    }
    if (busy) {
        if (g.raw_image) AImage_delete(g.raw_image);   /* stale one: drop          */
        g.raw_image = img;
        pthread_cond_broadcast(&g.cond);
        pthread_mutex_unlock(&g.lock);
        return;
    }
    if (g.meter_inflight > 0) g.meter_inflight--;
    mts = g.meter_ts; iso = g.meter_iso; mexp = g.meter_exposure_ns;
    pthread_mutex_unlock(&g.lock);
    AImage_getTimestamp(img, &ts);
    /* The result normally lands before the image; if not, report its values as
     * unknown (-1) rather than pair the frame with an older result. */
    if (mts != ts) { iso = -1; mexp = -1; }
    meter_frame(img, ts, iso, mexp);
    AImage_delete(img);
    pthread_mutex_lock(&g.lock);
    pthread_cond_broadcast(&g.cond);                   /* wake meter_quiesce()      */
    pthread_mutex_unlock(&g.lock);
}

/* Still capture finished: copy what the camera really applied.
 * The result object is only valid during this callback. */
/* Sends the "captured" event still owed for the photo in progress, if any:
 * called on its result and on its image, the first one sends it. */
static void captured_now(void)
{
    const char *ev;
    pthread_mutex_lock(&g.lock);
    ev = g.captured_owed;
    g.captured_owed = NULL;
    pthread_mutex_unlock(&g.lock);
    if (ev) emit_event(ev);
}

static void on_capture_completed(void *ctx, ACameraCaptureSession *s,
                                 ACaptureRequest *req, const ACameraMetadata *res)
{
    ACameraMetadata_const_entry e;
    (void)ctx; (void)s; (void)req;
    pthread_mutex_lock(&g.lock);
    g.r_timestamp = g.r_exposure_ns = g.r_frame_ns = -1;
    g.r_fps[0] = g.r_fps[1] = -1;
    g.r_control = -1;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT32 && e.count == 2) {
        g.r_fps[0] = e.data.i32[0];
        g.r_fps[1] = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_MODE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE && e.count)
        g.r_control = e.data.u8[0];      /* -1 / NaN = "not provided"     */
    g.r_iso = -1;
    g.r_ae_state = g.r_af_state = -1;
    g.r_nr = g.r_edge = -1;
    g.r_focus_diopters = -1.0f;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT64 && e.count)
        g.r_timestamp = e.data.i64[0];         /* matches AImage_getTimestamp   */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT64 && e.count)
        g.r_exposure_ns = e.data.i64[0];       /* shutter time in ns            */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_FRAME_DURATION, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT64 && e.count)
        g.r_frame_ns = e.data.i64[0];          /* >= exposure                   */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_INT32 && e.count)
        g.r_iso = e.data.i32[0];               /* ISO                           */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE && e.count)
        g.r_ae_state = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE && e.count)
        g.r_af_state = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_FLOAT && e.count)
        g.r_focus_diopters = isfinite(e.data.f[0]) ? e.data.f[0] : -1.0f; /* no nan/inf */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_NOISE_REDUCTION_MODE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE && e.count)
        g.r_nr = e.data.u8[0];                 /* NR really used                */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_EDGE_MODE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE && e.count)
        g.r_edge = e.data.u8[0];               /* sharpening really used        */
    if (g.r_meta) ACameraMetadata_free(g.r_meta);
    g.r_meta = ACameraMetadata_copy(res);      /* whole result, for the RAW sidecar */
    g.result_ready = 1;                        /* publish                       */
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    captured_now();                            /* the frame is read: "you may move" */
}

/* Still capture failed: wake indraw_jpeg() instead of letting it time out. */
static void on_capture_failed(void *ctx, ACameraCaptureSession *s,
                              ACaptureRequest *req, ACameraCaptureFailure *f)
{
    (void)ctx; (void)s; (void)req;
    pthread_mutex_lock(&g.lock);
    g.capture_failed = 1;
    g.capture_fail_reason = f ? f->reason : -1; /* CAPTURE_FAILURE_REASON_*      */
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* Result of a preview (or trigger) frame: track live AE/AF states.
 * A result echoes the controls of its request, so the frame that carried our
 * trigger is recognised by its AF_TRIGGER / AE_PRECAPTURE_TRIGGER = START. */
static float focus_untranslate(float hal_d);   /* below: calibration inverse */

/* --- The white balance as a colour temperature -----------------------------
 * The HAL's colour correction gains make a neutral surface neutral: that
 * surface's camera RGB is therefore proportional to 1/gains. Its correlated
 * colour temperature and Duv come from the sensor's two calibration
 * matrices, the DNG SDK's way (colortemp.h, host-tested): what the
 * interface shows as "5200K" and the tint, for the automatic white balance
 * and for the manual shifts alike (the result's gains are the ones applied). */
/* (colortemp.h is included near the top, with shading.h) */
static int ct_read_matrix(uint32_t tag, double m[9])
{
    ACameraMetadata_const_entry e;
    int i;
    if (ACameraMetadata_getConstEntry(g.chars, tag, &e) != ACAMERA_OK || e.count != 9) return -1;
    for (i = 0; i < 9; ++i) m[i] = e.data.r[i].denominator ? (double)e.data.r[i].numerator / e.data.r[i].denominator : 0;
    return 0;
}
static void setup_colour_temperature(void)
{
    ACameraMetadata_const_entry e;
    g.have_ct = 0; g.ct_t1 = g.ct_t2 = 0;
    if (ct_read_matrix(ACAMERA_SENSOR_COLOR_TRANSFORM1, g.ct_m1)) return;
    g.ct_t1 = 6504;                                  /* an unknown illuminant: assume D65 */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK && e.count)
        g.ct_t1 = ct_illuminant_k(e.data.u8[0]) > 0 ? ct_illuminant_k(e.data.u8[0]) : 6504;
    if (ct_read_matrix(ACAMERA_SENSOR_COLOR_TRANSFORM2, g.ct_m2) == 0 &&
        ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, &e) == ACAMERA_OK && e.count)
        g.ct_t2 = ct_illuminant_k(e.data.u8[0]);
    g.have_ct = 1;
}
static void colour_temperature(const float gains[4], int *cct, double *duv)
{
    double n[3];
    *cct = 0; *duv = 0;
    if (!g.have_ct || gains[0] <= 0 || gains[3] <= 0 || gains[1] + gains[2] <= 0) return;
    n[0] = 1.0 / gains[0]; n[1] = 2.0 / (gains[1] + gains[2]); n[2] = 1.0 / gains[3];
    ct_estimate(g.ct_m1, g.ct_t1, g.ct_t2 > 0 ? g.ct_m2 : NULL, g.ct_t2, n, cct, duv);
}

/* Remembers the crop a result's frame was taken with, by its sensor
 * timestamp (called under g.lock), so the frame can be announced with it
 * ("fcrop"). Every request that produces a viewfinder frame must record
 * here: the preview request AND the meter request (its preview frame, one
 * in a few, used to reach the UI with no crop, which made the view beat). */
static void record_result_crop(const ACameraMetadata *res)
{
    ACameraMetadata_const_entry te, ce;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_TIMESTAMP, &te) == ACAMERA_OK && te.count &&
        ACameraMetadata_getConstEntry(res, ACAMERA_SCALER_CROP_REGION, &ce) == ACAMERA_OK && ce.count == 4) {
        ACameraMetadata_const_entry ie, xe;
        int k = g.crop_ring_pos++ & 7;
        g.crop_ring[k].ts = te.data.i64[0];
        memcpy(g.crop_ring[k].crop, ce.data.i32, sizeof g.crop_ring[k].crop);
        /* ... and the pair the frame was exposed with, so its measure can be
         * tagged with it (the loop's settle check then needs no guessing) */
        g.crop_ring[k].iso = (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_SENSITIVITY, &ie) == ACAMERA_OK && ie.count) ? ie.data.i32[0] : 0;
        g.crop_ring[k].exp_ns = (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_EXPOSURE_TIME, &xe) == ACAMERA_OK && xe.count) ? xe.data.i64[0] : 0;
    }
}

static void on_preview_result(void *ctx, ACameraCaptureSession *s,
                              ACaptureRequest *req, const ACameraMetadata *res)
{
    ACameraMetadata_const_entry e;
    int started = 0;
    (void)ctx; (void)s; (void)req;
    pthread_mutex_lock(&g.lock);
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AF_TRIGGER, &e) == ACAMERA_OK &&
        e.count && e.data.u8[0] == ACAMERA_CONTROL_AF_TRIGGER_START)
        started = 1;                                   /* our AF trigger frame   */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, &e) ==
            ACAMERA_OK && e.count && e.data.u8[0] == ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START)
        started = 1;                                   /* our AE trigger frame   */
    if (started) g.trigger_seen = 1;
    if (g.trigger_seen) g.live_results++;              /* counts from the trigger */
    g.live_ae = g.live_af = -1;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK && e.count)
        g.live_ae = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK && e.count)
        g.live_af = e.data.u8[0];
    g.live_lens_state = -1;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_STATE, &e) == ACAMERA_OK && e.count)
        g.live_lens_state = e.data.u8[0];           /* 0 stationary, 1 moving     */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
        g.live_focus = e.data.f[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count)
        g.live_exp_ns = e.data.i64[0];          /* for a burst in auto: freeze this */
    record_result_crop(res);                   /* for "fcrop" with this frame      */
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count)
        g.live_iso_v = e.data.i32[0];
    /* Remember the AWB's own gains and transform while it runs: manual white
     * balance (indraw_wb_manual) starts from them, so a temperature/tint
     * setting is a controlled shift from the HAL's neutral, not a guess. */
    {
        ACameraMetadata_const_entry ge, te, me;
        if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AWB_MODE, &me) == ACAMERA_OK && me.count &&
            me.data.u8[0] != ACAMERA_CONTROL_AWB_MODE_OFF &&
            ACameraMetadata_getConstEntry(res, ACAMERA_COLOR_CORRECTION_GAINS, &ge) == ACAMERA_OK && ge.count == 4 &&
            ACameraMetadata_getConstEntry(res, ACAMERA_COLOR_CORRECTION_TRANSFORM, &te) == ACAMERA_OK && te.count == 9) {
            int k;
            for (k = 0; k < 4; ++k) g.awb_gains[k] = ge.data.f[k];
            for (k = 0; k < 9; ++k) g.awb_transform[k] = te.data.r[k];
            g.awb_seen = 1;
        }
        {   /* the watched vendor keys, as this result reports them (g.lock held) */
            int vi;
            for (vi = 0; vi < g.vw_n; ++vi) {
                ACameraMetadata_const_entry ve;
                if (ACameraMetadata_getConstEntry(res, g.vw_tag[vi], &ve) != ACAMERA_OK || !ve.count) {
                    snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "absent");
                    continue;
                }
                switch (ve.type) {
                case ACAMERA_TYPE_BYTE:  snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "%u", ve.data.u8[0]); break;
                case ACAMERA_TYPE_INT32: snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "%d", ve.data.i32[0]); break;
                case ACAMERA_TYPE_FLOAT: snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "%g", ve.data.f[0]); break;
                case ACAMERA_TYPE_INT64: snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "%lld", (long long)ve.data.i64[0]); break;
                default:                 snprintf(g.vw_value[vi], sizeof g.vw_value[vi], "type %d", ve.type); break;
                }
            }
        }
        /* the white balance in force (automatic or shifted), as a temperature
         * (g.lock is already held here: this whole block is under it) */
        if (ACameraMetadata_getConstEntry(res, ACAMERA_COLOR_CORRECTION_GAINS, &ge) == ACAMERA_OK && ge.count == 4)
            colour_temperature(ge.data.f, &g.live_cct, &g.live_duv);
    }
    g.ae_results++;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);

    /* Live exposure for the UI (SF2M "ae" line), every 3rd result (~10 Hz at
     * 30 fps): the ISO/exposure the preview really used (the HAL's choice in
     * auto, ours in manual), the 3A states and the lens position. Emitted
     * outside g.lock: emit_event takes out_lock, which the preview image
     * thread holds while writing a frame. */
    if (g.ae_results % 3 == 0) {
        char line[256];
        long long iso = -1, exp_ns = -1;
        int ae_mode = -1, ae_state = -1, af_state = -1, ev = 0;
        float focus = -1.0f;
        if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count)
            iso = e.data.i32[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count)
            exp_ns = e.data.i64[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_MODE, &e) == ACAMERA_OK && e.count)
            ae_mode = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK && e.count)
            ae_state = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK && e.count)
            af_state = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, &e) == ACAMERA_OK && e.count)
            ev = e.data.i32[0];
        if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count &&
            isfinite(e.data.f[0]))
            focus = e.data.f[0];
        {   /* the lens value as a real distance (calibration inverted), and
             * the white balance in force, read under the lock */
            float focus_real = -1.0f;
            int cct;
            double duv;
            pthread_mutex_lock(&g.lock);
            if (focus >= 0.0f) focus_real = focus_untranslate(focus);
            cct = g.live_cct; duv = g.live_duv;
            pthread_mutex_unlock(&g.lock);
            snprintf(line, sizeof line,
                     "ae iso=%lld exp_ns=%lld ae_mode=%d ae_state=%d af_state=%d ev=%d focus=%.3f focus_real=%.3f cct=%d duv=%.4f\n",
                     iso, exp_ns, ae_mode, ae_state, af_state, ev, focus, focus_real, cct, duv);
        }
        if (g.state_events) emit_event(line);     /* off in run mode: too noisy (read
                                                   * without out_lock: a flag set once
                                                   * at start, a stale read is harmless) */
    }
}

/* Result of a meter capture: remember ISO/exposure for the frame that follows
 * (paired by SENSOR_TIMESTAMP in on_raw_image). */
static void on_meter_result(void *ctx, ACameraCaptureSession *s,
                            ACaptureRequest *req, const ACameraMetadata *res)
{
    ACameraMetadata_const_entry e;
    (void)ctx; (void)s; (void)req;
    pthread_mutex_lock(&g.lock);
    record_result_crop(res);
    g.meter_ts = -1; g.meter_iso = -1; g.meter_exposure_ns = -1;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count)
        g.meter_ts = e.data.i64[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count)
        g.meter_iso = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count)
        g.meter_exposure_ns = e.data.i64[0];
    pthread_mutex_unlock(&g.lock);
}

/* A meter capture failed: its image will never come, free the slot. */
static void on_meter_failed(void *ctx, ACameraCaptureSession *s,
                            ACaptureRequest *req, ACameraCaptureFailure *f)
{
    (void)ctx; (void)s; (void)req; (void)f;
    pthread_mutex_lock(&g.lock);
    if (g.meter_inflight > 0) g.meter_inflight--;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* Callback tables, static so they outlive any single call. */
static ACameraCaptureSession_captureCallbacks g_preview_cbs = {
    .onCaptureCompleted = on_preview_result,
};
static ACameraCaptureSession_captureCallbacks g_meter_cbs = {
    .onCaptureCompleted = on_meter_result,
    .onCaptureFailed = on_meter_failed,
};
static ACameraCaptureSession_captureCallbacks g_still_cbs = {
    .onCaptureCompleted = on_capture_completed,
    .onCaptureFailed = on_capture_failed,
};

/* ------------------------------------------------------------------------- */
/* DNG preview (a true thumbnail of the raw data)                            */
/* ------------------------------------------------------------------------- */

/* Writes "<dng path without .dng>_vf.ppm": a half-size RGB picture of the
 * DNG's own pixels (one RGB pixel per 2x2 Bayer quad: R, mean of the two
 * greens, B), black-subtracted, corrected for vignetting with the same gain
 * map as the DNG (undosed, so the thumbnail is not darkened for darktable),
 * white-balanced with the as-shot neutral and CLIPPED at 1 (so a saturated
 * pixel is white, not pink), taken to sRGB through the colour matrix
 * (normalised so the balanced neutral is white), and gamma-encoded (1/2.2
 * through a table). Without a matrix: the white balance and the clip alone. The app turns it into the
 * NAME_vf.jpg the galleries show, rotated by the DNG's orientation, and
 * deletes the .ppm. Unlike the viewfinder-frame preview it replaces, it shows
 * what the DNG holds -- a stack's real exposure included. Binary PPM: the
 * simplest format Qt reads natively; ~9 MB for a 4096x3072 sensor, briefly. */
static const uint8_t k_gamma[1024] = {
    0, 11, 15, 18, 21, 23, 25, 26, 28, 30, 31, 32, 34, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 45,
    46, 47, 48, 49, 50, 50, 51, 52, 53, 54, 54, 55, 56, 56, 57, 58, 58, 59, 60, 60, 61, 62, 62, 63,
    63, 64, 65, 65, 66, 66, 67, 68, 68, 69, 69, 70, 70, 71, 71, 72, 72, 73, 73, 74, 74, 75, 75, 76,
    76, 77, 77, 78, 78, 79, 79, 80, 80, 81, 81, 81, 82, 82, 83, 83, 84, 84, 84, 85, 85, 86, 86, 87,
    87, 87, 88, 88, 89, 89, 89, 90, 90, 91, 91, 91, 92, 92, 93, 93, 93, 94, 94, 94, 95, 95, 96, 96,
    96, 97, 97, 97, 98, 98, 98, 99, 99, 99, 100, 100, 101, 101, 101, 102, 102, 102, 103, 103, 103, 104, 104, 104,
    105, 105, 105, 106, 106, 106, 107, 107, 107, 108, 108, 108, 108, 109, 109, 109, 110, 110, 110, 111, 111, 111, 112, 112,
    112, 112, 113, 113, 113, 114, 114, 114, 115, 115, 115, 115, 116, 116, 116, 117, 117, 117, 117, 118, 118, 118, 119, 119,
    119, 119, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 123, 123, 123, 123, 124, 124, 124, 124, 125, 125, 125, 125,
    126, 126, 126, 127, 127, 127, 127, 128, 128, 128, 128, 129, 129, 129, 129, 130, 130, 130, 130, 131, 131, 131, 131, 132,
    132, 132, 132, 133, 133, 133, 133, 134, 134, 134, 134, 135, 135, 135, 135, 136, 136, 136, 136, 137, 137, 137, 137, 138,
    138, 138, 138, 138, 139, 139, 139, 139, 140, 140, 140, 140, 141, 141, 141, 141, 142, 142, 142, 142, 142, 143, 143, 143,
    143, 144, 144, 144, 144, 144, 145, 145, 145, 145, 146, 146, 146, 146, 146, 147, 147, 147, 147, 148, 148, 148, 148, 148,
    149, 149, 149, 149, 149, 150, 150, 150, 150, 151, 151, 151, 151, 151, 152, 152, 152, 152, 152, 153, 153, 153, 153, 154,
    154, 154, 154, 154, 155, 155, 155, 155, 155, 156, 156, 156, 156, 156, 157, 157, 157, 157, 157, 158, 158, 158, 158, 158,
    159, 159, 159, 159, 159, 160, 160, 160, 160, 160, 161, 161, 161, 161, 161, 162, 162, 162, 162, 162, 163, 163, 163, 163,
    163, 164, 164, 164, 164, 164, 165, 165, 165, 165, 165, 165, 166, 166, 166, 166, 166, 167, 167, 167, 167, 167, 168, 168,
    168, 168, 168, 168, 169, 169, 169, 169, 169, 170, 170, 170, 170, 170, 171, 171, 171, 171, 171, 171, 172, 172, 172, 172,
    172, 173, 173, 173, 173, 173, 173, 174, 174, 174, 174, 174, 174, 175, 175, 175, 175, 175, 176, 176, 176, 176, 176, 176,
    177, 177, 177, 177, 177, 177, 178, 178, 178, 178, 178, 179, 179, 179, 179, 179, 179, 180, 180, 180, 180, 180, 180, 181,
    181, 181, 181, 181, 181, 182, 182, 182, 182, 182, 182, 183, 183, 183, 183, 183, 183, 184, 184, 184, 184, 184, 185, 185,
    185, 185, 185, 185, 186, 186, 186, 186, 186, 186, 186, 187, 187, 187, 187, 187, 187, 188, 188, 188, 188, 188, 188, 189,
    189, 189, 189, 189, 189, 190, 190, 190, 190, 190, 190, 191, 191, 191, 191, 191, 191, 192, 192, 192, 192, 192, 192, 192,
    193, 193, 193, 193, 193, 193, 194, 194, 194, 194, 194, 194, 195, 195, 195, 195, 195, 195, 195, 196, 196, 196, 196, 196,
    196, 197, 197, 197, 197, 197, 197, 197, 198, 198, 198, 198, 198, 198, 199, 199, 199, 199, 199, 199, 199, 200, 200, 200,
    200, 200, 200, 201, 201, 201, 201, 201, 201, 201, 202, 202, 202, 202, 202, 202, 202, 203, 203, 203, 203, 203, 203, 204,
    204, 204, 204, 204, 204, 204, 205, 205, 205, 205, 205, 205, 205, 206, 206, 206, 206, 206, 206, 206, 207, 207, 207, 207,
    207, 207, 207, 208, 208, 208, 208, 208, 208, 209, 209, 209, 209, 209, 209, 209, 210, 210, 210, 210, 210, 210, 210, 211,
    211, 211, 211, 211, 211, 211, 212, 212, 212, 212, 212, 212, 212, 213, 213, 213, 213, 213, 213, 213, 213, 214, 214, 214,
    214, 214, 214, 214, 215, 215, 215, 215, 215, 215, 215, 216, 216, 216, 216, 216, 216, 216, 217, 217, 217, 217, 217, 217,
    217, 218, 218, 218, 218, 218, 218, 218, 218, 219, 219, 219, 219, 219, 219, 219, 220, 220, 220, 220, 220, 220, 220, 221,
    221, 221, 221, 221, 221, 221, 221, 222, 222, 222, 222, 222, 222, 222, 223, 223, 223, 223, 223, 223, 223, 223, 224, 224,
    224, 224, 224, 224, 224, 225, 225, 225, 225, 225, 225, 225, 225, 226, 226, 226, 226, 226, 226, 226, 226, 227, 227, 227,
    227, 227, 227, 227, 228, 228, 228, 228, 228, 228, 228, 228, 229, 229, 229, 229, 229, 229, 229, 229, 230, 230, 230, 230,
    230, 230, 230, 230, 231, 231, 231, 231, 231, 231, 231, 232, 232, 232, 232, 232, 232, 232, 232, 233, 233, 233, 233, 233,
    233, 233, 233, 234, 234, 234, 234, 234, 234, 234, 234, 235, 235, 235, 235, 235, 235, 235, 235, 236, 236, 236, 236, 236,
    236, 236, 236, 237, 237, 237, 237, 237, 237, 237, 237, 238, 238, 238, 238, 238, 238, 238, 238, 238, 239, 239, 239, 239,
    239, 239, 239, 239, 240, 240, 240, 240, 240, 240, 240, 240, 241, 241, 241, 241, 241, 241, 241, 241, 242, 242, 242, 242,
    242, 242, 242, 242, 243, 243, 243, 243, 243, 243, 243, 243, 243, 244, 244, 244, 244, 244, 244, 244, 244, 245, 245, 245,
    245, 245, 245, 245, 245, 245, 246, 246, 246, 246, 246, 246, 246, 246, 247, 247, 247, 247, 247, 247, 247, 247, 248, 248,
    248, 248, 248, 248, 248, 248, 248, 249, 249, 249, 249, 249, 249, 249, 249, 249, 250, 250, 250, 250, 250, 250, 250, 250,
    251, 251, 251, 251, 251, 251, 251, 251, 251, 252, 252, 252, 252, 252, 252, 252, 252, 252, 253, 253, 253, 253, 253, 253,
    253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255
};
static int write_dng_preview(const char *dng_path, const dng_params_t *p)
{
    char path[1100], part[1110], dir[1100];
    size_t n = strlen(dng_path);
    const char *slash = strrchr(dng_path, '/');
    const char *base = slash ? slash + 1 : dng_path;
    size_t blen = strlen(base);
    int w2 = p->width / 2, h2 = p->height / 2, x, y, mr = 0, mc = 0;
    double gr = 1.0, gb = 1.0, range = (double)(p->white - p->black);
    FILE *f;
    uint8_t *row;
    float *smap = NULL;                     /* an UNDOSED shading map for the thumbnail */
    double cam2srgb[9];
    int have_c2s = 0;
    if (n < 4 || blen < 4 || range <= 0 || w2 < 1 || h2 < 1 || n + 20 >= sizeof path) return EINVAL;
    /* Into "<folder>/.previews/": a hidden folder the system's indexer skips,
     * so the preview never shows up as a photo of its own in the Jolla
     * gallery; only indRAW's gallery (and a PC) pairs it with its DNG. */
    {
        size_t dlen = (size_t)(base - dng_path);       /* folder part, with its '/'   */
        if (dlen + 10 >= sizeof dir || dlen + blen + 24 >= sizeof path) return EINVAL;
        memcpy(dir, dng_path, dlen);
        memcpy(dir + dlen, ".previews", 10);           /* includes the NUL            */
        mkdir(dir, 0755);
        {   /* ".nomedia" inside: a second, explicit "not for the indexer" sign */
            char nomedia[1120];
            FILE *nm;
            memcpy(nomedia, dir, dlen + 9);
            memcpy(nomedia + dlen + 9, "/.nomedia", 10);
            if (access(nomedia, F_OK) != 0 && (nm = fopen(nomedia, "w"))) fclose(nm);
        }
        memcpy(path, dir, dlen + 9);
        path[dlen + 9] = '/';
        memcpy(path + dlen + 10, base, blen - 4);
        memcpy(path + dlen + 10 + blen - 4, "_vf.ppm", 8);
        memcpy(part, path, dlen + 10 + blen - 4 + 7);
        memcpy(part + dlen + 10 + blen - 4 + 7, ".part", 6);
    }
    /* White balance from the as-shot neutral (R, G, B of a neutral grey, as
     * the camera saw it): multiply R and B so grey comes out grey. */
    if (p->have_neutral && p->neutral_num[0] > 0 && p->neutral_num[2] > 0 && p->neutral_num[1] > 0) {
        double nr = (double)p->neutral_num[0] / p->neutral_den[0], ng = (double)p->neutral_num[1] / p->neutral_den[1],
               nb = (double)p->neutral_num[2] / p->neutral_den[2];
        if (nr > 0 && nb > 0) { gr = ng / nr; gb = ng / nb; }
    }
    /* The shading map, UNDOSED (balance 0, strength 100): the thumbnail shows
     * the corrected brightness, not darkened for darktable. The measured map
     * is recomposed here at the photo's lens position and light; the HAL's is
     * used as it is. */
    if (g.shade_last_measured && g.shade_have) {
        mr = g.shade.rows; mc = g.shade.cols;
        smap = malloc(sizeof(float) * 4 * (size_t)(mr * mc));
        if (smap) shading_compose(&g.shade, g.shade_last_d, g.shade_last_k, 0, 100, g.shade_max_gain, smap);
    } else if (p->shading.have && p->shading.cols >= 2 && p->shading.rows >= 2) {
        mr = p->shading.rows; mc = p->shading.cols;
        smap = malloc(sizeof(float) * 4 * (size_t)(mr * mc));
        if (smap) memcpy(smap, p->shading.gains, sizeof(float) * 4 * (size_t)(mr * mc));
    }
    /* Sensor RGB -> sRGB for the thumbnail: invert ColorMatrix1 (XYZ->camera)
     * to camera->XYZ, compose with XYZ(D65)->sRGB, scale each row so the
     * as-shot neutral comes out white, then fold the neutral in so the matrix
     * takes WHITE-BALANCED camera RGB (the pixel loop applies gr/gb and clips
     * before it). The fallback when the forward matrices are missing: one
     * matrix, right only near its own illuminant (D65). */
    /* The DNG way first (0.47.69), when the HAL gives both colour and both
     * forward matrices: the light's temperature from the as-shot neutral
     * through the two ColorMatrices (ct_estimate), the ForwardMatrix
     * interpolated at it (white-balanced camera RGB -> XYZ D50), then XYZ D50
     * -> sRGB (Bradford-adapted). The one-matrix path below used the D65
     * matrix alone whatever the light: under a 3500 K LED the thumbnail came
     * out yellow-green where darktable shows the scene neutral. */
    if (p->color1.have && p->color2.have && p->forward1.have && p->forward2.have && p->have_neutral &&
        p->neutral_num[0] > 0 && p->neutral_num[1] > 0 && p->neutral_num[2] > 0 &&
        ct_illuminant_k(p->illuminant1) > 0 && ct_illuminant_k(p->illuminant2) > 0) {
        static const double xyzd50_to_srgb[9] = {
            3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427 };
        double c1[9], c2[9], f1[9], f2[9], fm[9], neutral[3], wn[3];
        double t1 = ct_illuminant_k(p->illuminant1), t2 = ct_illuminant_k(p->illuminant2), duv;
        int i, r, cc, kk, cct = 0;
        for (i = 0; i < 9; ++i) {
            c1[i] = p->color1.den[i] ? (double)p->color1.num[i] / p->color1.den[i] : 0;
            c2[i] = p->color2.den[i] ? (double)p->color2.num[i] / p->color2.den[i] : 0;
            f1[i] = p->forward1.den[i] ? (double)p->forward1.num[i] / p->forward1.den[i] : 0;
            f2[i] = p->forward2.den[i] ? (double)p->forward2.num[i] / p->forward2.den[i] : 0;
        }
        for (i = 0; i < 3; ++i) neutral[i] = (double)p->neutral_num[i] / p->neutral_den[i];
        if (ct_estimate(c1, t1, c2, t2, neutral, &cct, &duv) == 0 && cct > 0) {
            ct_matrix_at(f1, t1, f2, t2, (double)cct, fm);
            for (r = 0; r < 3; ++r)
                for (cc = 0; cc < 3; ++cc) {
                    double a = 0;
                    for (kk = 0; kk < 3; ++kk) a += xyzd50_to_srgb[r * 3 + kk] * fm[kk * 3 + cc];
                    cam2srgb[r * 3 + cc] = a;
                }
            /* white-balanced white (1,1,1) -> exactly white */
            for (r = 0; r < 3; ++r) wn[r] = cam2srgb[r * 3] + cam2srgb[r * 3 + 1] + cam2srgb[r * 3 + 2];
            if (wn[0] > 1e-6 && wn[1] > 1e-6 && wn[2] > 1e-6) {
                for (r = 0; r < 3; ++r) for (cc = 0; cc < 3; ++cc) cam2srgb[r * 3 + cc] /= wn[r];
                gr = neutral[1] / neutral[0];        /* the white balance, applied below */
                gb = neutral[1] / neutral[2];
                have_c2s = 1;
            }
        }
    }
    if (!have_c2s && p->color1.have && p->have_neutral && p->neutral_num[0] > 0 && p->neutral_num[1] > 0 && p->neutral_num[2] > 0) {
        double cm[9], inv[9], neutral[3], wn[3];
        int i, r, cc, kk;
        static const double xyz2srgb[9] = {
            3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570 };
        for (i = 0; i < 9; ++i) cm[i] = p->color1.den[i] ? (double)p->color1.num[i] / p->color1.den[i] : 0;
        neutral[0] = (double)p->neutral_num[0] / p->neutral_den[0];
        neutral[1] = (double)p->neutral_num[1] / p->neutral_den[1];
        neutral[2] = (double)p->neutral_num[2] / p->neutral_den[2];
        if (ct_invert3(cm, inv) == 0) {
            for (r = 0; r < 3; ++r)
                for (cc = 0; cc < 3; ++cc) {
                    double a = 0;
                    for (kk = 0; kk < 3; ++kk) a += xyz2srgb[r * 3 + kk] * inv[kk * 3 + cc];
                    cam2srgb[r * 3 + cc] = a;
                }
            for (r = 0; r < 3; ++r) wn[r] = cam2srgb[r * 3] * neutral[0] + cam2srgb[r * 3 + 1] * neutral[1] + cam2srgb[r * 3 + 2] * neutral[2];
            if (wn[0] > 1e-6 && wn[1] > 1e-6 && wn[2] > 1e-6) {
                for (r = 0; r < 3; ++r) for (cc = 0; cc < 3; ++cc) cam2srgb[r * 3 + cc] /= wn[r];
                /* Make it act on WHITE-BALANCED camera RGB: the pixels get the
                 * white balance first and are clipped at 1 (below), so a pixel
                 * the sensor saturated -- all channels at white, which for the
                 * sensor is NOT neutral (green dominates a grey) -- becomes
                 * (1,1,1) and renders white instead of pink (0.47.49). The
                 * white-balanced neutral (1,1,1) is the camera's (nr,1,nb). */
                for (r = 0; r < 3; ++r) {
                    cam2srgb[r * 3 + 0] *= neutral[0];
                    cam2srgb[r * 3 + 2] *= neutral[2];
                }
                gr = neutral[1] / neutral[0];        /* the white balance, applied below */
                gb = neutral[1] / neutral[2];
                have_c2s = 1;
            }
        }
    }
    row = malloc((size_t)w2 * 3);
    if (!row) { free(smap); return ENOMEM; }
    f = fopen(part, "wb");
    if (!f) { free(row); return errno ? errno : EIO; }
    fprintf(f, "P6\n%d %d\n255\n", w2, h2);
    for (y = 0; y < h2; ++y) {
        const uint16_t *r0 = p->pixels + (size_t)(2 * y) * p->width, *r1 = r0 + p->width;
        for (x = 0; x < w2; ++x) {
            int q[4] = { r0[2 * x], r0[2 * x + 1], r1[2 * x], r1[2 * x + 1] }, rv, g1, g2, bv, k;
            double c[3];
            switch (p->cfa) {                      /* 0 RGGB 1 GRBG 2 GBRG 3 BGGR */
            case 0:  rv = q[0]; g1 = q[1]; g2 = q[2]; bv = q[3]; break;
            case 1:  g1 = q[0]; rv = q[1]; bv = q[2]; g2 = q[3]; break;
            case 2:  g1 = q[0]; bv = q[1]; rv = q[2]; g2 = q[3]; break;
            default: bv = q[0]; g1 = q[1]; g2 = q[2]; rv = q[3]; break;
            }
            c[0] = (rv - p->black) / range * gr;   /* white balance first ... */
            c[1] = ((g1 + g2) / 2.0 - p->black) / range;
            c[2] = (bv - p->black) / range * gb;
            if (smap) {                        /* vignetting, bilinear over the map */
                double fy = (double)y / (h2 - 1) * (mr - 1), fx = (double)x / (w2 - 1) * (mc - 1);
                int iy = (int)fy, ix = (int)fx;
                double wy = fy - iy, wx = fx - ix;
                const float *m00, *m01, *m10, *m11;
                int iy1 = iy + 1 < mr ? iy + 1 : iy, ix1 = ix + 1 < mc ? ix + 1 : ix;
                m00 = smap + (iy * mc + ix) * 4; m01 = smap + (iy * mc + ix1) * 4;
                m10 = smap + (iy1 * mc + ix) * 4; m11 = smap + (iy1 * mc + ix1) * 4;
                for (k = 0; k < 3; ++k) {
                    int ch = k == 0 ? 0 : k == 1 ? 1 : 3;   /* R, Ge, B */
                    double g = m00[ch] * (1 - wy) * (1 - wx) + m01[ch] * (1 - wy) * wx
                             + m10[ch] * wy * (1 - wx) + m11[ch] * wy * wx;
                    c[k] *= g;
                }
            }
            for (k = 0; k < 3; ++k)            /* ... then clip the highlights: a */
                if (c[k] > 1.0) c[k] = 1.0;    /* saturated pixel is white, not pink */
            if (have_c2s) {                    /* white-balanced camera RGB -> sRGB */
                double o[3];
                int r;
                for (r = 0; r < 3; ++r) o[r] = cam2srgb[r * 3] * c[0] + cam2srgb[r * 3 + 1] * c[1] + cam2srgb[r * 3 + 2] * c[2];
                c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
            }
            for (k = 0; k < 3; ++k) {
                double v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                int idx = (int)(v * 1023.0 + 0.5);
                if (idx < 0) idx = 0;
                if (idx > 1023) idx = 1023;
                row[3 * x + k] = k_gamma[idx];
            }
        }
        if (fwrite(row, 1, (size_t)w2 * 3, f) != (size_t)w2 * 3) { int e = errno ? errno : EIO; fclose(f); unlink(part); free(row); return e; }
    }
    free(row); free(smap);
    if (fclose(f) != 0 || rename(part, path) != 0) { int e = errno ? errno : EIO; unlink(part); return e; }
    {
        char ev[1200];
        snprintf(ev, sizeof ev, "written kind=preview path=%s orientation=%d\n", path, p->orientation);
        emit_event(ev);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Abort of a running stack                                                  */
/* ------------------------------------------------------------------------- */

/* Set from a signal handler (the CLI's SIGUSR1) or when the CLI's stdin
 * closes: the stack loop checks it after every sub-frame, stops, and writes
 * what it has accumulated so far, labelled with the exposure really reached.
 * A plain volatile flag, nothing a signal handler may not touch. */
static volatile sig_atomic_t g_abort;
IR_API void indraw_request_abort(void) { g_abort = 1; }

/* ------------------------------------------------------------------------- */
/* Exposure meter thread                                                     */
/* ------------------------------------------------------------------------- */

/* Submits one RAW16 measure capture every meter_interval_ms while the meter is
 * on, never more than one in flight, and never while a photo owns the RAW
 * stream (capture_busy). The frame is measured in on_raw_image. */
static void *meter_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g.lock);
    while (!g.meter_stop) {
        struct timespec dl = deadline_in(g.meter_interval_ms > 0 ? g.meter_interval_ms : 200);
        pthread_cond_timedwait(&g.cond, &g.lock, &dl);
        if (g.meter_stop) break;
        if ((g.meter_on || g.wbpick_pending) && g.session && g.meter_request && !g.capture_busy &&
            g.meter_inflight == 0 && streaming()) {
            ACaptureRequest *reqs[1] = { g.meter_request };
            g.meter_inflight = 1;
            pthread_mutex_unlock(&g.lock);
            if (ACameraCaptureSession_capture(g.session, &g_meter_cbs, 1, reqs, NULL) != ACAMERA_OK) {
                pthread_mutex_lock(&g.lock);
                g.meter_inflight = 0;
            } else {
                pthread_mutex_lock(&g.lock);           /* lockcheck: branch (the other arm of the if) */
            }
        }
    }
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

/* Called at the start of every photo that uses the RAW stream (or the HAL
 * heavily): pauses the meter and waits (max 1 s) for an in-flight measure
 * frame, so a photo never receives a meter frame and vice versa. */
static void meter_quiesce(void)
{
    struct timespec dl = deadline_in(1000);
    pthread_mutex_lock(&g.lock);
    g.capture_busy++;
    while (g.meter_inflight > 0)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) { g.meter_inflight = 0; break; }
    pthread_mutex_unlock(&g.lock);
}
/* (meter_quiesce's timeout is now only a safety net: a meter frame in flight
 * frees its slot when its image or its failure arrives.) */

/* The photo is done with the RAW stream: the meter may resume. */
static void meter_release(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.capture_busy > 0) g.capture_busy--;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

/* Stops the meter thread (close). */
static void meter_thread_stop(void)
{
    if (!g.meter_thread_up) return;
    pthread_mutex_lock(&g.lock);
    g.meter_stop = 1;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    pthread_join(g.meter_thread, NULL);
    g.meter_thread_up = 0;
    g.meter_stop = 0;
    g.meter_on = 0;
    g.meter_inflight = 0;
}

/* ------------------------------------------------------------------------- */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------- */

/* Releases everything indraw_open created, in reverse order. Safe on partial state. */
static void close_all(void)
{
    meter_thread_stop();                                /* before the session goes */
    shading_free(&g.shade); free(g.shade_buf); g.shade_buf = NULL; g.shade_have = 0;
    if (g.session) {
        ACameraCaptureSession_stopRepeating(g.session); /* stop the viewfinder   */
        ACameraCaptureSession_close(g.session);         /* then the session      */
        g.session = NULL;
    }
    if (g.device) { ACameraDevice_close(g.device); g.device = NULL; } /* frees the HAL */
    if (g.preview_request) { ACaptureRequest_free(g.preview_request); g.preview_request = NULL; }
    if (g.still_request) { ACaptureRequest_free(g.still_request); g.still_request = NULL; }
    if (g.trigger_request) { ACaptureRequest_free(g.trigger_request); g.trigger_request = NULL; }
    if (g.raw_request) { ACaptureRequest_free(g.raw_request); g.raw_request = NULL; }
    if (g.meter_request) { ACaptureRequest_free(g.meter_request); g.meter_request = NULL; }
    if (g.raw_target) { ACameraOutputTarget_free(g.raw_target); g.raw_target = NULL; }
    if (g.raw_output) { ACaptureSessionOutput_free(g.raw_output); g.raw_output = NULL; }
    if (g.preview_target) { ACameraOutputTarget_free(g.preview_target); g.preview_target = NULL; }
    if (g.jpeg_target) { ACameraOutputTarget_free(g.jpeg_target); g.jpeg_target = NULL; }
    if (g.preview_output) { ACaptureSessionOutput_free(g.preview_output); g.preview_output = NULL; }
    if (g.jpeg_output) { ACaptureSessionOutput_free(g.jpeg_output); g.jpeg_output = NULL; }
    if (g.outputs) { ACaptureSessionOutputContainer_free(g.outputs); g.outputs = NULL; }
    pthread_mutex_lock(&g.lock);
    if (g.jpeg_image) { AImage_delete(g.jpeg_image); g.jpeg_image = NULL; } /* before reader */
    if (g.raw_image) { AImage_delete(g.raw_image); g.raw_image = NULL; }
    if (g.r_meta) { ACameraMetadata_free(g.r_meta); g.r_meta = NULL; }
    pthread_mutex_unlock(&g.lock);
    if (g.preview_reader) { AImageReader_delete(g.preview_reader); g.preview_reader = NULL; }
    if (g.jpeg_reader) { AImageReader_delete(g.jpeg_reader); g.jpeg_reader = NULL; }
    if (g.raw_reader) { AImageReader_delete(g.raw_reader); g.raw_reader = NULL; }
    g.preview_window = g.jpeg_window = g.raw_window = NULL; /* died with readers */
    g.raw_w = g.raw_h = 0;
    if (g.chars) { ACameraMetadata_free(g.chars); g.chars = NULL; }
    if (g.manager) { ACameraManager_delete(g.manager); g.manager = NULL; }
}

/* ------------------------------------------------------------------------- */
/* Exported API                                                              */
/* ------------------------------------------------------------------------- */

/* Library version string, to check that the right .so is installed. */
IR_API const char *indraw_version(void)
{
    return "rawfish-ir " IR_VERSION;   /* the installed name; the code keeps indraw */
}

/* Lists the camera ids the device exposes, as a JSON array of strings. Mirrors
 * RAWfish/minicam: we no longer assume camera "0" exists on every phone. */
/* The inspection entry points (indraw_cameras, indraw_caps) share ONE
 * camera manager per process, created on first use and never deleted: the
 * probe calls them three times in a row (cameras, then caps per camera),
 * and on the Xperia XA2 (32-bit) creating a manager again after deleting
 * one corrupted the heap ('corrupted double-linked list' at exit, 0.47.51).
 * A single 'caps' call, create + delete once, was fine. The camera session
 * has its own manager (g.manager); this one is for inspection only and
 * dies with the process. */
static ACameraManager *inspect_manager(void)
{
    static ACameraManager *mgr;
    if (!mgr) mgr = ACameraManager_create();
    return mgr;
}

IR_API int indraw_cameras(char *out, size_t out_size)
{
    ACameraManager *mgr;                          /* service client               */
    ACameraIdList *list = NULL;                    /* the ids the HAL reports      */
    camera_status_t st;
    jw_t w;
    int i;

    if (!out || !out_size) return IR_EARG;
    mgr = inspect_manager();                      /* shared, kept (see it)        */
    if (!mgr) return fail(out, out_size, IR_ESETUP, "manager", "ACameraManager_create", 0);
    st = ACameraManager_getCameraIdList(mgr, &list); /* enumerate                 */
    if (st != ACAMERA_OK) {
        return fail(out, out_size, IR_ESETUP, "list", "getCameraIdList failed", st);
    }
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"cameras\":[");       /* open the report + array      */
    for (i = 0; i < list->numCameras; ++i) {       /* each id, JSON-escaped        */
        jw_add(&w, "%s", i ? "," : "");
        jw_str(&w, list->cameraIds[i]);
    }
    jw_add(&w, "]}");                              /* close                        */
    ACameraManager_deleteCameraIdList(list);       /* free the list                */
    return IR_OK;
}

/* Describes what camera `id` offers for our purposes (does NOT open it). */
/* Names for the standard Camera2 tags a photo app drives, so the probe's
 * key lists read as words; anything else is given as hex. */
static const char *tag_name(uint32_t tag)
{
    switch (tag) {
    case ACAMERA_CONTROL_AE_MODE: return "control.aeMode";
    case ACAMERA_CONTROL_AE_LOCK: return "control.aeLock";
    case ACAMERA_CONTROL_AE_REGIONS: return "control.aeRegions";
    case ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION: return "control.aeExposureCompensation";
    case ACAMERA_CONTROL_AE_TARGET_FPS_RANGE: return "control.aeTargetFpsRange";
    case ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER: return "control.aePrecaptureTrigger";
    case ACAMERA_CONTROL_AE_ANTIBANDING_MODE: return "control.aeAntibandingMode";
    case ACAMERA_CONTROL_AF_MODE: return "control.afMode";
    case ACAMERA_CONTROL_AF_REGIONS: return "control.afRegions";
    case ACAMERA_CONTROL_AF_TRIGGER: return "control.afTrigger";
    case ACAMERA_CONTROL_AWB_MODE: return "control.awbMode";
    case ACAMERA_CONTROL_AWB_LOCK: return "control.awbLock";
    case ACAMERA_CONTROL_AWB_REGIONS: return "control.awbRegions";
    case ACAMERA_CONTROL_MODE: return "control.mode";
    case ACAMERA_CONTROL_SCENE_MODE: return "control.sceneMode";
    case ACAMERA_CONTROL_EFFECT_MODE: return "control.effectMode";
    case ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE: return "control.videoStabilizationMode";
    case ACAMERA_CONTROL_CAPTURE_INTENT: return "control.captureIntent";
    case ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST: return "control.postRawSensitivityBoost";
    case ACAMERA_CONTROL_ENABLE_ZSL: return "control.enableZsl";
    case ACAMERA_SENSOR_EXPOSURE_TIME: return "sensor.exposureTime";
    case ACAMERA_SENSOR_SENSITIVITY: return "sensor.sensitivity";
    case ACAMERA_SENSOR_FRAME_DURATION: return "sensor.frameDuration";
    case ACAMERA_SENSOR_TEST_PATTERN_MODE: return "sensor.testPatternMode";
    case ACAMERA_LENS_FOCUS_DISTANCE: return "lens.focusDistance";
    case ACAMERA_LENS_APERTURE: return "lens.aperture";
    case ACAMERA_LENS_OPTICAL_STABILIZATION_MODE: return "lens.opticalStabilizationMode";
    case ACAMERA_NOISE_REDUCTION_MODE: return "noiseReduction.mode";
    case ACAMERA_EDGE_MODE: return "edge.mode";
    case ACAMERA_HOT_PIXEL_MODE: return "hotPixel.mode";
    case ACAMERA_SHADING_MODE: return "shading.mode";
    case ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE: return "statistics.lensShadingMapMode";
    case ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE: return "statistics.hotPixelMapMode";
    case ACAMERA_STATISTICS_FACE_DETECT_MODE: return "statistics.faceDetectMode";
    case ACAMERA_TONEMAP_MODE: return "tonemap.mode";
    case ACAMERA_TONEMAP_CURVE_RED: return "tonemap.curveRed";
    case ACAMERA_TONEMAP_CURVE_GREEN: return "tonemap.curveGreen";
    case ACAMERA_TONEMAP_CURVE_BLUE: return "tonemap.curveBlue";
    case ACAMERA_TONEMAP_GAMMA: return "tonemap.gamma";
    case ACAMERA_TONEMAP_PRESET_CURVE: return "tonemap.presetCurve";
    case ACAMERA_COLOR_CORRECTION_MODE: return "colorCorrection.mode";
    case ACAMERA_COLOR_CORRECTION_GAINS: return "colorCorrection.gains";
    case ACAMERA_COLOR_CORRECTION_TRANSFORM: return "colorCorrection.transform";
    case ACAMERA_COLOR_CORRECTION_ABERRATION_MODE: return "colorCorrection.aberrationMode";
    case ACAMERA_BLACK_LEVEL_LOCK: return "blackLevel.lock";
    case ACAMERA_FLASH_MODE: return "flash.mode";
    case ACAMERA_SCALER_CROP_REGION: return "scaler.cropRegion";
    case ACAMERA_JPEG_QUALITY: return "jpeg.quality";
    case ACAMERA_JPEG_ORIENTATION: return "jpeg.orientation";
    case ACAMERA_JPEG_THUMBNAIL_SIZE: return "jpeg.thumbnailSize";
    case ACAMERA_SENSOR_TIMESTAMP: return "sensor.timestamp";
    case ACAMERA_SENSOR_NEUTRAL_COLOR_POINT: return "sensor.neutralColorPoint";
    case ACAMERA_SENSOR_NOISE_PROFILE: return "sensor.noiseProfile";
    case ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL: return "sensor.dynamicBlackLevel";
    case ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL: return "sensor.dynamicWhiteLevel";
    case ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW: return "sensor.rollingShutterSkew";
    case ACAMERA_STATISTICS_LENS_SHADING_MAP: return "statistics.lensShadingMap";
    case ACAMERA_STATISTICS_SCENE_FLICKER: return "statistics.sceneFlicker";
    case ACAMERA_CONTROL_AE_STATE: return "control.aeState";
    case ACAMERA_CONTROL_AF_STATE: return "control.afState";
    case ACAMERA_CONTROL_AWB_STATE: return "control.awbState";
    case ACAMERA_LENS_STATE: return "lens.state";
    case ACAMERA_LENS_FOCUS_RANGE: return "lens.focusRange";
    default: return NULL;
    }
}
static const char *capability_name(uint8_t c)
{
    static const char *const names[] = {
        "backward_compatible", "manual_sensor", "manual_post_processing", "raw",
        "private_reprocessing", "read_sensor_settings", "burst_capture", "yuv_reprocessing",
        "depth_output", "constrained_high_speed_video", "motion_tracking",
        "logical_multi_camera", "monochrome", "secure_image_data", "system_camera",
        "offline_processing" };
    return c < sizeof names / sizeof names[0] ? names[c] : "?";
}
/* An int32 tag list as JSON names (hex for the unnamed), only tags < 0x80000000. */
static void add_tag_list(jw_t *w, const ACameraMetadata *chars, uint32_t list_tag)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    int first = 1;
    if (ACameraMetadata_getConstEntry(chars, list_tag, &e) == ACAMERA_OK && e.type == ACAMERA_TYPE_INT32)
        for (i = 0; i < e.count; ++i) {
            uint32_t t = (uint32_t)e.data.i32[i];
            const char *n;
            if (t >= 0x80000000u) continue;
            n = tag_name(t);
            if (n) jw_add(w, "%s\"%s\"", first ? "" : ",", n);
            else jw_add(w, "%s\"0x%08x\"", first ? "" : ",", t);
            first = 0;
        }
}

IR_API int indraw_caps(const char *id, char *out, size_t out_size)
{
    ACameraManager *mgr;
    ACameraMetadata *chars = NULL;
    ACameraMetadata_const_entry e;
    camera_status_t st;
    jw_t w;
    uint32_t i;

    if (!id || !out || !out_size) return IR_EARG;
    mgr = inspect_manager();                                         /* shared, kept   */
    if (!mgr) return fail(out, out_size, IR_ESETUP, "manager", "ACameraManager_create", 0);
    st = ACameraManager_getCameraCharacteristics(mgr, id, &chars);   /* static info    */
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_EARG, "characteristics", "unknown camera id", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"camera_id\":");
    jw_str(&w, id);
    jw_add(&w, ",\"jpeg\":");
    add_sizes(&w, chars, AIMAGE_FORMAT_JPEG);                        /* photo sizes    */
    jw_add(&w, ",\"raw16\":");
    add_sizes(&w, chars, AIMAGE_FORMAT_RAW16);                       /* RAW sizes      */
    jw_add(&w, ",\"yuv\":");
    add_sizes(&w, chars, AIMAGE_FORMAT_YUV_420_888);                 /* preview sizes  */
    jw_add(&w, ",\"noise_modes\":[");                                   /* noise reduction */
    if (ACameraMetadata_getConstEntry(chars,
            ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s\"%s\"", i ? "," : "", nr_name(e.data.u8[i]));
    jw_add(&w, "],\"edge_modes\":[");                               /* sharpening     */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_EDGE_AVAILABLE_EDGE_MODES, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s\"%s\"", i ? "," : "", edge_name(e.data.u8[i]));
    jw_add(&w, "],\"iso_range\":");                                  /* manual ISO     */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT32 && e.count == 2)
        jw_add(&w, "[%d,%d]", e.data.i32[0], e.data.i32[1]);
    else
        jw_add(&w, "null");
    jw_add(&w, ",\"exposure_ns_range\":");                           /* manual speed   */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT64 && e.count == 2)
        jw_add(&w, "[%lld,%lld]", (long long)e.data.i64[0], (long long)e.data.i64[1]);
    else
        jw_add(&w, "null");
    jw_add(&w, ",\"min_focus_diopters\":");                          /* manual focus   */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_FLOAT && e.count)
        jw_add(&w, "%.3f", e.data.f[0]);
    else
        jw_add(&w, "null");
    /* How trustworthy LENS_FOCUS_DISTANCE is: calibrated (diopters are real
     * metres), approximate (roughly right), or uncalibrated (relative only). */
    jw_add(&w, ",\"focus_distance_calibration\":");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &e) ==
            ACAMERA_OK && e.count) {
        const char *c = e.data.u8[0] == ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_CALIBRATED ? "calibrated"
                      : e.data.u8[0] == ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE ? "approximate"
                      : e.data.u8[0] == ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED ? "uncalibrated"
                      : "unknown";
        jw_str(&w, c);
    } else {
        jw_add(&w, "null");
    }
    jw_add(&w, ",\"af_modes\":[");                                  /* focus modes    */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AF_AVAILABLE_MODES, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s{\"name\":\"%s\",\"value\":%u}", i ? "," : "",
                   af_mode_name(e.data.u8[i]), e.data.u8[i]);
    jw_add(&w, "]");
    /* Stream timing: shortest frame duration and JPEG/RAW stall, per size. */
    jw_add(&w, ",\"min_frame_ns\":{\"jpeg\":");
    add_durations(&w, chars, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, AIMAGE_FORMAT_JPEG);
    jw_add(&w, ",\"raw16\":");
    add_durations(&w, chars, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, AIMAGE_FORMAT_RAW16);
    jw_add(&w, ",\"yuv\":");
    add_durations(&w, chars, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, AIMAGE_FORMAT_YUV_420_888);
    jw_add(&w, "},\"stall_ns\":{\"jpeg\":");
    add_durations(&w, chars, ACAMERA_SCALER_AVAILABLE_STALL_DURATIONS, AIMAGE_FORMAT_JPEG);
    jw_add(&w, ",\"raw16\":");
    add_durations(&w, chars, ACAMERA_SCALER_AVAILABLE_STALL_DURATIONS, AIMAGE_FORMAT_RAW16);
    jw_add(&w, "},\"max_frame_ns\":");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT64 && e.count)
        jw_add(&w, "%lld", (long long)e.data.i64[0]);
    else
        jw_add(&w, "null");
    /* Vignetting: can the camera report a shading map, and is RAW pre-corrected? */
    jw_add(&w, ",\"shading_map_size\":");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_SHADING_MAP_SIZE, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT32 && e.count == 2)
        jw_add(&w, "[%d,%d]", e.data.i32[0], e.data.i32[1]);
    else
        jw_add(&w, "null");
    jw_add(&w, ",\"shading_map_reportable\":%s",
           u8_list_has(chars, ACAMERA_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
                       ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON) ? "true" : "false");
    jw_add(&w, ",\"raw_shading_applied\":");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED, &e) ==
            ACAMERA_OK && e.count)
        jw_add(&w, "%s", e.data.u8[0] ? "true" : "false");
    else
        jw_add(&w, "null");
    jw_add(&w, ",\"ae_fps_ranges\":[");                            /* [min,max] pairs */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT32)
        for (i = 0; i + 1 < e.count; i += 2)
            jw_add(&w, "%s[%d,%d]", i ? "," : "", e.data.i32[i], e.data.i32[i + 1]);
    jw_add(&w, "],\"control_modes\":[");                            /* 0 OFF 1 AUTO ... */
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AVAILABLE_MODES, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s%u", i ? "," : "", e.data.u8[i]);
    /* What this HAL declares it can do and will take. The hardware level
     * (legacy / limited / full / 3) frames it; the capabilities say which
     * families of controls are honoured (manual_sensor: exposure, ISO, frame
     * duration; manual_post_processing: noise, edge, tonemap, colour; raw;
     * read_sensor_settings: the applied values come back); the request keys
     * are the standard controls a request may carry, the result keys what a
     * capture result reports. Named where a photo app drives them. */
    jw_add(&w, "],\"hardware_level\":");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK && e.count) {
        static const char *const lv[] = { "limited", "full", "legacy", "3", "external" };
        jw_add(&w, "\"%s\"", e.data.u8[0] < 5 ? lv[e.data.u8[0]] : "?");
    } else jw_add(&w, "null");
    jw_add(&w, ",\"capabilities\":[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK && e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i) jw_add(&w, "%s\"%s\"", i ? "," : "", capability_name(e.data.u8[i]));
    jw_add(&w, "],\"request_keys\":[");
    add_tag_list(&w, chars, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS);
    jw_add(&w, "],\"result_keys\":[");
    add_tag_list(&w, chars, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS);
    /* Vendor (chipset) request keys: tag ids >= 0x80000000, as hex. */
    jw_add(&w, "],\"vendor_request_keys\":[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT32) {
        int first = 1;
        for (i = 0; i < e.count; ++i)
            if ((uint32_t)e.data.i32[i] >= 0x80000000u) {
                jw_add(&w, "%s\"0x%08x\"", first ? "" : ",", (uint32_t)e.data.i32[i]);
                first = 0;
            }
    }
    jw_add(&w, "]");
    /* Keys the HAL wants at SESSION creation (Camera2 session parameters): a
     * key listed here is read when the session is configured, not per frame
     * -- changing it in a request may do nothing. */
    jw_add(&w, ",\"session_keys\":[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_INT32)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s\"0x%08x\"", i ? "," : "", (uint32_t)e.data.i32[i]);
    jw_add(&w, "]");
    /* The VALUES of the vendor characteristics (tags >= 0x80000000, e.g.
     * MediaTek's "available3dnrmodes"): what a vendor request may take. Up to
     * 16 values each, in their own type. */
    jw_add(&w, ",\"vendor_chars\":{");
    {
        int32_t ntags = 0;
        const uint32_t *tags = NULL;
        int first = 1;
        if (ACameraMetadata_getAllTags(chars, &ntags, &tags) == ACAMERA_OK && tags)
            for (i = 0; i < (uint32_t)ntags; ++i) {
                ACameraMetadata_const_entry ve;
                uint32_t k, n;
                if (tags[i] < 0x80000000u || ACameraMetadata_getConstEntry(chars, tags[i], &ve) != ACAMERA_OK) continue;
                jw_add(&w, "%s\"0x%08x\":[", first ? "" : ",", tags[i]);
                first = 0;
                n = ve.count < 16 ? ve.count : 16;
                for (k = 0; k < n; ++k) {
                    const char *sep = k ? "," : "";
                    switch (ve.type) {
                    case ACAMERA_TYPE_BYTE:     jw_add(&w, "%s%u", sep, ve.data.u8[k]); break;
                    case ACAMERA_TYPE_INT32:    jw_add(&w, "%s%d", sep, ve.data.i32[k]); break;
                    case ACAMERA_TYPE_FLOAT:    jw_add(&w, "%s%g", sep, ve.data.f[k]); break;
                    case ACAMERA_TYPE_INT64:    jw_add(&w, "%s%lld", sep, (long long)ve.data.i64[k]); break;
                    case ACAMERA_TYPE_DOUBLE:   jw_add(&w, "%s%g", sep, ve.data.d[k]); break;
                    case ACAMERA_TYPE_RATIONAL: jw_add(&w, "%s\"%d/%d\"", sep, ve.data.r[k].numerator, ve.data.r[k].denominator); break;
                    default: break;
                    }
                }
                if (ve.count > 16) jw_add(&w, ",\"...%u\"", ve.count);
                jw_add(&w, "]");
            }
    }
    jw_add(&w, "}");
    jw_add(&w, ",\"raw_capability\":%s",
           u8_list_has(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
                       ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW) ? "true" : "false");
    /* Grouped controls, listed as {name,value} FOR INFORMATION ONLY. indraw
     * never drives these (it keeps every control atomic); they are reported so a
     * future UI knows what the device offers. What each scene/effect actually
     * does is vendor-defined, so we do not interpret it. */
    jw_add(&w, ",\"scene_modes\":[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s{\"name\":\"%s\",\"value\":%u}", i ? "," : "",
                   scene_name(e.data.u8[i]), e.data.u8[i]);
    jw_add(&w, "],\"effects\":[");
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AVAILABLE_EFFECTS, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_BYTE)
        for (i = 0; i < e.count; ++i)
            jw_add(&w, "%s{\"name\":\"%s\",\"value\":%u}", i ? "," : "",
                   effect_name(e.data.u8[i]), e.data.u8[i]);
    jw_add(&w, "]");
    /* jw_finish closes the object with ,"truncated":true/false} using the
     * reserved tail, so the flag is present even if the body was cut off. */
    jw_finish(&w);
    ACameraMetadata_free(chars);                 /* the manager stays (inspect_manager) */
    return IR_OK;
}

/* Reads a u8 control from a request as created by its template (-1 if unset).
 * Kept so that "default" restores it: the still request is reused, so a
 * setting from one photo would otherwise stick to the next. */
static int template_u8(const ACaptureRequest *req, uint32_t tag)
{
    ACameraMetadata_const_entry e;
    if (ACaptureRequest_getConstEntry(req, tag, &e) != ACAMERA_OK || !e.count) return -1;
    return e.data.u8[0];
}

/* Reads the phone's make and model from Android properties into g.make/g.model,
 * with fallbacks, so the DNG and the noise profile identify the device the same
 * way on any phone (not hard-coded to the JP2). */
static void read_product_id(void)
{
    char v[92];                                   /* PROP_VALUE_MAX on bionic     */
    v[0] = '\0';
    if (__system_property_get("ro.product.manufacturer", v) > 0 && v[0])
        snprintf(g.make, sizeof g.make, "%s", v);
    else
        snprintf(g.make, sizeof g.make, "unknown");
    v[0] = '\0';
    if (__system_property_get("ro.product.model", v) > 0 && v[0])
        snprintf(g.model, sizeof g.model, "%s", v);
    else
        snprintf(g.model, sizeof g.model, "unknown");
}

/* Opens camera `id`, attaches a YUV preview stream (pw x ph), a JPEG stream
 * (jw x jh) and, if rw x rh is not 0 x 0, a RAW16 stream to one session,
 * starts the preview and waits for its first frame. */
IR_API int indraw_open(const char *id, int jw, int jh, int pw, int ph, int rw, int rh,
                   char *out, size_t out_size)
{
    ACameraDevice_StateCallbacks dev_cb = { NULL, on_disconnected, on_device_error };
    ACameraCaptureSession_stateCallbacks ses_cb = { NULL, on_session_noop,
                                                    on_session_noop, on_session_noop };
    AImageReader_ImageListener prev_l = { NULL, on_preview_image };
    AImageReader_ImageListener jpeg_l = { NULL, on_jpeg_image };
    AImageReader_ImageListener raw_l = { NULL, on_raw_image };
    ACaptureRequest *reqs[1];
    camera_status_t st;
    media_status_t ms;
    double t0, t_open, t_session, t_first = -1;
    struct timespec dl;
    uint8_t u8;
    ACameraMetadata_const_entry e;           /* for reading characteristics       */
    jw_t w;

#define SETUP_FAIL(stage, what, code) \
    do { int c_ = (int)(code); close_all(); return fail(out, out_size, IR_ESETUP, stage, what, c_); } while (0)

    /* -1 x -1 = let the engine pick: the largest JPEG / RAW16 size, and for
     * the preview the largest YUV size no wider than 1280 (so any camera,
     * front included, opens without the UI knowing its size list). */
    if (!id || !out || !out_size || (jw <= 0 && jw != -1) || (jh <= 0 && jh != -1) ||
        (pw <= 0 && pw != -1) || (ph <= 0 && ph != -1) ||
        (rw < -1) || (rh < -1) || (rw == 0) != (rh == 0)) return IR_EARG;
    if (g.device) return fail(out, out_size, IR_ESTATE, "open", "already open", 0);
    sync_init();
    pthread_mutex_lock(&g.lock);                  /* reset callback-written state  */
    g.preview_frames = 0;
    g.device_error = 0;
    pthread_mutex_unlock(&g.lock);

    t0 = now_ms();
    g.manager = ACameraManager_create();
    if (!g.manager) SETUP_FAIL("manager", "ACameraManager_create", 0);
    st = ACameraManager_getCameraCharacteristics(g.manager, id, &g.chars);
    if (st != ACAMERA_OK) {
        close_all();
        return fail(out, out_size, IR_EARG, "characteristics", "unknown camera id", st);
    }
    if (jw == -1 && !largest_output_size(g.chars, AIMAGE_FORMAT_JPEG, 0, &jw, &jh)) {
        close_all();
        return fail(out, out_size, IR_EARG, "check", "no jpeg size listed", 0);
    }
    /* The device profile's limits (indraw_set_stream_limits): a JPEG no
     * larger than jpeg_max (a larger one asked becomes the largest within),
     * no RAW stream when raw_stream is off. Nothing changes without them. */
    if (g.lim_jw > 0 && (jw > g.lim_jw || jh > g.lim_jh)) {
        if (!largest_output_size_within(g.chars, AIMAGE_FORMAT_JPEG, g.lim_jw, g.lim_jh, &jw, &jh)) {
            close_all();
            return fail(out, out_size, IR_EARG, "check", "no jpeg size within jpeg_max", 0);
        }
    }
    if (g.lim_raw_off) rw = rh = 0;
    if (pw == -1 && !preview_output_size(g.chars, AIMAGE_FORMAT_YUV_420_888, 1280, jw, jh, &pw, &ph)) {
        close_all();
        return fail(out, out_size, IR_EARG, "check", "no preview size listed", 0);
    }
    if (rw == -1 && !largest_output_size(g.chars, AIMAGE_FORMAT_RAW16, 0, &rw, &rh))
        rw = rh = 0;                                /* no RAW on this camera: skip   */
    /* Reject unsupported sizes BEFORE touching the camera. */
    if (!has_output_size(g.chars, AIMAGE_FORMAT_JPEG, jw, jh)) {
        close_all();
        return fail(out, out_size, IR_EARG, "check", "jpeg size not listed", 0);
    }
    if (!has_output_size(g.chars, AIMAGE_FORMAT_YUV_420_888, pw, ph)) {
        close_all();
        return fail(out, out_size, IR_EARG, "check", "preview size not listed", 0);
    }
    if (rw && !has_output_size(g.chars, AIMAGE_FORMAT_RAW16, rw, rh)) {
        close_all();
        return fail(out, out_size, IR_EARG, "check", "raw16 size not listed", 0);
    }
    /* The lens shading map is a per-frame result; it is only produced if asked
     * for and if the camera lists STATISTICS_LENS_SHADING_MAP_MODE_ON. */
    g.shading_map = u8_list_has(g.chars, ACAMERA_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
                                ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON);
    g.af_continuous = u8_list_has(g.chars, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
                                  ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
    /* Physical max focus diopters (= 1 / min focus distance), the clamp for the
     * calibration table so we never ask the lens past its closest distance. */
    g.max_diopters = 0.0f;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) ==
            ACAMERA_OK && e.type == ACAMERA_TYPE_FLOAT && e.count)
        g.max_diopters = e.data.f[0];
    setup_colour_temperature();              /* the camera->XYZ matrix, for the WB temperature */
    g.sensor_orientation = 0;                /* JPEG_ORIENTATION uses this (JP2: 90) */
    g.device_rot = 0;
    g.wb_fixed = 0; g.wb_shift = 0; g.wb_tint = 0; g.wbpick_pending = 0;
    g.vw_n = 0;
    g.af_tapped = 0; g.af_zoom_on = 0; g.scene_mode = 0;   /* a new session starts with no scene */
    if (!g.shade_balance && !g.shade_strength) { g.shade_balance = 50; g.shade_strength = 100; g.shade_max_gain = 5; }
    g.lens_front = 0;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count)
        g.lens_front = e.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_ORIENTATION, &e) ==
            ACAMERA_OK && e.count)
        g.sensor_orientation = e.data.i32[0];

    /* Image readers: 4 preview buffers (drained at once), 2 JPEG buffers. */
    ms = AImageReader_new(pw, ph, AIMAGE_FORMAT_YUV_420_888, 4, &g.preview_reader);
    if (ms != AMEDIA_OK) SETUP_FAIL("preview_reader", "AImageReader_new", ms);
    ms = AImageReader_new(jw, jh, AIMAGE_FORMAT_JPEG, 2, &g.jpeg_reader);
    if (ms != AMEDIA_OK) SETUP_FAIL("jpeg_reader", "AImageReader_new", ms);
    AImageReader_setImageListener(g.preview_reader, &prev_l);  /* drain + count     */
    AImageReader_setImageListener(g.jpeg_reader, &jpeg_l);     /* hand to indraw_jpeg   */
    ms = AImageReader_getWindow(g.preview_reader, &g.preview_window); /* its surface */
    if (ms != AMEDIA_OK) SETUP_FAIL("preview_window", "AImageReader_getWindow", ms);
    ms = AImageReader_getWindow(g.jpeg_reader, &g.jpeg_window);
    if (ms != AMEDIA_OK) SETUP_FAIL("jpeg_window", "AImageReader_getWindow", ms);
    if (rw) {                                                  /* optional RAW16    */
        ms = AImageReader_new(rw, rh, AIMAGE_FORMAT_RAW16, 2, &g.raw_reader);
        if (ms != AMEDIA_OK) SETUP_FAIL("raw_reader", "AImageReader_new", ms);
        AImageReader_setImageListener(g.raw_reader, &raw_l);   /* hand to indraw_raw    */
        ms = AImageReader_getWindow(g.raw_reader, &g.raw_window);
        if (ms != AMEDIA_OK) SETUP_FAIL("raw_window", "AImageReader_getWindow", ms);
    }

    /* Open the device (needs the binder thread pool: see the Sailfish side). */
    st = ACameraManager_openCamera(g.manager, id, &dev_cb, &g.device);
    if (st != ACAMERA_OK) SETUP_FAIL("open_camera", "ACameraManager_openCamera", st);
    t_open = now_ms();

    /* Session with all outputs. Configured once; every photo reuses it. */
    st = ACaptureSessionOutputContainer_create(&g.outputs);
    if (st != ACAMERA_OK) SETUP_FAIL("container", "ACaptureSessionOutputContainer_create", st);
    st = ACaptureSessionOutput_create(g.preview_window, &g.preview_output);
    if (st != ACAMERA_OK) SETUP_FAIL("preview_output", "ACaptureSessionOutput_create", st);
    st = ACaptureSessionOutput_create(g.jpeg_window, &g.jpeg_output);
    if (st != ACAMERA_OK) SETUP_FAIL("jpeg_output", "ACaptureSessionOutput_create", st);
    ACaptureSessionOutputContainer_add(g.outputs, g.preview_output);
    ACaptureSessionOutputContainer_add(g.outputs, g.jpeg_output);
    if (rw) {
        st = ACaptureSessionOutput_create(g.raw_window, &g.raw_output);
        if (st != ACAMERA_OK) SETUP_FAIL("raw_output", "ACaptureSessionOutput_create", st);
        ACaptureSessionOutputContainer_add(g.outputs, g.raw_output);
    }
    /* This is where the HAL accepts or refuses the combination of streams.
     * With session parameters from the device profile, the API 28 variant,
     * looked up at run time (an Android 8.1 base has it not). */
    g.session_params_used = 0;
    if (g.n_session_req > 0) {
        typedef camera_status_t (*create_sp_fn)(ACameraDevice *, const ACaptureSessionOutputContainer *,
                                                const ACaptureRequest *, const ACameraCaptureSession_stateCallbacks *,
                                                ACameraCaptureSession **);
        create_sp_fn create_sp = (create_sp_fn)dlsym(RTLD_DEFAULT, "ACameraDevice_createCaptureSessionWithSessionParameters");
        if (create_sp) {
            ACaptureRequest *sp = NULL;
            if (ACameraDevice_createCaptureRequest(g.device, TEMPLATE_PREVIEW, &sp) == ACAMERA_OK && sp) {
                int n = apply_session_request(sp);
                st = create_sp(g.device, g.outputs, sp, &ses_cb, &g.session);
                ACaptureRequest_free(sp);
                if (st != ACAMERA_OK) SETUP_FAIL("session", "createCaptureSessionWithSessionParameters", st);
                g.session_params_used = n > 0 ? 1 : 0;
            } else SETUP_FAIL("session", "session parameters request", 0);
        } else {
            g.session_params_used = -1;
            st = ACameraDevice_createCaptureSession(g.device, g.outputs, &ses_cb, &g.session);
            if (st != ACAMERA_OK) SETUP_FAIL("session", "ACameraDevice_createCaptureSession", st);
        }
    } else {
        st = ACameraDevice_createCaptureSession(g.device, g.outputs, &ses_cb, &g.session);
        if (st != ACAMERA_OK) SETUP_FAIL("session", "ACameraDevice_createCaptureSession", st);
    }
    t_session = now_ms();

    /* Targets say which outputs a request fills. */
    st = ACameraOutputTarget_create(g.preview_window, &g.preview_target);
    if (st != ACAMERA_OK) SETUP_FAIL("preview_target", "ACameraOutputTarget_create", st);
    st = ACameraOutputTarget_create(g.jpeg_window, &g.jpeg_target);
    if (st != ACAMERA_OK) SETUP_FAIL("jpeg_target", "ACameraOutputTarget_create", st);
    if (rw) {
        st = ACameraOutputTarget_create(g.raw_window, &g.raw_target);
        if (st != ACAMERA_OK) SETUP_FAIL("raw_target", "ACameraOutputTarget_create", st);
    }

    /* Preview request: preview stream only, full auto. */
    st = ACameraDevice_createCaptureRequest(g.device, TEMPLATE_PREVIEW, &g.preview_request);
    if (st != ACAMERA_OK) SETUP_FAIL("preview_request", "createCaptureRequest", st);
    ACaptureRequest_addTarget(g.preview_request, g.preview_target);

    /* Still request: JPEG + preview, so the viewfinder keeps flowing during a photo. */
    st = ACameraDevice_createCaptureRequest(g.device, g.still_template_preview ? TEMPLATE_PREVIEW : TEMPLATE_STILL_CAPTURE, &g.still_request);
    if (st != ACAMERA_OK) SETUP_FAIL("still_request", "createCaptureRequest", st);
    ACaptureRequest_addTarget(g.still_request, g.jpeg_target);
    ACaptureRequest_addTarget(g.still_request, g.preview_target);
    g.tpl_nr = template_u8(g.still_request, ACAMERA_NOISE_REDUCTION_MODE);
    g.tpl_edge = template_u8(g.still_request, ACAMERA_EDGE_MODE);
    g.pv_nr = -1;                            /* preview processing untouched so far   */
    g.pv_edge = -1;
    pthread_mutex_lock(&g.out_lock);         /* the descriptors live under out_lock */
    g.stream_fd = -1;                        /* not streaming unless indraw_stream_* */
    g.event_fd = -1;                         /* no controller unless indraw_set_event_fd */
    pthread_mutex_unlock(&g.out_lock);
    g.tpl_control = template_u8(g.still_request, ACAMERA_CONTROL_MODE);
    {
        ACameraMetadata_const_entry fe;
        uint32_t k;
        g.tpl_fps[0] = g.tpl_fps[1] = -1;
        if (ACaptureRequest_getConstEntry(g.still_request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, &fe) ==
                ACAMERA_OK && fe.count == 2) {
            g.tpl_fps[0] = fe.data.i32[0];
            g.tpl_fps[1] = fe.data.i32[1];
        }
        /* Lowest range: smallest minimum, then smallest maximum. */
        g.low_fps[0] = g.low_fps[1] = -1;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &fe) ==
                ACAMERA_OK && fe.type == ACAMERA_TYPE_INT32)
            for (k = 0; k + 1 < fe.count; k += 2)
                if (g.low_fps[0] < 0 || fe.data.i32[k] < g.low_fps[0] ||
                    (fe.data.i32[k] == g.low_fps[0] && fe.data.i32[k + 1] < g.low_fps[1])) {
                    g.low_fps[0] = fe.data.i32[k];
                    g.low_fps[1] = fe.data.i32[k + 1];
                }
    }

    /* RAW request: RAW16 + preview. Noise/edge modes do not touch RAW data. */
    if (rw) {
        st = ACameraDevice_createCaptureRequest(g.device, g.still_template_preview ? TEMPLATE_PREVIEW : TEMPLATE_STILL_CAPTURE, &g.raw_request);
        if (st != ACAMERA_OK) SETUP_FAIL("raw_request", "createCaptureRequest", st);
        ACaptureRequest_addTarget(g.raw_request, g.raw_target);
        ACaptureRequest_addTarget(g.raw_request, g.preview_target);
        if (g.shading_map) {                     /* ask for the vignetting gains   */
            u8 = ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON;
            ACaptureRequest_setEntry_u8(g.raw_request, ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE, 1, &u8);
        }
        /* Meter request: RAW16 + preview on the PREVIEW template, so a measure
         * frame is exposed exactly like the viewfinder (same AE / manual pair,
         * same AF), and reflects what the photo will be. */
        st = ACameraDevice_createCaptureRequest(g.device, TEMPLATE_PREVIEW, &g.meter_request);
        if (st != ACAMERA_OK) SETUP_FAIL("meter_request", "createCaptureRequest", st);
        ACaptureRequest_addTarget(g.meter_request, g.raw_target);
        ACaptureRequest_addTarget(g.meter_request, g.preview_target);
    }

    /* Trigger request: like the preview, sent once to start/cancel AF and AE. */
    st = ACameraDevice_createCaptureRequest(g.device, TEMPLATE_PREVIEW, &g.trigger_request);
    if (st != ACAMERA_OK) SETUP_FAIL("trigger_request", "createCaptureRequest", st);
    ACaptureRequest_addTarget(g.trigger_request, g.preview_target);

    /* Same AF mode on both requests, so a photo does not restart focusing. */
    if (g.af_continuous) {
        u8 = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        ACaptureRequest_setEntry_u8(g.preview_request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
        ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
        ACaptureRequest_setEntry_u8(g.trigger_request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
        if (g.raw_request)
            ACaptureRequest_setEntry_u8(g.raw_request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
        if (g.meter_request)
            ACaptureRequest_setEntry_u8(g.meter_request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
    }

    /* Neutralise the GROUPED controls on every request, so they can never
     * silently override the atomic ones (noise, edge, exposure...). A scene or
     * an effect would reconfigure several things at once behind our back; we
     * keep CONTROL_MODE = AUTO (so auto-exposure still works) but with no scene
     * and no effect. This is what makes every capture report exactly what was
     * applied. Scenes/effects are listed by caps for a future UI, never driven
     * here. */
    {
        ACaptureRequest *reqs4[5];
        int r, nr4 = 0;
        reqs4[nr4++] = g.preview_request;
        reqs4[nr4++] = g.still_request;
        reqs4[nr4++] = g.trigger_request;
        if (g.raw_request) reqs4[nr4++] = g.raw_request;
        if (g.meter_request) reqs4[nr4++] = g.meter_request;
        for (r = 0; r < nr4; ++r) {
            u8 = ACAMERA_CONTROL_MODE_AUTO;       /* not USE_SCENE_MODE, not OFF  */
            ACaptureRequest_setEntry_u8(reqs4[r], ACAMERA_CONTROL_MODE, 1, &u8);
            u8 = ACAMERA_CONTROL_SCENE_MODE_DISABLED;
            ACaptureRequest_setEntry_u8(reqs4[r], ACAMERA_CONTROL_SCENE_MODE, 1, &u8);
            u8 = ACAMERA_CONTROL_EFFECT_MODE_OFF;
            ACaptureRequest_setEntry_u8(reqs4[r], ACAMERA_CONTROL_EFFECT_MODE, 1, &u8);
        }
    }

    /* Start the viewfinder. */
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK) SETUP_FAIL("repeating", "setRepeatingRequest", st);

    /* Live exposure starts automatic; the meter thread exists but is off until
     * the UI asks for it (indraw_meter). */
    g.pv_exposure_ns = 0; g.pv_iso = 0; g.ev_steps = 0; g.ae_results = 0;
    g.meter_on = 0; g.meter_stop = 0; g.meter_inflight = 0; g.capture_busy = 0;
    g.meter_interval_ms = 200;
    if (g.meter_request && !g.meter_thread_up) {
        if (pthread_create(&g.meter_thread, NULL, meter_main, NULL) == 0) g.meter_thread_up = 1;
    }

    /* Wait (max 3 s) for the first preview frame: proves the pipeline runs. */
    dl = deadline_in(3000);
    pthread_mutex_lock(&g.lock);
    while (g.preview_frames == 0 && !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    if (g.preview_frames > 0) t_first = now_ms();
    pthread_mutex_unlock(&g.lock);
    if (t_first < 0) SETUP_FAIL("first_frame", g.device_error ? "device error" : "no preview frame",
                                g.device_error);

    snprintf(g.camera_id, sizeof g.camera_id, "%s", id);   /* remember for reports */
    read_product_id();                                     /* phone make/model     */
    g.jpeg_w = jw;
    g.preview_w = pw;
    g.preview_h = ph;
    g.jpeg_h = jh;
    g.raw_w = rw;
    g.raw_h = rh;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"camera_id\":");
    jw_str(&w, id);
    jw_add(&w, ",\"jpeg_size\":[%d,%d],\"preview_size\":[%d,%d],"
               "\"raw16_size\":[%d,%d],\"shading_map\":%s,"
               "\"af_continuous\":%s,\"template\":{\"noise\":\"%s\",\"edge\":\"%s\"},"
               "\"session_params\":\"%s\",\"limits\":{\"raw_stream\":%s,\"jpeg_max\":\"%dx%d\"},"
               "\"ms\":{\"open\":%.1f,\"session\":%.1f,\"first_frame\":%.1f}}",
           jw, jh, pw, ph, rw, rh, g.shading_map ? "true" : "false",
           g.af_continuous ? "true" : "false",
           g.tpl_nr >= 0 ? nr_name(g.tpl_nr) : "none", g.tpl_edge >= 0 ? edge_name(g.tpl_edge) : "none",
           g.session_params_used < 0 ? "unsupported" : g.session_params_used ? "applied" : "none",
           g.lim_raw_off ? "false" : "true", g.lim_jw, g.lim_jh,
           t_open - t0, t_session - t0, t_first - t0);
    return IR_OK;
#undef SETUP_FAIL
}

/* Sets or restores the variant controls of a reusable still request. */
static void set_variant(ACaptureRequest *req, int variant)
{
    uint8_t u8;
    if ((variant & IR_V_FPS_LOW) && g.low_fps[0] > 0)
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, g.low_fps);
    else if (g.tpl_fps[0] >= 0)
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, g.tpl_fps);
    if (variant & IR_V_CONTROL_OFF) {
        u8 = ACAMERA_CONTROL_MODE_OFF;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_MODE, 1, &u8);
    } else if (g.scene_mode > 0) {
        /* A scene is on: the capture must stay in USE_SCENE_MODE, or the HAL
         * takes the photo without the scene (the preview had it, the photo
         * never did: the HDR scene changed nothing, 0.47.67). */
        u8 = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_MODE, 1, &u8);
    } else if (g.tpl_control >= 0) {
        u8 = (uint8_t)g.tpl_control;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_MODE, 1, &u8);
    }
}

/* Stops / restarts the repeating preview around a capture (IR_V_PAUSE). */
static void preview_pause(void)
{
    ACameraCaptureSession_stopRepeating(g.session);
}
static void preview_resume(void)
{
    ACaptureRequest *reqs[1] = { g.preview_request };
    ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
}

/* Appends the variant flags as JSON. */
static void add_variant(jw_t *w, int variant)
{
    jw_add(w, "\"variant\":{\"preview\":\"%s\",\"fps\":\"%s\",\"control\":\"%s\"}",
           variant & IR_V_PAUSE ? "pause" : "run", variant & IR_V_FPS_LOW ? "low" : "default",
           variant & IR_V_CONTROL_OFF ? "off" : "default");
}

/* Sets the exposure controls of a reusable still request, every time, so a
 * manual setting never sticks to the next photo.
 *   exposure_ns > 0 : manual. AE off, exposure and ISO as given (ISO 0 ->
 *                     lowest), frame duration stretched to fit the exposure.
 *   exposure_ns = 0 : automatic (AE on); the sensor entries are ignored. */
/* Applies a focus mode (and, for IR_FOCUS_MANUAL, a distance in diopters) to a
 * reusable request, atomically: focus is set here and nowhere else, so it never
 * mixes with exposure. IR_FOCUS_KEEP leaves whatever the request already has
 * (the session opens every request on continuous AF, so "keep" means auto).
 * Returns 1 if a single-shot AF scan should be triggered (AUTO/MACRO), else 0. */
/* Translates a REAL focus diopter (what the user means: 1/metres, 0 = infinity)
 * into the diopter to send the HAL, using the calibration table if present.
 * Linear interpolation in diopters (optics is linear there); extrapolation
 * beyond the ends keeps the end segments' slope; the result is clamped to
 * [0, max_diopters] so we never ask the lens past its physical range. With no
 * table, returns the input unchanged (the uncalibrated 1:1 mapping). */
static float focus_translate(float real_d)
{
    int n = g.focus_cal_n, i;
    float out;
    if (n <= 0) return real_d;                    /* no table: 1:1                */
    if (n == 1) {                                  /* one point: the 1:1 mapping   */
        out = real_d + (g.focus_cal_hal[0] - g.focus_cal_real[0]);   /* shifted to it */
        if (out < 0.0f) out = 0.0f;
        if (g.max_diopters > 0.0f && out > g.max_diopters) out = g.max_diopters;
        return out;
    }
    /* Find the segment [i, i+1] whose real_d brackets the request; clamp index
     * to the ends so we extrapolate along the first/last segment. */
    i = 0;
    while (i < n - 2 && real_d > g.focus_cal_real[i + 1]) ++i;
    {
        float r0 = g.focus_cal_real[i], r1 = g.focus_cal_real[i + 1];
        float h0 = g.focus_cal_hal[i], h1 = g.focus_cal_hal[i + 1];
        float t = (r1 > r0) ? (real_d - r0) / (r1 - r0) : 0.0f; /* may be <0 or >1 */
        out = h0 + t * (h1 - h0);                 /* interpolate / extrapolate    */
    }
    if (out < 0.0f) out = 0.0f;                    /* never past infinity          */
    if (g.max_diopters > 0.0f && out > g.max_diopters) out = g.max_diopters; /* nor min dist */
    return out;
}

/* The inverse: the lens value the HAL reports -> the REAL diopters it means
 * (what the user would call the distance), along the same segments as
 * focus_translate (interpolating, extrapolating at the ends); with no table,
 * unchanged. Used to tell the UI the distance the autofocus found, so manual
 * focus can continue from there. Never negative (infinity at most). */
static float focus_untranslate(float hal_d)
{
    int n = g.focus_cal_n, i;
    float out;
    if (n <= 0) return hal_d;
    if (n == 1) out = hal_d - (g.focus_cal_hal[0] - g.focus_cal_real[0]);
    else {
        /* Segments are ordered by real diopters; the HAL values along them are
         * monotonic for a sane lens, so the same walk finds the segment. */
        int rising = g.focus_cal_hal[n - 1] >= g.focus_cal_hal[0];
        i = 0;
        while (i < n - 2 && (rising ? hal_d > g.focus_cal_hal[i + 1] : hal_d < g.focus_cal_hal[i + 1])) ++i;
        {
            float r0 = g.focus_cal_real[i], r1 = g.focus_cal_real[i + 1];
            float h0 = g.focus_cal_hal[i], h1 = g.focus_cal_hal[i + 1];
            float t = (h1 != h0) ? (hal_d - h0) / (h1 - h0) : 0.0f;
            out = r0 + t * (r1 - r0);
        }
    }
    return out < 0.0f ? 0.0f : out;
}

/* Copies the preview's focus (AF mode, and the lens distance when AF is off)
 * onto another request, so a capture or a meter frame asks the HAL for the
 * SAME focus as the preview. The still, raw and meter requests were created
 * at setup with the setup's AF mode (CONTINUOUS_PICTURE where available)
 * and never followed the preview afterwards: a capture that "keeps" the
 * focus (no focus= -- the 'once' mode after a tap, a locked focus) still
 * asked for continuous AF, and the XA2's HAL started a scan at the capture,
 * undoing the lock; its meter frames did the same in RAW (0.47.50/51). */
static void copy_preview_focus(ACaptureRequest *req)
{
    ACameraMetadata_const_entry e;
    if (!req || !g.preview_request || req == g.preview_request) return;
    if (ACaptureRequest_getConstEntry(g.preview_request, ACAMERA_CONTROL_AF_MODE, &e) == ACAMERA_OK && e.count)
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, e.data.u8);
    if (ACaptureRequest_getConstEntry(g.preview_request, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
        ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, e.data.f);
}

static int set_focus(ACaptureRequest *req, int mode, float diopters)
{
    uint8_t af;
    switch (mode) {
    case IR_FOCUS_AUTO:             af = ACAMERA_CONTROL_AF_MODE_AUTO; break;
    case IR_FOCUS_CONTINUOUS:       af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE; break;
    case IR_FOCUS_CONTINUOUS_VIDEO: af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO; break;
    case IR_FOCUS_MACRO:            af = ACAMERA_CONTROL_AF_MODE_MACRO; break;
    case IR_FOCUS_EDOF:             af = ACAMERA_CONTROL_AF_MODE_EDOF; break;
    case IR_FOCUS_MANUAL:
    case IR_FOCUS_RAW:
        af = ACAMERA_CONTROL_AF_MODE_OFF;        /* no AF; set the lens directly  */
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
        if (diopters < 0) diopters = 0;          /* 0 = infinity                  */
        {
            /* MANUAL: the user's real distance, through the focus calibration.
             * RAW: the HAL's own value as given (the assistant walks these). */
            float hal_d = mode == IR_FOCUS_RAW ? diopters : focus_translate(diopters);
            ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &hal_d);
        }
        return 0;
    case IR_FOCUS_HOLD:
        /* A locked focus is a LENS POSITION, not an autofocus state: AF off
         * and the position the HAL reported in its last result (its own units,
         * no calibration involved). Then no later change of the request --
         * exposure, white balance, a scene -- can move the lens. Used after a
         * tap's scan has locked, and to freeze the lens for a stack. */
        af = ACAMERA_CONTROL_AF_MODE_OFF;
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
        {
            float lens;
            pthread_mutex_lock(&g.lock);
            lens = g.live_focus;
            pthread_mutex_unlock(&g.lock);
            if (lens >= 0 && lens == lens)                     /* known, finite        */
                ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &lens);
        }
        return 0;
    default:                                     /* IR_FOCUS_KEEP: as the preview */
        copy_preview_focus(req);                 /* (nothing when req IS the preview) */
        return 0;
    }
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
    /* AUTO and MACRO are single-shot: the caller triggers one scan. */
    return mode == IR_FOCUS_AUTO || mode == IR_FOCUS_MACRO;
}

/* Flash on one capture request. With automatic exposure the AE mode carries
 * it (set_exposure above): the HAL meters with a pre-flash and fires when it
 * judges it dark ("auto") or always ("on"). With a MANUAL pair (the user's,
 * or the app holding a pair from a tapped spot) there is no AE to judge:
 * "on" fires a single flash with the shot (FLASH_MODE_SINGLE), "auto" does
 * NOT fire -- an automatic flash needs the camera's own metering. The request
 * is set back after the shot (flash_disarm), so a repeating preview never
 * flashes. Torch is a preview state (indraw_flash). */
static void flash_arm(ACaptureRequest *req, int manual)
{
    uint8_t fm = (manual && g.flash_mode == 2) ? ACAMERA_FLASH_MODE_SINGLE
               : g.flash_mode == 3 ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
    if (req) ACaptureRequest_setEntry_u8(req, ACAMERA_FLASH_MODE, 1, &fm);
}
static void flash_disarm(ACaptureRequest *req)
{
    uint8_t fm = g.flash_mode == 3 ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
    if (req) ACaptureRequest_setEntry_u8(req, ACAMERA_FLASH_MODE, 1, &fm);
}

static int set_exposure(ACaptureRequest *req, int64_t exposure_ns, int iso,
                        char *out, size_t out_size)
{
    ACameraMetadata_const_entry e;
    int64_t min_exp = 0, max_exp = 0, max_frame = 0, frame;
    int32_t min_iso = 0, max_iso = 0, iso32;
    uint8_t ae;
    if (exposure_ns <= 0) {                              /* back to automatic       */
        int32_t ev = g.ev_steps;                         /* the UI's EV wheel        */
        /* Automatic exposure tells the HAL about the flash: it then pre-flashes
         * during the precapture and fires on the photo (auto: when dark). */
        ae = g.flash_mode == 1 ? ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH
           : g.flash_mode == 2 ? ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH
           : ACAMERA_CONTROL_AE_MODE_ON;
        {   /* the automatic exposure as held by a reference point, or free */
            uint8_t lk = g.ae_lock ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
            ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_LOCK, 1, &lk);
        }
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &ev);
        return IR_OK;
    }
    /* Check the request against what the sensor declares. */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) ==
            ACAMERA_OK && e.count == 2) { min_exp = e.data.i64[0]; max_exp = e.data.i64[1]; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) ==
            ACAMERA_OK && e.count == 2) { min_iso = e.data.i32[0]; max_iso = e.data.i32[1]; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION, &e) ==
            ACAMERA_OK && e.count) max_frame = e.data.i64[0];
    if (!max_exp || !max_iso)
        return fail(out, out_size, IR_EARG, "check", "manual exposure not supported", 0);
    if (exposure_ns < min_exp || exposure_ns > max_exp)
        return fail(out, out_size, IR_EARG, "check", "exposure outside sensor range", 0);
    if (iso <= 0) iso = min_iso;                         /* default: cleanest ISO   */
    if (iso < min_iso || iso > max_iso)
        return fail(out, out_size, IR_EARG, "check", "iso outside sensor range", iso);
    /* A frame cannot be shorter than its exposure; the stream minimum is 1/30 s
     * by default, but some HALs accept shorter/longer -- override it per device
     * with min_frame_ns in override-hal.json. */
    {
        int64_t floor = g.override_min_frame_ns > 0 ? g.override_min_frame_ns : 33333333;
        frame = exposure_ns > floor ? exposure_ns : floor;
    }
    if (max_frame && frame > max_frame) frame = max_frame;
    ae = ACAMERA_CONTROL_AE_MODE_OFF;
    iso32 = iso;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
    ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure_ns);
    ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso32);
    ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_FRAME_DURATION, 1, &frame);
    return IR_OK;
}

/* Sends the trigger request once with the given AF / AE trigger values. */
static int send_trigger(uint8_t af, uint8_t ae)
{
    ACaptureRequest *reqs[1] = { g.trigger_request };
    char dummy[256];
    /* The trigger request goes to the HAL as a frame of its own between the
     * preview's: it must carry the PREVIEW's focus and exposure. It was built
     * once at setup (AF continuous, no lens distance = infinity, AE on) and
     * never followed the preview: with the focus manual or held and the
     * exposure manual, the cancel sent after every photo told the HAL, for
     * one frame, "continuous AF, lens at infinity, auto exposure" -- the XA2's
     * lens left for the distance and came back, and the photo, a few frames
     * ahead in the pipeline, was sometimes taken while it travelled (0.47.56). */
    copy_preview_focus(g.trigger_request);
    set_exposure(g.trigger_request, g.pv_exposure_ns, g.pv_iso, dummy, sizeof dummy);
    ACaptureRequest_setEntry_u8(g.trigger_request, ACAMERA_CONTROL_AF_TRIGGER, 1, &af);
    ACaptureRequest_setEntry_u8(g.trigger_request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &ae);
    if (af == ACAMERA_CONTROL_AF_TRIGGER_START || ae == ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START)
        g.trigger_pending = 1;                      /* af_release has something to cancel */
    return ACameraCaptureSession_capture(g.session, &g_preview_cbs, 1, reqs, NULL);
}

/* Cancels any AF scan / AE precapture we started, so the preview is never left
 * with a locked focus or exposure. Safe to call even if nothing was triggered
 * (an AF_TRIGGER_CANCEL on an idle request is harmless). Called on EVERY exit
 * path of a capture that may have triggered one, not just success. */
static void af_release(int focus_mode)
{
    /* Cancel only what was started: an AF scan or an AE precapture sent with
     * send_trigger (START). Without one there is nothing to release, and an
     * extra frame between the preview's is only a risk (0.47.56). */
    (void)focus_mode;
    if (!g.trigger_pending) return;
    send_trigger(ACAMERA_CONTROL_AF_TRIGGER_CANCEL, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
    g.trigger_pending = 0;
}

/* What run_3a() ended with, for the report. */
typedef struct {
    const char *result;   /* "ready", "timeout", "skipped"   */
    int ae, af;           /* last AE / AF states seen         */
} a3_report_t;

/* AF is done when locked (sharp or not); AE when it stopped searching. */
static int af_done(int s)
{
    return s == ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED ||
           s == ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
}
static int ae_done(int s)
{
    return s == ACAMERA_CONTROL_AE_STATE_CONVERGED ||
           s == ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED ||
           s == ACAMERA_CONTROL_AE_STATE_LOCKED;
}

/* Starts an AF scan (if continuous AF exists) and an AE precapture, then waits
 * until both settle on a frame AFTER the trigger, or `timeout_ms` elapses.
 * A timeout is not an error: the photo is taken anyway and the report says so. */
static int run_3a(int timeout_ms, a3_report_t *rep)
{
    struct timespec dl;
    int st, ok = 0;
    pthread_mutex_lock(&g.lock);
    g.trigger_seen = 0;                         /* wait for OUR trigger's frame  */
    g.live_results = 0;
    pthread_mutex_unlock(&g.lock);
    st = send_trigger(g.af_continuous ? ACAMERA_CONTROL_AF_TRIGGER_START
                                      : ACAMERA_CONTROL_AF_TRIGGER_IDLE,
                      ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START);
    if (st != ACAMERA_OK) return st;
    dl = deadline_in(timeout_ms);
    pthread_mutex_lock(&g.lock);
    for (;;) {
        /* live_results > 1: ignore the trigger frame itself (stale states). */
        ok = g.live_results > 1 && ae_done(g.live_ae) &&
             (!g.af_continuous || af_done(g.live_af));
        if (ok || g.device_error) break;
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    }
    rep->result = ok ? "ready" : "timeout";
    rep->ae = g.live_ae;
    rep->af = g.live_af;
    pthread_mutex_unlock(&g.lock);
    return ACAMERA_OK;
}

/* Applies a focus mode to the running PREVIEW request and restarts it, so the
 * lens physically moves and the viewfinder reflects it -- the capture requests
 * alone are too brief to move the lens (this is why a one-shot manual distance
 * on the still request did nothing). For a manual mode it then waits (up to
 * ~1.2 s) for the lens to stop moving near the requested distance. Mirrors how
 * RAWfish drives focus on the repeating request. Returns 1 if an AF scan should
 * be triggered (AUTO/MACRO), else 0. */
static int set_preview_focus_opt(int mode, float diopters, int wait);
static int set_preview_focus(int mode, float diopters) { return set_preview_focus_opt(mode, diopters, 1); }
/* wait = 0: a manual distance is set and the call returns at once (the
 * controller's volume-key stepping sends one every few tens of ms; waiting
 * for the lens on each made the steps pile up and run on after the key
 * was released). The lens still moves; the next result tells where it is. */
static int set_preview_focus_opt(int mode, float diopters, int wait)
{
    ACaptureRequest *reqs[1] = { g.preview_request };
    int want_af_scan = set_focus(g.preview_request, mode, diopters); /* on the PREVIEW */
    struct timespec dl;
    if (mode == IR_FOCUS_KEEP) return 0;           /* leave the preview as it is   */
    /* The RAW meter's request (a frame every 200 ms when the meter is on)
     * must carry the SAME focus as the preview, as it does for the exposure:
     * it kept the setup's CONTINUOUS_PICTURE, so with the focus manual or
     * 'once' every meter frame told the HAL "continuous AF" between preview
     * frames saying "AF off" -- the XA2's HAL restarted a scan each time and
     * the lens hunted without end in RAW (0.47.50). A HOLD becomes the
     * preview's current lens value, as set_focus did for the preview. */
    copy_preview_focus(g.meter_request);
    /* Restart the repeating request so the new focus takes effect now. */
    ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if ((mode != IR_FOCUS_MANUAL && mode != IR_FOCUS_RAW) || !wait) return want_af_scan; /* AF modes settle via 3A; HOLD is already there */
    /* Manual distance: wait for the lens to reach the (translated) target and
     * stop moving. The HAL reports the value we sent it, so compare against the
     * calibrated value, not the user's requested diopters. */
    {
        float target = mode == IR_FOCUS_RAW ? (diopters < 0 ? 0 : diopters) : focus_translate(diopters < 0 ? 0 : diopters);
        dl = deadline_in(1200);
        pthread_mutex_lock(&g.lock);
        for (;;) {
            int settled = g.live_lens_state == ACAMERA_LENS_STATE_STATIONARY &&
                          fabsf(g.live_focus - target) < 0.5f;
            if (settled || g.device_error) break;
            if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&g.lock);
    }
    return 0;
}

/* Prepares focus and (optionally) exposure before a still/raw capture, keeping
 * them ATOMIC and independent:
 *   - focus_mode/diopters: applied to `req` via set_focus (one responsibility)
 *   - if that mode is single-shot (AUTO/MACRO), an AF scan is triggered
 *   - ae_precapture: the photo is in automatic exposure. With the flash to
 *     fire, an AE precapture (its pre-flash metering) and up to af_wait_ms
 *     (1.5 s); without flash no precapture, only a wait of at most 0.5 s
 *     for the continuous AE to settle (0.47.29); independent of focus
 * Fills `rep` with the 3A outcome. A timeout is not fatal. This replaces the old
 * all-in-one "focus" flag; the combined easy path calls run_3a directly. */
static void prepare_capture(ACaptureRequest *req, int focus_mode, float focus_diopters,
                            int ae_precapture, a3_report_t *rep)
{
    int want_af_scan;
    struct timespec dl;
    int ok = 0;
    /* Move the lens via the preview first, then copy the same focus to the
     * capture request so the shot is taken at that focus -- including when
     * the capture KEEPS the preview's focus (no focus= from the interface:
     * set_focus then copies the preview's AF mode and lens distance; up to
     * 0.47.50 it touched nothing and the capture kept its setup AF mode). */
    want_af_scan = set_preview_focus(focus_mode, focus_diopters);
    set_focus(req, focus_mode, focus_diopters);
    if (g.capture_delay_ms > 0) {                  /* device profile: let the lens settle */
        struct timespec ts;
        ts.tv_sec = g.capture_delay_ms / 1000;
        ts.tv_nsec = (long)(g.capture_delay_ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
    rep->result = "skipped";
    rep->ae = rep->af = -1;
    /* The automatic exposure (ae_precapture: the photo is in automatic
     * exposure). WITH the flash to fire, the HAL needs a precapture (its
     * pre-flash metering), converged or not, and up to 1.5 s for it.
     * WITHOUT flash, no precapture at all (0.47.29): it is a sequence for
     * the flash, it outlasts 0.5 s on the JP2, and its state lingered from
     * one shot to the next, so every shot triggered another and waited
     * 0.5 s. The AE runs continuously anyway: the photo only waits, at most
     * 0.5 s, for it to settle if it is still searching; a precapture left
     * over from earlier is cancelled first. */
    {
        int flash, settle, cancel, full;
        pthread_mutex_lock(&g.lock);
        flash = ae_precapture && (g.flash_mode == 1 || g.flash_mode == 2);
        settle = ae_precapture && !flash && !ae_done(g.live_ae);
        cancel = ae_precapture && !flash && g.live_ae == ACAMERA_CONTROL_AE_STATE_PRECAPTURE;
        pthread_mutex_unlock(&g.lock);
        if (!want_af_scan && !flash && !settle) return;   /* nothing to wait for */
        pthread_mutex_lock(&g.lock);
        g.trigger_seen = 0;
        g.live_results = 0;
        pthread_mutex_unlock(&g.lock);
        /* Trigger only what is needed: an AF scan, the flash's precapture, or
         * the cancel of a lingering one. */
        if (want_af_scan || flash || cancel)
            send_trigger(want_af_scan ? ACAMERA_CONTROL_AF_TRIGGER_START : ACAMERA_CONTROL_AF_TRIGGER_IDLE,
                         flash ? ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START
                               : cancel ? ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL
                                        : ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        /* How long: 1.5 s (af_wait_ms; shorter and the flash has no time to
         * meter: over-exposed) for the flash or an AF scan asked for (the
         * CLI's focus=auto); otherwise at most 0.5 s for the exposure to
         * settle (0.1-0.3 s for ordinary changes). */
        full = g.af_wait_ms > 0 ? g.af_wait_ms : 1500;
        dl = deadline_in((flash || want_af_scan) ? full : (full < 500 ? full : 500));
        pthread_mutex_lock(&g.lock);
        for (;;) {
            ok = g.live_results > 1 &&
                 (!(flash || settle) || ae_done(g.live_ae)) &&
                 (!want_af_scan || af_done(g.live_af));
            if (ok || g.device_error) break;
            if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
        }
    }
    rep->result = ok ? "ready" : "timeout";
    rep->ae = g.live_ae;
    rep->af = g.live_af;
    pthread_mutex_unlock(&g.lock);
}


/* Writes `len` bytes to `path` through "<path>.part" + rename, so a failure
 * never leaves a truncated photo behind. Returns 0 or an errno value. */
static int write_file(const char *path, const uint8_t *data, size_t len)
{
    char tmp[1024];
    size_t done = 0;
    int fd, err = 0;
    if (snprintf(tmp, sizeof tmp, "%s.part", path) >= (int)sizeof tmp) return ENAMETOOLONG;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);         /* fresh temp file */
    if (fd < 0) return errno;
    while (done < len) {                                        /* write may be short */
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0) { if (errno == EINTR) continue; err = errno; break; }
        done += (size_t)n;
    }
    if (close(fd) != 0 && !err) err = errno;                    /* flush errors     */
    if (!err && rename(tmp, path) != 0) err = errno;            /* atomic replace   */
    if (err) unlink(tmp);                                       /* no leftovers     */
    return err;
}

/* Takes one JPEG on the open session and saves it to `path`.
 *   quality  1..100 (ACAMERA_JPEG_QUALITY)
 *   nr       noise-reduction mode value, or -1 to keep the template default
 *   edge     edge (sharpening) mode value, or -1 to keep the template default
 *   focus_mode     IR_FOCUS_* (KEEP = leave the session's auto focus)
 *   focus_diopters for IR_FOCUS_MANUAL: 0 = infinity, else 1/metres
 *   ae_precapture  1 = run an auto-exposure precapture first (auto exposure only)
 *   exposure_ns, iso   manual exposure (see set_exposure); 0 = automatic
 *   variant  RF_V_* flags (0 = normal capture)
 *   timeout  max wait for the image, in ms (add the exposure for long ones) */
static int indraw_jpeg_impl(const char *path, int quality, int nr, int edge,
                   int focus_mode, float focus_diopters, int ae_precapture,
                   int64_t exposure_ns, int iso, int variant, int timeout_ms,
                   char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    AImage *img = NULL;
    uint8_t *data = NULL;
    int data_len = 0, err, failed, fail_reason, dev_err, exif_dropped = 0;
    int64_t img_ts = -1;
    size_t jpeg_len;
    double t0, t_3a, t_image, t_written;
    long frames_before, frames_during;
    a3_report_t a3 = { "skipped", -1, -1 };
    struct timespec dl;
    camera_status_t st;
    uint8_t u8;
    jw_t w;

    if (!path || !out || !out_size || quality < 1 || quality > 100 || timeout_ms <= 0)
        return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "jpeg", "camera not open", 0);
    if (nr >= 0 && !u8_list_has(g.chars, ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nr))
        return fail(out, out_size, IR_EARG, "check", "noise reduction mode not listed", nr);
    if (edge >= 0 && !u8_list_has(g.chars, ACAMERA_EDGE_AVAILABLE_EDGE_MODES, edge))
        return fail(out, out_size, IR_EARG, "check", "edge mode not listed", edge);

    /* Per-photo settings on the reusable still request. */
    if (set_exposure(g.still_request, exposure_ns, iso, out, out_size)) return IR_EARG;
    set_variant(g.still_request, variant);
    u8 = (uint8_t)quality;
    ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_JPEG_QUALITY, 1, &u8);
    {   /* Tag the JPEG upright: ACAMERA_JPEG_ORIENTATION is in degrees and the
         * HAL bakes the matching EXIF orientation, so the gallery shows it the
         * right way up (not rotated 90 like before). Use the sensor mounting
         * angle read at open. */
        int32_t jo = photo_orientation_deg();      /* upright on the ground */
        ACaptureRequest_setEntry_i32(g.still_request, ACAMERA_JPEG_ORIENTATION, 1, &jo);
    }
    /* -1 ("default") restores the template value instead of keeping the last one. */
    {   /* Per-shot nr/edge override the session setting, which overrides the
         * template. So "jpeg off_noise" wins for that shot; otherwise the
         * session value set via "preview noise=..." applies; else the template. */
        int use_nr = nr >= 0 ? nr : g.tpl_nr;       /* the shot's own, or the template's */
        int use_edge = edge >= 0 ? edge : g.tpl_edge;
        if (use_nr >= 0) { u8 = (uint8_t)use_nr; ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_NOISE_REDUCTION_MODE, 1, &u8); }
        if (use_edge >= 0) { u8 = (uint8_t)use_edge; ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_EDGE_MODE, 1, &u8); }
    }

    /* Forget anything left from a previous photo. */
    pthread_mutex_lock(&g.lock);
    if (g.jpeg_image) { AImage_delete(g.jpeg_image); g.jpeg_image = NULL; }
    g.capture_failed = 0;
    g.result_ready = 0;
    g.captured_owed = "captured kind=jpeg\n";   /* sent at its result or its image */
    pthread_mutex_unlock(&g.lock);

    t0 = now_ms();
    /* Atomic focus + optional AE precapture, each independent. */
    prepare_capture(g.still_request, focus_mode, focus_diopters, ae_precapture, &a3);
    t_3a = now_ms();
    if (variant & IR_V_PAUSE) preview_pause();
    pthread_mutex_lock(&g.lock);
    frames_before = g.preview_frames;           /* to count frames during capture */
    pthread_mutex_unlock(&g.lock);
    reqs[0] = g.still_request;
    flash_arm(g.still_request, exposure_ns > 0);
    st = ACameraCaptureSession_capture(g.session, &g_still_cbs, 1, reqs, NULL); /* one shot */
    flash_disarm(g.still_request);              /* the request is copied at submit */
    /* The still capture has latched the focus/exposure it needs, so release any
     * AF scan / AE precapture now: this runs on EVERY path below (success and
     * all errors), so the preview is never left locked. */
    af_release(focus_mode);
    if (st != ACAMERA_OK) {
        if (variant & IR_V_PAUSE) preview_resume();
        return fail(out, out_size, IR_ECAPTURE, "capture", "capture refused", st);
    }

    /* Wait for the image (or a failure). */
    dl = deadline_in(timeout_ms);
    pthread_mutex_lock(&g.lock);
    while (!g.jpeg_image && !g.capture_failed && !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    frames_during = g.preview_frames - frames_before;
    img = g.jpeg_image;                         /* take ownership                 */
    g.jpeg_image = NULL;
    failed = g.capture_failed;
    fail_reason = g.capture_fail_reason;
    dev_err = g.device_error;
    pthread_mutex_unlock(&g.lock);
    t_image = now_ms();
    if (variant & IR_V_PAUSE) preview_resume();  /* viewfinder back on            */

    if (!img) {
        if (failed) return fail(out, out_size, IR_ECAPTURE, "capture_failed", "camera reported failure", fail_reason);
        if (dev_err) return fail(out, out_size, IR_ECAPTURE, "device", "device error", dev_err);
        return fail(out, out_size, IR_ECAPTURE, "wait_image", "timeout", timeout_ms);
    }

    /* The JPEG buffer can be longer than the image (zero padding):
     * keep everything up to the last End-Of-Image marker (FF D9). */
    AImage_getTimestamp(img, &img_ts);
    if (AImage_getPlaneData(img, 0, &data, &data_len) != AMEDIA_OK || data_len < 4 ||
        data[0] != 0xFF || data[1] != 0xD8) {
        AImage_delete(img);
        return fail(out, out_size, IR_ECAPTURE, "jpeg_data", "not a JPEG buffer", data_len);
    }
    /* Trim the zero padding the reader leaves after the JPEG by walking back
     * from the END to the last End-Of-Image marker (FF D9). Scanning from the
     * end (not forward) avoids stopping at an FF D9 that occurs inside the
     * entropy-coded data. The only residual risk is a stray FF D9 in trailing
     * padding, which is vanishingly unlikely; if no marker is found we keep the
     * whole buffer. */
    jpeg_len = (size_t)data_len;
    while (jpeg_len >= 2 && !(data[jpeg_len - 2] == 0xFF && data[jpeg_len - 1] == 0xD9))
        --jpeg_len;                             /* walk back to FF D9             */
    if (jpeg_len < 2) jpeg_len = (size_t)data_len; /* no marker: keep it all      */

    captured_now();                               /* if the result has not already */

    /* The result usually arrives before the image; allow 500 ms more if not.
     * It is needed BEFORE writing: the Exif we add carries the applied
     * exposure and ISO from it (the HAL's own Exif lacks them). */
    dl = deadline_in(500);
    pthread_mutex_lock(&g.lock);
    while (!g.result_ready)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    {
        exif_info_t ex;
        ACameraMetadata_const_entry fe;
        char dt[24];
        /* Make / Model: the device's own, from the Android properties the HAL
         * runs with (ro.product.manufacturer / ro.product.model), so the tags
         * say what the hardware is; a missing property leaves the tag out. */
        static char make[PROP_VALUE_MAX], model[PROP_VALUE_MAX];
        static int props_read = 0;
        if (!props_read) {
            if (__system_property_get("ro.product.manufacturer", make) <= 0) make[0] = '\0';
            if (__system_property_get("ro.product.model", model) <= 0 &&
                __system_property_get("ro.product.device", model) <= 0) model[0] = '\0';
            props_read = 1;
        }
        memset(&ex, 0, sizeof ex);
        exif_datetime(dt, sizeof dt);
        ex.make = make[0] ? make : NULL;
        ex.model = model[0] ? model : NULL;
        ex.software = "RAWfish-ir " IR_VERSION;
        ex.datetime = dt;
        ex.exposure_ns = g.result_ready ? g.r_exposure_ns : -1;    /* only what the HAL reported */
        ex.iso = g.result_ready ? g.r_iso : -1;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &fe) == ACAMERA_OK && fe.count)
            ex.focal_mm = fe.data.f[0];
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_AVAILABLE_APERTURES, &fe) == ACAMERA_OK && fe.count)
            ex.f_number = fe.data.f[0];
        if (ex.focal_mm > 0 && ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &fe) == ACAMERA_OK &&
            fe.count >= 2 && fe.data.f[0] > 0)
            ex.focal_eq_mm = (int)(ex.focal_mm * 36.0f / fe.data.f[0] + 0.5f);
        ex.orientation = 0;                                         /* keep the HAL's tag  */
        ex.wb_manual = g.wb_fixed || g.wb_shift || g.wb_tint;       /* the white the file got */
        ex.exposure_manual = exposure_ns > 0;                       /* a manual pair was asked */
        ex.hal_dropped = &exif_dropped;                             /* reported if not 0 */
        pthread_mutex_unlock(&g.lock);
        err = exif_write_jpeg(path, data, jpeg_len, &ex);
        if (err) err = write_file(path, data, jpeg_len);           /* never lose the shot */
        pthread_mutex_lock(&g.lock);
    }
    AImage_delete(img);                         /* buffer back to the reader      */
    t_written = now_ms();
    if (err) { pthread_mutex_unlock(&g.lock); return fail(out, out_size, IR_EFILE, "write", strerror(err), err); }
    pthread_mutex_unlock(&g.lock);
    emit_written("jpeg", path);
    pthread_mutex_lock(&g.lock);

    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"path\":");
    jw_str(&w, path);                            /* JSON-escaped path            */
    jw_add(&w, ",\"size\":[%d,%d],\"bytes\":%zu,\"quality\":%d,"
               "\"noise\":\"%s\",\"edge\":\"%s\","
               "\"3a\":{\"result\":\"%s\",\"ae\":\"%s\",\"af\":\"%s\"},"
               "\"ms\":{\"3a\":%.1f,\"image\":%.1f,\"written\":%.1f},\"preview_frames\":%ld,\"applied\":",
           g.jpeg_w, g.jpeg_h, jpeg_len, quality, nr >= 0 ? nr_name(nr) : "default_noise",
           edge >= 0 ? edge_name(edge) : "default_edge", a3.result, ae_state_name(a3.ae), af_state_name(a3.af),
           t_3a - t0, t_image - t0, t_written - t0, g.preview_frames);
    if (g.result_ready)
        jw_add(&w, "{\"exposure_ns\":%lld,\"frame_ns\":%lld,\"iso\":%d,\"fps_range\":[%d,%d],"
                   "\"control\":%d,\"ae\":\"%s\",\"af\":\"%s\","
                   "\"focus_diopters\":%.3f,\"noise\":\"%s\",\"edge\":\"%s\",\"matches_image\":%s}",
               (long long)g.r_exposure_ns, (long long)g.r_frame_ns, g.r_iso, g.r_fps[0], g.r_fps[1],
               g.r_control, ae_state_name(g.r_ae_state),
               af_state_name(g.r_af_state), g.r_focus_diopters,
               g.r_nr >= 0 ? nr_name(g.r_nr) : "none", g.r_edge >= 0 ? edge_name(g.r_edge) : "none",
               g.r_timestamp == img_ts ? "true" : "false");
    else
        jw_add(&w, "null");                     /* never invent values            */
    jw_add(&w, ",\"requested\":{\"exposure_ns\":%lld,\"iso\":%d},\"preview_frames_during\":%ld,",
           (long long)exposure_ns, iso, frames_during); /* 0 = automatic            */
    if (exif_dropped > 0) jw_add(&w, "\"exif_hal_dropped\":%d,", exif_dropped);   /* never silent */
    add_variant(&w, variant);
    jw_add(&w, "}");
    pthread_mutex_unlock(&g.lock);
    return IR_OK;
}

/* ------------------------------------------------------------------------- */
/* RAW16                                                                     */
/* ------------------------------------------------------------------------- */

/* Tag number + its name without the "ACAMERA_" prefix (8 characters), for the
 * sidecar. &(#t)[8] points 8 characters into the string; written this way
 * because clang warns on the equivalent "#t + 8" (-Wstring-plus-int). */
#define TAG(t) { t, &(#t)[8] }
typedef struct { uint32_t tag; const char *name; } tag_name_t;

/* Static facts needed to develop the RAW or to build a DNG. */
static const tag_name_t k_raw_static[] = {
    TAG(ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT),  /* Bayer order (3 = BGGR)       */
    TAG(ACAMERA_SENSOR_INFO_WHITE_LEVEL),               /* saturation value             */
    TAG(ACAMERA_SENSOR_BLACK_LEVEL_PATTERN),            /* black per 2x2 position       */
    TAG(ACAMERA_SENSOR_OPTICAL_BLACK_REGIONS),
    TAG(ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE),
    TAG(ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE),
    TAG(ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE),
    TAG(ACAMERA_SENSOR_INFO_PHYSICAL_SIZE),             /* mm                           */
    TAG(ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED),      /* 1 = RAW already de-vignetted */
    TAG(ACAMERA_SENSOR_ORIENTATION),
    TAG(ACAMERA_SENSOR_REFERENCE_ILLUMINANT1),          /* colour calibration ...       */
    TAG(ACAMERA_SENSOR_REFERENCE_ILLUMINANT2),
    TAG(ACAMERA_SENSOR_CALIBRATION_TRANSFORM1),
    TAG(ACAMERA_SENSOR_CALIBRATION_TRANSFORM2),
    TAG(ACAMERA_SENSOR_COLOR_TRANSFORM1),
    TAG(ACAMERA_SENSOR_COLOR_TRANSFORM2),
    TAG(ACAMERA_SENSOR_FORWARD_MATRIX1),
    TAG(ACAMERA_SENSOR_FORWARD_MATRIX2),                /* ... for the DNG              */
    TAG(ACAMERA_LENS_INFO_AVAILABLE_APERTURES),
    TAG(ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS),
    TAG(ACAMERA_LENS_INFO_SHADING_MAP_SIZE),            /* columns, rows of the gain map */
    TAG(ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE),
    TAG(ACAMERA_LENS_INFO_HYPERFOCAL_DISTANCE),
    TAG(ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION),
    TAG(ACAMERA_LENS_DISTORTION),
    TAG(ACAMERA_LENS_INTRINSIC_CALIBRATION),
};

/* Per-frame facts, from the capture result of this very RAW. */
static const tag_name_t k_raw_result[] = {
    TAG(ACAMERA_SENSOR_TIMESTAMP),
    TAG(ACAMERA_SENSOR_EXPOSURE_TIME),
    TAG(ACAMERA_SENSOR_FRAME_DURATION),
    TAG(ACAMERA_SENSOR_SENSITIVITY),
    TAG(ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST),
    TAG(ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL),            /* if present, beats the static one */
    TAG(ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL),
    TAG(ACAMERA_SENSOR_NEUTRAL_COLOR_POINT),            /* as-shot white balance        */
    TAG(ACAMERA_COLOR_CORRECTION_GAINS),
    TAG(ACAMERA_COLOR_CORRECTION_TRANSFORM),
    TAG(ACAMERA_SENSOR_NOISE_PROFILE),                  /* noise model, for denoising   */
    TAG(ACAMERA_SENSOR_GREEN_SPLIT),
    TAG(ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW),
    TAG(ACAMERA_LENS_APERTURE),
    TAG(ACAMERA_LENS_FOCAL_LENGTH),
    TAG(ACAMERA_LENS_FOCUS_DISTANCE),
    TAG(ACAMERA_LENS_FOCUS_RANGE),
    TAG(ACAMERA_LENS_STATE),
    TAG(ACAMERA_LENS_OPTICAL_STABILIZATION_MODE),
    TAG(ACAMERA_CONTROL_MODE),
    TAG(ACAMERA_CONTROL_AE_MODE),
    TAG(ACAMERA_CONTROL_AE_TARGET_FPS_RANGE),
    TAG(ACAMERA_CONTROL_AE_STATE),
    TAG(ACAMERA_CONTROL_AF_STATE),
    TAG(ACAMERA_CONTROL_AWB_MODE),
    TAG(ACAMERA_CONTROL_AWB_STATE),
    TAG(ACAMERA_SHADING_MODE),
    TAG(ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE),
    TAG(ACAMERA_STATISTICS_LENS_SHADING_MAP),           /* vignetting gains R,Ge,Go,B   */
};

/* Writes the entries of `meta` listed in `tags` as a JSON object body.
 * Absent tags are skipped; single values are written bare, lists as arrays,
 * rationals as [numerator, denominator] to keep them exact. */
static void dump_tags(FILE *f, const ACameraMetadata *meta, const tag_name_t *tags, size_t n)
{
    size_t t;
    uint32_t i;
    int first = 1;
    for (t = 0; t < n; ++t) {
        ACameraMetadata_const_entry e;
        if (!meta || ACameraMetadata_getConstEntry(meta, tags[t].tag, &e) != ACAMERA_OK) continue;
        fprintf(f, "%s\n    \"%s\": %s", first ? "" : ",", tags[t].name, e.count == 1 ? "" : "[");
        first = 0;
        for (i = 0; i < e.count; ++i) {
            if (i) fputc(',', f);
            switch (e.type) {
            case ACAMERA_TYPE_BYTE:   fprintf(f, "%u", e.data.u8[i]); break;
            case ACAMERA_TYPE_INT32:  fprintf(f, "%d", e.data.i32[i]); break;
            case ACAMERA_TYPE_INT64:  fprintf(f, "%lld", (long long)e.data.i64[i]); break;
            case ACAMERA_TYPE_FLOAT:  fprintf(f, "%.9g", e.data.f[i]); break;
            case ACAMERA_TYPE_DOUBLE: fprintf(f, "%.12g", e.data.d[i]); break;
            case ACAMERA_TYPE_RATIONAL:
                fprintf(f, "[%d,%d]", e.data.r[i].numerator, e.data.r[i].denominator);
                break;
            default:                  fputs("null", f); break;
            }
        }
        if (e.count != 1) fputc(']', f);
    }
}

/* Writes `rows` rows of `row_bytes` bytes, taken every `stride` bytes, to
 * "<path>.part" then renames it to `path`. Returns 0 or an errno value. */
static int write_rows(const char *path, const uint8_t *data, size_t row_bytes,
                      size_t stride, size_t rows)
{
    char tmp[1024];
    FILE *f;
    size_t r;
    int err = 0;
    if (snprintf(tmp, sizeof tmp, "%s.part", path) >= (int)sizeof tmp) return ENAMETOOLONG;
    f = fopen(tmp, "wb");
    if (!f) return errno;
    for (r = 0; r < rows && !err; ++r)                    /* drop the row padding */
        if (fwrite(data + r * stride, 1, row_bytes, f) != row_bytes) err = errno ? errno : EIO;
    if (fclose(f) != 0 && !err) err = errno;
    if (!err && rename(tmp, path) != 0) err = errno;
    if (err) unlink(tmp);
    return err;
}

/* True if `path` ends in ".dng" (case-insensitive). */
static int path_is_dng(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n >= 4 && (path[n - 4] == '.') &&
           (path[n - 3] == 'd' || path[n - 3] == 'D') &&
           (path[n - 2] == 'n' || path[n - 2] == 'N') &&
           (path[n - 1] == 'g' || path[n - 1] == 'G');
}

/* Fills a dng matrix from a 3x3 rational metadata entry (9 pairs). */
static void dng_mat_from(dng_matrix_t *m, const ACameraMetadata *meta, uint32_t tag)
{
    ACameraMetadata_const_entry e;
    uint32_t i;
    m->have = 0;
    if (!meta || ACameraMetadata_getConstEntry(meta, tag, &e) != ACAMERA_OK ||
        e.type != ACAMERA_TYPE_RATIONAL || e.count != 9)
        return;
    for (i = 0; i < 9; ++i) { m->num[i] = e.data.r[i].numerator; m->den[i] = e.data.r[i].denominator; }
    m->have = 1;
}

/* Fills dng_params_t from the static characteristics and the capture result.
 * `shading_gains`, if non-NULL, must hold cols*rows*4 floats already copied out
 * of the (transient) result; dng_write copies them again, so it may be freed
 * after the call. */
static void dng_params_from(dng_params_t *p, const ACameraMetadata *res,
                            const uint16_t *pixels, int w, int h,
                            float *shading_gains, int sh_cols, int sh_rows,
                            int64_t exposure_ns, int iso, const char *software)
{
    ACameraMetadata_const_entry e;
    memset(p, 0, sizeof *p);
    g.shade_last_measured = 0;
    p->pixels = pixels;
    p->width = w;
    p->height = h;
    p->software = software;
    p->make = g.make;                             /* dynamic, from Android props  */
    p->model = g.model;
    p->black = 64;
    p->white = 1023;
    p->cfa = 3;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count == 4) {
        int bi;
        p->have_black4 = 1;                        /* keep the 2x2 pattern         */
        for (bi = 0; bi < 4; ++bi) p->black4[bi] = e.data.i32[bi];
        p->black = (e.data.i32[0] + e.data.i32[1] + e.data.i32[2] + e.data.i32[3]) / 4; /* mean fallback */
    }
    /* Orientation: the sensor's mounting plus the phone's rotation at the
     * shot (photo_orientation_tiff), so the developer shows the image upright. */
    p->orientation = photo_orientation_tiff();
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count)
        p->white = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count)
        p->cfa = e.data.u8[0];
    p->illuminant1 = 21;                          /* D65 default                 */
    p->illuminant2 = 17;                          /* Standard A default          */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK && e.count)
        p->illuminant1 = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, &e) == ACAMERA_OK && e.count)
        p->illuminant2 = e.data.u8[0];
    dng_mat_from(&p->color1, g.chars, ACAMERA_SENSOR_COLOR_TRANSFORM1);
    dng_mat_from(&p->color2, g.chars, ACAMERA_SENSOR_COLOR_TRANSFORM2);
    dng_mat_from(&p->forward1, g.chars, ACAMERA_SENSOR_FORWARD_MATRIX1);
    dng_mat_from(&p->forward2, g.chars, ACAMERA_SENSOR_FORWARD_MATRIX2);
    dng_mat_from(&p->calibration1, g.chars, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1);
    dng_mat_from(&p->calibration2, g.chars, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2);
    /* AsShotNeutral: the white the photo was really balanced on. With a
     * manual white (measured on an object, or shifted) the HAL's
     * NEUTRAL_COLOR_POINT is still ITS estimate, not what was applied; the
     * applied gains are in the result (AWB off), and the neutral is their
     * inverse -- the file says what was applied (0.47.21). */
    p->wb_manual = g.wb_fixed || g.wb_shift || g.wb_tint;
    {
        /* With the AWB off in this shot's result, its gains are the ones we
         * set (a manual white): the neutral is their inverse.
         * Otherwise the HAL's own NEUTRAL_COLOR_POINT. */
        ACameraMetadata_const_entry am;
        int awb_off = ACameraMetadata_getConstEntry(res, ACAMERA_CONTROL_AWB_MODE, &am) == ACAMERA_OK && am.count &&
                      am.data.u8[0] == ACAMERA_CONTROL_AWB_MODE_OFF;
        int from_gains = awb_off && ACameraMetadata_getConstEntry(res, ACAMERA_COLOR_CORRECTION_GAINS, &e) == ACAMERA_OK &&
                         e.count == 4 && e.data.f[0] > 0 && e.data.f[1] + e.data.f[2] > 0 && e.data.f[3] > 0;
        if (from_gains) {
            double gr = e.data.f[0], gg = (e.data.f[1] + e.data.f[2]) / 2, gb = e.data.f[3];
            p->have_neutral = 1;
            p->neutral_num[0] = (int32_t)lround(gg / gr * 1000000); p->neutral_den[0] = 1000000;
            p->neutral_num[1] = 1000000;                              p->neutral_den[1] = 1000000;
            p->neutral_num[2] = (int32_t)lround(gg / gb * 1000000); p->neutral_den[2] = 1000000;
        } else if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT, &e) == ACAMERA_OK &&
                   e.type == ACAMERA_TYPE_RATIONAL && e.count == 3) {
            int i;
            p->have_neutral = 1;
            for (i = 0; i < 3; ++i) { p->neutral_num[i] = e.data.r[i].numerator; p->neutral_den[i] = e.data.r[i].denominator; }
        }
    }
    if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_NOISE_PROFILE, &e) == ACAMERA_OK &&
        e.type == ACAMERA_TYPE_DOUBLE && e.count >= 2) {
        p->have_noise = 1;
        p->noise[0] = e.data.d[0];
        p->noise[1] = e.data.d[1];
    }
    p->exposure_ns = exposure_ns;
    p->iso = iso;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_FOCAL_LENGTH, &e) == ACAMERA_OK && e.count)
        p->focal_mm = e.data.f[0];
    if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_APERTURE, &e) == ACAMERA_OK && e.count)
        p->aperture = e.data.f[0];
    /* The gain map: none if asked (shade_mode 2); the measured calibration
     * at THIS photo's lens position and light when one is loaded; else the
     * HAL's map. Never when the HAL says the RAW is already lens-shading
     * corrected (a double correction). */
    {
        int already = 0;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED, &e) ==
                ACAMERA_OK && e.count)
            already = e.data.u8[0];
        if (already || g.shade_mode == 2) return;
        if (g.shade_mode == 1 && g.shade_have && g.shade_buf) {
            double hal_d = 0, n[3] = { 1, 1, 1 };
            int cct = 0;
            double duv;
            /* the lens position: the HAL's own value (no focus calibration) */
            if (ACameraMetadata_getConstEntry(res, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
                hal_d = e.data.f[0];
            /* the light: the HAL's neutral estimate, as a colour temperature
             * (colortemp.h); if unknown, 5500 K (daylight) */
            if (ACameraMetadata_getConstEntry(res, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT, &e) == ACAMERA_OK &&
                e.type == ACAMERA_TYPE_RATIONAL && e.count == 3) {
                int i;
                for (i = 0; i < 3; ++i) n[i] = e.data.r[i].denominator ? (double)e.data.r[i].numerator / e.data.r[i].denominator : 1;
                if (g.have_ct) ct_estimate(g.ct_m1, g.ct_t1, g.ct_t2 > 0 ? g.ct_m2 : NULL, g.ct_t2, n, &cct, &duv);
            }
            shading_compose(&g.shade, hal_d, cct > 0 ? cct : 5500, g.shade_balance, g.shade_strength,
                             g.shade_max_gain, g.shade_buf);
            p->shading.have = 1;
            p->shading.cols = g.shade.cols;
            p->shading.rows = g.shade.rows;
            p->shading.cfa = p->cfa;
            p->shading.gains = g.shade_buf;
            g.shade_last_d = hal_d; g.shade_last_k = cct > 0 ? cct : 5500;  /* for the preview, undosed */
            g.shade_last_measured = 1;
            return;
        }
    }
    if (shading_gains && sh_cols > 0 && sh_rows > 0) {      /* the HAL's map */
        p->shading.have = 1;
        p->shading.cols = sh_cols;
        p->shading.rows = sh_rows;
        p->shading.cfa = p->cfa;                 /* map gain phases per arrangement */
        p->shading.gains = shading_gains;
    }
}

/* Copies the lens shading map out of a (transient) result into a new float
 * buffer, returning it and its grid size, or NULL if the result has none. */
static float *copy_shading(const ACameraMetadata *res, int *cols, int *rows)
{
    ACameraMetadata_const_entry e, sz;
    float *g2;
    uint32_t i;
    *cols = *rows = 0;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_SHADING_MAP_SIZE, &sz) != ACAMERA_OK ||
        sz.type != ACAMERA_TYPE_INT32 || sz.count != 2)
        return NULL;
    if (ACameraMetadata_getConstEntry(res, ACAMERA_STATISTICS_LENS_SHADING_MAP, &e) != ACAMERA_OK ||
        e.type != ACAMERA_TYPE_FLOAT)
        return NULL;
    *cols = sz.data.i32[0];
    *rows = sz.data.i32[1];
    if ((uint32_t)(*cols * *rows * 4) != e.count) { *cols = *rows = 0; return NULL; }
    g2 = malloc(e.count * sizeof *g2);
    if (!g2) { *cols = *rows = 0; return NULL; }
    for (i = 0; i < e.count; ++i) g2[i] = e.data.f[i];
    return g2;
}

/* Takes one RAW16 frame on the open session and saves:
 *   path         the pixels: uint16 little-endian, width x height, row-major,
 *                no padding (values are sensor counts, black ~64, white ~1023)
 *   path.json    image layout + static and per-frame metadata (see k_raw_*)
 *   focus        1 = lock AF + AE precapture first, 0 = shoot now
 *   exposure_ns, iso   manual exposure (see set_exposure); 0 = automatic
 *   variant      RF_V_* flags (0 = normal capture)
 *   path "-"     measure only: nothing is written (for exposure sweeps)
 * The report always includes "measured": the black-subtracted mean of the
 * central quarter of the frame, so the real exposure can be checked
 * without copying 25 MB around. */
static int indraw_raw_impl(const char *path, const char *datetime, int focus_mode,
                  float focus_diopters, int ae_precapture, int64_t exposure_ns,
                  int iso, int variant, int timeout_ms, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    AImage *img = NULL;
    uint8_t *data = NULL;
    int data_len = 0, err = 0, failed, fail_reason, dev_err, w_px = 0, h_px = 0, stride = 0, res_ok;
    int64_t img_ts = -1, res_ts = -1;
    double t0, t_3a, t_image, t_written;
    long frames_before, frames_during;
    double sum = 0.0, black = 64.0;
    long long n_px = 0, n_clip = 0;
    int white = 1023, save = strcmp(path ? path : "-", "-") != 0;
    int want_dng = save && path_is_dng(path);    /* .dng -> DNG, else raw16+json */
    ACameraMetadata *rmeta = NULL;                /* owned copy of this result    */
    int64_t a_exposure = -1, a_frame = -1;        /* applied values, read from     */
    int a_iso = -1, a_fps0 = -1, a_fps1 = -1, a_control = -1; /* rmeta (not globals) */
    float a_focus = -1.0f;
    uint16_t *dng_px = NULL;                      /* contiguous copy for the DNG  */
    float *sh_gains = NULL;
    int sh_cols = 0, sh_rows = 0;
    a3_report_t a3 = { "skipped", -1, -1 };
    ACameraMetadata_const_entry e;
    char side[1024];
    struct timespec dl;
    camera_status_t st;
    FILE *f;
    jw_t w;

    if (!path || !out || !out_size || timeout_ms <= 0) return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "raw", "camera not open", 0);
    if (!g.raw_request) return fail(out, out_size, IR_ESTATE, "raw", "session has no raw16 stream", 0);
    if (snprintf(side, sizeof side, "%s.json", path) >= (int)sizeof side)
        return fail(out, out_size, IR_EARG, "raw", "path too long", 0);
    if (set_exposure(g.raw_request, exposure_ns, iso, out, out_size)) return IR_EARG;
    set_variant(g.raw_request, variant);

    pthread_mutex_lock(&g.lock);                 /* forget any previous frame      */
    if (g.raw_image) { AImage_delete(g.raw_image); g.raw_image = NULL; }
    g.capture_failed = 0;
    g.result_ready = 0;
    g.captured_owed = save ? "captured kind=raw\n" : NULL;   /* a photo: owed */
    pthread_mutex_unlock(&g.lock);

    t0 = now_ms();
    prepare_capture(g.raw_request, focus_mode, focus_diopters, ae_precapture, &a3);
    t_3a = now_ms();
    if (variant & IR_V_PAUSE) preview_pause();
    pthread_mutex_lock(&g.lock);
    frames_before = g.preview_frames;
    pthread_mutex_unlock(&g.lock);
    reqs[0] = g.raw_request;
    flash_arm(g.raw_request, exposure_ns > 0);
    st = ACameraCaptureSession_capture(g.session, &g_still_cbs, 1, reqs, NULL);
    flash_disarm(g.raw_request);
    af_release(focus_mode);                      /* release on every path below   */
    if (st != ACAMERA_OK) {
        if (variant & IR_V_PAUSE) preview_resume();
        return fail(out, out_size, IR_ECAPTURE, "capture", "capture refused", st);
    }

    /* A RAW is useless without its metadata: wait for BOTH image and result. */
    dl = deadline_in(timeout_ms);
    pthread_mutex_lock(&g.lock);
    while (!(g.raw_image && g.result_ready) && !g.capture_failed && !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    frames_during = g.preview_frames - frames_before;
    img = g.raw_image;
    g.raw_image = NULL;
    failed = g.capture_failed;
    fail_reason = g.capture_fail_reason;
    dev_err = g.device_error;
    res_ok = g.result_ready;                     /* read under the lock           */
    /* Copy the result under the lock: reading g.r_meta after unlocking would
     * race a later still capture overwriting it (safe today only because the
     * caller is single-threaded; this makes it robust regardless). */
    if (res_ok && g.r_meta) rmeta = ACameraMetadata_copy(g.r_meta);
    pthread_mutex_unlock(&g.lock);
    t_image = now_ms();
    if (variant & IR_V_PAUSE) preview_resume();  /* viewfinder back on            */
    if (img) captured_now();                     /* if the result has not already */

    if (!img || !res_ok) {
        if (img) AImage_delete(img);
        if (rmeta) ACameraMetadata_free(rmeta); /* may be NULL on this path     */
        if (failed) return fail(out, out_size, IR_ECAPTURE, "capture_failed", "camera reported failure", fail_reason);
        if (dev_err) return fail(out, out_size, IR_ECAPTURE, "device", "device error", dev_err);
        return fail(out, out_size, IR_ECAPTURE, "wait_raw", img ? "no capture result" : "timeout", timeout_ms);
    }

    /* Pixel buffer: plane 0, 2 bytes per pixel, rows may be padded (stride). */
    AImage_getTimestamp(img, &img_ts);
    /* Pair the image with its result NOW, on every path: a mismatch means a late
     * frame from an earlier capture, whose metadata (noise, neutral, shading)
     * must not be written into this file. Refuse it rather than lie. */
    if (rmeta && ACameraMetadata_getConstEntry(rmeta, ACAMERA_SENSOR_TIMESTAMP, &e) ==
            ACAMERA_OK && e.count)
        res_ts = e.data.i64[0];
    /* Read the applied values from rmeta (the under-lock copy), not the globals
     * that a later still capture could overwrite. */
    if (rmeta) {
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count)
            a_exposure = e.data.i64[0];
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_SENSOR_FRAME_DURATION, &e) == ACAMERA_OK && e.count)
            a_frame = e.data.i64[0];
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count)
            a_iso = e.data.i32[0];
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, &e) == ACAMERA_OK && e.count == 2)
            { a_fps0 = e.data.i32[0]; a_fps1 = e.data.i32[1]; }
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_CONTROL_MODE, &e) == ACAMERA_OK && e.count)
            a_control = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(rmeta, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
            a_focus = isfinite(e.data.f[0]) ? e.data.f[0] : -1.0f;
    }
    if (save && res_ts != img_ts) {              /* measure-only may skip this    */
        AImage_delete(img);
        if (rmeta) ACameraMetadata_free(rmeta);
        return fail(out, out_size, IR_ECAPTURE, "raw_sync", "image/result timestamp mismatch", 0);
    }
    AImage_getWidth(img, &w_px);
    AImage_getHeight(img, &h_px);
    if (AImage_getPlaneData(img, 0, &data, &data_len) != AMEDIA_OK ||
        AImage_getPlaneRowStride(img, 0, &stride) != AMEDIA_OK ||
        w_px <= 0 || h_px <= 0 || stride < w_px * 2 ||
        (int64_t)stride * (h_px - 1) + w_px * 2 > data_len) {
        AImage_delete(img);
        if (rmeta) ACameraMetadata_free(rmeta);
        return fail(out, out_size, IR_ECAPTURE, "raw_data", "unexpected raw16 layout", data_len);
    }
    /* Measure the central quarter: mean above black, and clipped pixels. */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) ==
            ACAMERA_OK && e.count == 4)
        black = (e.data.i32[0] + e.data.i32[1] + e.data.i32[2] + e.data.i32[3]) / 4.0;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) ==
            ACAMERA_OK && e.count)
        white = e.data.i32[0];
    {
        int y, x;
        for (y = h_px / 4; y < 3 * h_px / 4; ++y) {
            const uint16_t *row = (const uint16_t *)(data + (size_t)y * stride);
            for (x = w_px / 4; x < 3 * w_px / 4; ++x) {
                sum += row[x];
                n_clip += row[x] >= white;
            }
            n_px += 3 * w_px / 4 - w_px / 4;
        }
    }
    if (want_dng) {
        /* Copy pixels (dropping row padding) and the shading map while the
         * image and result are still valid, then write the DNG. */
        int y;
        dng_px = malloc((size_t)w_px * h_px * 2);
        if (!dng_px) { AImage_delete(img); if (rmeta) ACameraMetadata_free(rmeta); return fail(out, out_size, IR_EFILE, "dng", "out of memory", 0); }
        for (y = 0; y < h_px; ++y)
            memcpy(dng_px + (size_t)y * w_px, data + (size_t)y * stride, (size_t)w_px * 2);
        sh_gains = copy_shading(rmeta, &sh_cols, &sh_rows);
    } else if (save) {
        err = write_rows(path, data, (size_t)w_px * 2, (size_t)stride, (size_t)h_px);
    }
    AImage_delete(img);                          /* buffer back to the reader      */
    if (focus_mode == IR_FOCUS_AUTO || focus_mode == IR_FOCUS_MACRO)
        send_trigger(ACAMERA_CONTROL_AF_TRIGGER_CANCEL,  /* end the one-shot scan */
                     ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);

    if (want_dng) {
        dng_params_t dp;
        char sw[32], dt[24];
        snprintf(sw, sizeof sw, "RAWfish-ir %s", IR_VERSION);
        dng_params_from(&dp, rmeta, dng_px, w_px, h_px, sh_gains, sh_cols, sh_rows,
                        a_exposure, a_iso, sw);
        if (datetime && datetime[0]) dp.datetime = datetime; /* caller's instant */
        else { exif_datetime(dt, sizeof dt); dp.datetime = dt; }
        err = dng_write(path, &dp);
        if (!err) write_dng_preview(path, &dp);       /* best effort: a thumbnail   */
        free(dng_px);
        free(sh_gains);
        if (err) { if (rmeta) ACameraMetadata_free(rmeta); return fail(out, out_size, IR_EFILE, "dng", err > 0 ? strerror(err) : "bad dng params", err); }
        goto report;                             /* no .json sidecar for DNG      */
    }
    if (err) { if (rmeta) ACameraMetadata_free(rmeta); return fail(out, out_size, IR_EFILE, "write", strerror(err), err); }

    /* Sidecar: layout, then everything needed to develop or wrap as DNG.
     * (res_ts was read and matched right after the capture.) */
    f = save ? fopen(side, "w") : NULL;
    if (!save) goto report;                      /* measure-only: no files        */
    if (!f) { if (rmeta) ACameraMetadata_free(rmeta); return fail(out, out_size, IR_EFILE, "sidecar", strerror(errno), errno); }
    /* make / model / orientation since 0.1.1: the PC converter
     * (scripts/raw16-to-dng.py) wrote "Jolla Phone 2" and no rotation for
     * every phone (October 2026 audit) */
    fprintf(f, "{\n  \"indraw\": \"%s\",\n  \"camera_id\": \"%s\",\n"
               "  \"make\": \"%s\",\n  \"model\": \"%s\",\n  \"photo_orientation\": %d,\n"
               "  \"layout\": {\"format\": \"uint16le\", \"width\": %d, \"height\": %d, "
               "\"source_row_stride\": %d},\n  \"image_timestamp\": %lld,\n"
               "  \"matches_result\": %s,\n  \"static\": {",
            IR_VERSION, g.camera_id, g.make, g.model, (int)photo_orientation_deg(), w_px, h_px, stride, (long long)img_ts,
            res_ts == img_ts ? "true" : "false");
    dump_tags(f, g.chars, k_raw_static, sizeof k_raw_static / sizeof k_raw_static[0]);
    fputs("\n  },\n  \"result\": {", f);
    dump_tags(f, rmeta, k_raw_result, sizeof k_raw_result / sizeof k_raw_result[0]);
    fputs("\n  }\n}\n", f);
    if (fclose(f) != 0) { if (rmeta) ACameraMetadata_free(rmeta); return fail(out, out_size, IR_EFILE, "sidecar", strerror(errno), errno); }
report:
    if (rmeta) ACameraMetadata_free(rmeta);      /* done with the result copy     */
    rmeta = NULL;
    t_written = now_ms();
    if (save) emit_written(want_dng ? "dng" : "raw16", path);

    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"path\":");
    jw_str(&w, path);                            /* JSON-escaped path            */
    jw_add(&w, ",\"sidecar\":");
    jw_str(&w, want_dng ? "-" : (save ? side : "-")); /* escaped sidecar path    */
    jw_add(&w, ",\"size\":[%d,%d],"
               "\"bytes\":%lld,\"3a\":{\"result\":\"%s\",\"ae\":\"%s\",\"af\":\"%s\"},"
               "\"ms\":{\"3a\":%.1f,\"image\":%.1f,\"written\":%.1f},"
               "\"applied\":{\"exposure_ns\":%lld,\"frame_ns\":%lld,\"iso\":%d,\"fps_range\":[%d,%d],"
               "\"control\":%d,\"focus_diopters\":%.3f,"
               "\"matches_image\":%s},\"requested\":{\"exposure_ns\":%lld,\"iso\":%d},"
               "\"measured\":{\"center_mean\":%.2f,\"center_clip_pct\":%.3f},"
               "\"preview_frames_during\":%ld,",
           w_px, h_px,
           save ? (long long)w_px * h_px * 2 : 0LL, a3.result,
           ae_state_name(a3.ae), af_state_name(a3.af),
           t_3a - t0, t_image - t0, t_written - t0,
           (long long)a_exposure, (long long)a_frame, a_iso, a_fps0, a_fps1,
           a_control, a_focus,
           res_ts == img_ts ? "true" : "false", (long long)exposure_ns, iso,
           n_px ? sum / n_px - black : -1.0, n_px ? 100.0 * n_clip / n_px : -1.0, frames_during);
    add_variant(&w, variant);
    jw_add(&w, "}");
    return IR_OK;
}

/* Sensor white (saturation) level, with a sane default. */
static int white_level(void)
{
    ACameraMetadata_const_entry e;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count)
        return e.data.i32[0];
    return 1023;
}

/* One RAW16 capture on the open session, for stacking. On success returns 0,
 * hands the image out through *img (caller deletes it) and fills *stride, *w,
 * *h. On failure returns an RF_E* code and writes the JSON error into out.
 * The exposure/variant are already set on g.raw_request by the caller. */
static int capture_one_raw(int timeout_ms, AImage **img, int *w, int *h, int *stride,
                           char *out, size_t out_size)
{
    ACaptureRequest *reqs[1] = { g.raw_request };
    int data_len = 0, failed, dev_err, res_ok, fail_reason;
    uint8_t *data = NULL;
    struct timespec dl;
    camera_status_t st;

    pthread_mutex_lock(&g.lock);
    if (g.raw_image) { AImage_delete(g.raw_image); g.raw_image = NULL; }
    g.capture_failed = 0;
    g.result_ready = 0;
    g.captured_owed = NULL;                  /* a stack frame / a burst signals by itself */
    pthread_mutex_unlock(&g.lock);

    st = ACameraCaptureSession_capture(g.session, &g_still_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK) return fail(out, out_size, IR_ECAPTURE, "capture", "capture refused", st);

    dl = deadline_in(timeout_ms);
    pthread_mutex_lock(&g.lock);
    while (!(g.raw_image && g.result_ready) && !g.capture_failed && !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    *img = g.raw_image;
    g.raw_image = NULL;
    failed = g.capture_failed;
    fail_reason = g.capture_fail_reason;
    dev_err = g.device_error;
    res_ok = g.result_ready;
    pthread_mutex_unlock(&g.lock);

    if (!*img || !res_ok) {
        int had_image = (*img != NULL);          /* note it before we free *img   */
        if (*img) { AImage_delete(*img); *img = NULL; }
        if (failed) return fail(out, out_size, IR_ECAPTURE, "capture_failed", "camera reported failure", fail_reason);
        if (dev_err) return fail(out, out_size, IR_ECAPTURE, "device", "device error", dev_err);
        return fail(out, out_size, IR_ECAPTURE, "wait_raw",
                    had_image ? "no capture result" : "timeout", timeout_ms);
    }
    /* Make sure the result belongs to this image: compare their timestamps.
     * A mismatch means a frame was dropped or reordered, which would silently
     * pair pixels with the wrong metadata. */
    {
        int64_t img_ts = -1, res_ts = -1;
        ACameraMetadata_const_entry te;
        AImage_getTimestamp(*img, &img_ts);
        if (g.r_meta && ACameraMetadata_getConstEntry(g.r_meta, ACAMERA_SENSOR_TIMESTAMP, &te) ==
                ACAMERA_OK && te.count) res_ts = te.data.i64[0];
        if (img_ts != res_ts) {
            AImage_delete(*img);
            *img = NULL;
            return fail(out, out_size, IR_ECAPTURE, "raw_sync", "image/result timestamp mismatch", 0);
        }
    }
    AImage_getWidth(*img, w);
    AImage_getHeight(*img, h);
    if (AImage_getPlaneData(*img, 0, &data, &data_len) != AMEDIA_OK ||
        AImage_getPlaneRowStride(*img, 0, stride) != AMEDIA_OK ||
        *w <= 0 || *h <= 0 || *stride < *w * 2 ||
        (int64_t)*stride * (*h - 1) + *w * 2 > data_len) {
        AImage_delete(*img);
        *img = NULL;
        return fail(out, out_size, IR_ECAPTURE, "raw_data", "unexpected raw16 layout", data_len);
    }
    return IR_OK;
}

/* Applies sensor-capability CORRECTIONS from a device override profile
 * (nature 2 in docs/DEVICE-PROFILES.md). The Sailfish side parses
 * override-hal.json and passes typed values here, so the bridge needs no JSON
 * parser and the correction is explicit and atomic. Each argument is 0 / <=0
 * for "no override, keep what the HAL/our constants say".
 *   observed_shutter_max_ns  the real single-exposure ceiling found by testing;
 *                            overrides the built-in 1/2 s stacking sub-frame max
 * Must be called after indraw_open (it stores into the open session's state). */
IR_API int indraw_set_override(int64_t observed_shutter_max_ns, int64_t min_frame_ns,
                               char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.manager) return fail(out, out_size, IR_ESTATE, "override", "camera not open", 0);
    g.override_shutter_max_ns = observed_shutter_max_ns > 0 ? observed_shutter_max_ns : 0;
    g.override_min_frame_ns = min_frame_ns > 0 ? min_frame_ns : 0;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"observed_shutter_max_ns\":%lld,\"min_frame_ns\":%lld}",
           (long long)g.override_shutter_max_ns, (long long)g.override_min_frame_ns);
    return IR_OK;
}

/* Sets the preview-aid thresholds (device-tunable, from override-hal.json via
 * the CLI): the peaking edge gradient, and the zebra clipping level for jpg
 * (0..255 on the 8-bit viewfinder) and for raw (per-mille of the white level:
 * how close to saturation the RAW must be). 0 keeps the built-in default
 * (peak 40, zebra jpg 250, zebra raw 992). Published in caps for the UI. */
IR_API int indraw_set_aids_override(int64_t peak_edge, int64_t zebra_jpeg, int64_t zebra_raw,
                                    int64_t peak_width, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    g.override_peak_edge = peak_edge > 0 ? peak_edge : 0;
    g.override_zebra_jpeg = zebra_jpeg > 0 ? zebra_jpeg : 0;
    g.override_zebra_raw = zebra_raw > 0 ? zebra_raw : 0;
    g.override_peak_width = peak_width > 0 ? peak_width : 0;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"peak_edge\":%lld,\"zebra_jpeg\":%lld,\"zebra_raw\":%lld,\"peak_width\":%lld}",
           (long long)g.override_peak_edge, (long long)g.override_zebra_jpeg, (long long)g.override_zebra_raw,
           (long long)g.override_peak_width);
    return IR_OK;
}

/* Sets the focus calibration table: `n` points mapping a real focus diopter
 * (1/metres, 0 = infinity) to the diopter to send the HAL. The Sailfish side
 * parses the table from override-hal.json and passes it here, so the bridge
 * stays JSON-free. Points must be given ascending by real diopter; up to 16.
 * n = 0 clears the table (back to the 1:1 uncalibrated mapping). */
IR_API int indraw_set_focus_calibration(const float *real_d, const float *hal_d, int n,
                                        char *out, size_t out_size)
{
    jw_t w;
    int i;
    if (!out || !out_size) return IR_EARG;
    if (!g.manager) return fail(out, out_size, IR_ESTATE, "focus_cal", "camera not open", 0);
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    /* The table must be usable by focus_translate / focus_untranslate: real
     * diopters strictly increasing, lens values non-decreasing with them (a
     * lens does not go the other way). Anything else is refused as a whole
     * rather than interpolated into nonsense (an audit's point, 0.47.15). */
    for (i = 0; i < n; ++i) {
        if (!(real_d[i] >= 0) || !(hal_d[i] >= 0))
            return fail(out, out_size, IR_EARG, "focus_cal", "negative or NaN point", i);
        if (i > 0 && !(real_d[i] > real_d[i - 1]))
            return fail(out, out_size, IR_EARG, "focus_cal", "real diopters must increase", i);
        if (i > 0 && hal_d[i] < hal_d[i - 1])
            return fail(out, out_size, IR_EARG, "focus_cal", "lens values must not decrease", i);
    }
    pthread_mutex_lock(&g.lock);                    /* focus_untranslate reads it on the callbacks */
    g.focus_cal_n = n;
    for (i = 0; i < n; ++i) { g.focus_cal_real[i] = real_d[i]; g.focus_cal_hal[i] = hal_d[i]; }
    pthread_mutex_unlock(&g.lock);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"focus_calibration_points\":%d}", n);
    return IR_OK;
}

/* Long exposure by stacking. The HAL caps a real single exposure near 1/2 s
 * (longer requests are wrong or never arrive), so a "film-equivalent" exposure
 * is built from many sub-frames summed into 16-bit pixels, with the DNG's
 * black/white levels scaled to match -- a lossless brute sum, correct exposure.
 *   path         .dng -> a DNG; else uint32 sum pixels + path.json
 *   film_ns      wanted film-equivalent exposure; it sets the BRIGHTNESS. The
 *                base frame count is film_ns / sub_ns.
 *   sub_ns       per-frame exposure (0 -> 500 ms, the measured safe maximum);
 *                shorter sub-frames clip highlights later, at the cost of more
 *                frames for the same film_ns
 *   stk_denoise  1, 2 or 4: stack this many times the base frames to cut noise
 *                by sqrt(stk_denoise) more, then rescale back to film_ns's
 *                brightness (so denoise changes noise, not brightness)
 *   iso          manual ISO (0 -> sensor minimum)
 * N = (film_ns / sub_ns) * stk_denoise frames are summed; the sum is divided by
 * stk_denoise (and by any extra power of two needed to fit 16 bits) so the
 * written pixels carry the brightness of film_ns. Black/white levels are
 * multiplied by the same net frame-equivalent, keeping exposure truthful.
 * Each frame is manual-exposure with AE off; AF is turned OFF for the whole
 * burst so the focus cannot drift. The preview is paused throughout. */
static int indraw_stack_impl(const char *path, const char *datetime, int64_t film_ns, int64_t sub_ns,
                    int stk_denoise, int iso, char *out, size_t out_size)
{
    /* Per-frame ceiling: the device override (observed_shutter_max_ns) if set,
     * else the built-in 1/2 s that we proved exact on the JP2. This is the one
     * value that is hardware-specific, so it comes from the device profile. */
    const int64_t SAFE_SUB = g.override_shutter_max_ns > 0 ? g.override_shutter_max_ns : 500000000;
    AImage *img = NULL;
    uint32_t *acc = NULL;
    uint8_t *data = NULL;
    int w = 0, h = 0, i, n, rc = IR_OK;
    int64_t base;                                  /* base frame count (int64)      */
    int aborted = 0;                               /* stopped early (indraw_request_abort) */
    int want_dng = path_is_dng(path);
    double base_eff;                               /* frames of light in the result */
    double blk_sum[4] = { 0, 0, 0, 0 };            /* sum of every frame's black, per CFA phase */
    int blk_frames = 0;                            /* frames whose black was added  */
    double black = 64.0, peak = 0.0;
    float *sh_gains = NULL;
    int sh_cols = 0, sh_rows = 0;
    ACameraMetadata *first_meta = NULL;           /* owned copy of frame 0 result  */
    int64_t img_ts = -1, res_ts = -1;
    double t0, t_written;
    long long n_sat = 0;
    char side[1024], tmp[1024];
    ACameraMetadata_const_entry e;
    FILE *f;
    jw_t w_;

    if (!path || !out || !out_size || film_ns <= 0) return IR_EARG;
    if (!g.session || !g.raw_request)
        return fail(out, out_size, IR_ESTATE, "stack", "camera not open with a raw16 stream", 0);
    if (!strcmp(path, "-")) return fail(out, out_size, IR_EARG, "stack", "needs a real path", 0);
    if (stk_denoise != 1 && stk_denoise != 2 && stk_denoise != 4)
        return fail(out, out_size, IR_EARG, "stack", "stk_denoise must be 1, 2 or 4", stk_denoise);
    if (snprintf(side, sizeof side, "%s.json", path) >= (int)sizeof side ||
        snprintf(tmp, sizeof tmp, "%s.part", path) >= (int)sizeof tmp)
        return fail(out, out_size, IR_EARG, "stack", "path too long", 0);
    if (sub_ns <= 0) sub_ns = SAFE_SUB;
    if (sub_ns > SAFE_SUB) sub_ns = SAFE_SUB;    /* never above the safe maximum   */
    base = (film_ns + sub_ns - 1) / sub_ns;      /* base frames set brightness (int64) */
    if (base < 1) base = 1;
    /* 4096 frames: 8 min at 1/2 s with the denoise x4 (32-bit accumulator:
     * 4096 x 1023 fits with room). Beyond that the per-frame HAL latency and
     * the heat make it a bad idea. */
    if (base * (int64_t)stk_denoise > 4096)      /* test in int64, before the cast */
        return fail(out, out_size, IR_EARG, "stack", "too many frames (max 4096)",
                    (int)(base > 4096 ? 4096 : base));
    n = (int)(base * stk_denoise);               /* extra frames for less noise    */
    base_eff = (double)base;
    /* How the stack becomes a DNG: see the "if (want_dng)" block below. */

    /* Validate the manual exposure FIRST, so invalid params don't waste an AF. */
    if (set_exposure(g.raw_request, sub_ns, iso, out, out_size)) return IR_EARG;
    /* FREEZE the focus where the live preview has it (the HOLD rule of
     * set_focus): no scan here, the point was made before the shot (circle,
     * continuous, or the distance), as for a photo. Without a position, AF off
     * had sent the lens to infinity mid-stack. */
    set_focus(g.raw_request, IR_FOCUS_HOLD, 0);
    g_abort = 0;
    {   /* the stack starts: the interface shows its progress from here (it
         * no longer sees the "stack" command itself: the controller issues it) */
        char ev[64];
        snprintf(ev, sizeof ev, "stack_start n=%d\n", n);
        emit_event(ev);
    }
    set_variant(g.raw_request, 0);
    preview_pause();
    t0 = now_ms();

    for (i = 0; i < n; ++i) {
        int cw, ch, cs;
        char ev[128];
        if (g_abort) {                                   /* keep what we have      */
            aborted = 1;
            if (i == 0) { rc = fail(out, out_size, IR_ECAPTURE, "stack", "aborted before the first frame", 0); goto done; }
            break;
        }
        rc = capture_one_raw(10000, &img, &cw, &ch, &cs, out, out_size);
        if (rc) goto done;                       /* out already holds the error    */
        /* Progress for the UI: frame i+1 of n, elapsed so far. */
        snprintf(ev, sizeof ev, "progress kind=stack frame=%d n=%d elapsed_ms=%.0f\n", i + 1, n, now_ms() - t0);
        emit_event(ev);
        if (i == 0) {                            /* first frame sets the geometry  */
            w = cw; h = ch;
            acc = calloc((size_t)w * h, sizeof *acc);
            if (!acc) { rc = fail(out, out_size, IR_EFILE, "stack", "out of memory", 0); AImage_delete(img); img = NULL; goto done; }
            AImage_getTimestamp(img, &img_ts);
            if (ACameraMetadata_getConstEntry(g.r_meta, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count)
                res_ts = e.data.i64[0];
            /* Own a copy of the first frame's result and shading: g.r_meta is
             * overwritten by every later frame, so the DNG/sidecar must not read
             * it directly. */
            first_meta = ACameraMetadata_copy(g.r_meta);
            if (want_dng) sh_gains = copy_shading(g.r_meta, &sh_cols, &sh_rows);  /* first frame */
        } else if (cw != w || ch != h) {         /* geometry must not change       */
            AImage_delete(img);
            img = NULL;                          /* avoid a double free at done:  */
            rc = fail(out, out_size, IR_ECAPTURE, "stack", "frame size changed mid-burst", 0);
            goto done;
        }
        {
            int y, x, data_len = 0, cs = 0;
            if (AImage_getPlaneData(img, 0, &data, &data_len) != AMEDIA_OK ||
                AImage_getPlaneRowStride(img, 0, &cs) != AMEDIA_OK ||
                cs < w * 2 || (int64_t)cs * (h - 1) + w * 2 > data_len) {
                AImage_delete(img);
                img = NULL;                      /* avoid a double free at done:  */
                rc = fail(out, out_size, IR_ECAPTURE, "stack", "unexpected raw16 layout mid-burst", 0);
                goto done;
            }
            for (y = 0; y < h; ++y) {             /* sum into the 32-bit accumulator */
                const uint16_t *src = (const uint16_t *)(data + (size_t)y * cs);
                uint32_t *dst = acc + (size_t)y * w;
                for (x = 0; x < w; ++x) dst[x] += src[x];  /* 4096 x 1023 fits in 32 bits */
            }
            /* This frame's black level, per CFA phase: the HAL's per-frame
             * measure (SENSOR_DYNAMIC_BLACK_LEVEL, from the sensor's optical
             * black rows) when it gives one, else the static pattern. The black
             * drifts with temperature over a long stack; removing each frame's
             * own black, not N times the first one, keeps the shadows neutral. */
            {
                ACameraMetadata_const_entry be;
                int k;
                if (ACameraMetadata_getConstEntry(g.r_meta, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL, &be) == ACAMERA_OK && be.count == 4) {
                    for (k = 0; k < 4; ++k) blk_sum[k] += be.data.f[k];
                } else if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &be) == ACAMERA_OK && be.count == 4) {
                    for (k = 0; k < 4; ++k) blk_sum[k] += be.data.i32[k];
                } else {
                    for (k = 0; k < 4; ++k) blk_sum[k] += 64.0;   /* last resort, logged as such below */
                }
                blk_frames++;
            }
        }
        AImage_delete(img);                       /* back to the reader before next */
        img = NULL;
    }
    preview_resume();
    emit_event("captured kind=stack\n");           /* last sub-frame exposed        */
    {
        /* Tell the UI how it went (frames, time: it refines its estimates). An
         * abort leaves `i` of n frames in the sum: the picture is then a stack
         * of i frames divided as planned, i.e. the brightness of i/denoise
         * sub-frames, and every level and the EXIF exposure follow that. */
        char ev[160];
        snprintf(ev, sizeof ev, "stack_done frames=%d n=%d aborted=%d ms=%.0f\n", aborted ? i : n, n, aborted, now_ms() - t0);
        emit_event(ev);
        if (aborted) {
            base_eff = (double)i / (double)stk_denoise;
            n = i;
            film_ns = (int64_t)(base_eff * (double)sub_ns + 0.5);
            base = (int64_t)(base_eff + 0.5);              /* reports (int64)      */
        }
    }

    /* Stats on the summed centre: peak value and saturated-input pixels. */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) ==
            ACAMERA_OK && e.count == 4)
        black = (e.data.i32[0] + e.data.i32[1] + e.data.i32[2] + e.data.i32[3]) / 4.0;
    {
        int y, x;
        for (y = h / 4; y < 3 * h / 4; ++y)
            for (x = w / 4; x < 3 * w / 4; ++x) {
                uint32_t v = acc[(size_t)y * w + x];
                if (v > peak) peak = v;
            }
    }

    if (want_dng) {
        /* A stack is written as ONE long exposure would be (summing and
         * averaging calibrated frames give the same signal-to-noise ratio; the
         * sum keeps the brightness of the total exposure -- docs/STACKING.md).
         * The sum holds the light of n frames plus n black pedestals. Per pixel,
         * with Bk = this CFA phase's black of frame k (its own measured black):
         *     v = (sum - sum_k Bk) / denoise   -> base frames of light, no black
         *     v = min(v, W - B)                -> what saturates, saturates
         *     out = scale x (v + B)            -> one black B on top
         * B and W are the single frame's black and white (from frame 0), and
         * `scale` = the largest integer with scale x W <= 65535 (64 for a
         * 10-bit sensor): the DNG's black and white are multiplied by the same
         * factor, so a raw developer sees exactly the same picture, but the
         * fractions left by the per-frame blacks and the denoise division are
         * kept instead of being rounded to whole sensor units.
         * Earlier versions scaled black/white by the frame count (a developer
         * then cancels the gain: a Σ8s as bright as 1/2 s) or kept one black
         * under n pedestals (lifted, magenta shadows).
         * Noise profile: in units of the single-frame range the shot term is
         * unchanged by the sum, the read term grows with base, the denoise
         * averaging divides both. EXIF exposure = the photo-equivalent. */
        dng_params_t dp;
        uint16_t *px16 = malloc((size_t)w * h * 2);
        char sw[32], dt[24];
        int y, x, k, scale;
        if (!px16) { rc = fail(out, out_size, IR_EFILE, "stack", "out of memory", 0); goto done; }
        snprintf(sw, sizeof sw, "RAWfish-ir %s", IR_VERSION);
        /* Metadata (levels, matrices, noise) from the first frame. */
        dng_params_from(&dp, first_meta ? first_meta : g.r_meta, px16, w, h,
                        sh_gains, sh_cols, sh_rows, (int64_t)(base_eff * sub_ns + 0.5), g.r_iso, sw);
        if (datetime && datetime[0]) dp.datetime = datetime; /* caller's instant */
        else { exif_datetime(dt, sizeof dt); dp.datetime = dt; }
        scale = dp.white > 0 ? 65535 / dp.white : 1;
        if (scale < 1) scale = 1;
        if (scale > 64) scale = 64;
        {
            const double W = dp.white;
            double B[4], blk_frame_sum[4];
            for (k = 0; k < 4; ++k) {
                B[k] = dp.have_black4 ? dp.black4[k] : dp.black;
                /* Frames summed so far (an abort leaves fewer than n). */
                blk_frame_sum[k] = blk_frames > 0 ? blk_sum[k] * ((double)n / blk_frames) : B[k] * n;
            }
            for (y = 0; y < h; ++y)
                for (x = 0; x < w; ++x) {
                    const int ph = ((y & 1) << 1) | (x & 1);
                    double v = ((double)acc[(size_t)y * w + x] - blk_frame_sum[ph]) / stk_denoise;
                    if (v < -B[ph]) v = -B[ph];            /* never below 0 in the file */
                    if (v > W - B[ph]) v = W - B[ph];      /* saturation, as one exposure */
                    v = scale * (v + B[ph]);
                    px16[(size_t)y * w + x] = (uint16_t)(v + 0.5);
                }
        }
        /* The DNG's levels follow the same scale (normalisation unchanged). */
        dp.black = dp.black * scale;
        dp.white = dp.white * scale;
        if (dp.have_black4) for (k = 0; k < 4; ++k) dp.black4[k] = dp.black4[k] * scale;
        if (dp.have_noise) {
            dp.noise[0] *= 1.0 / stk_denoise;             /* signal (shot) term       */
            dp.noise[1] *= base_eff / stk_denoise;        /* read-noise term          */
        }
        rc = dng_write(path, &dp);
        if (!rc) write_dng_preview(path, &dp);        /* best effort: a thumbnail   */
        free(px16);
        if (rc) { rc = fail(out, out_size, IR_EFILE, "dng", rc > 0 ? strerror(rc) : "bad dng params", rc); goto done; }
        t_written = now_ms();
        emit_written("dng", path);                     /* like any other photo       */
        jw_init(&w_, out, out_size);
        jw_add(&w_, "{\"ok\":true,\"path\":");
        jw_str(&w_, path);                       /* JSON-escaped path            */
        jw_add(&w_, ",\"sidecar\":\"-\",\"size\":[%d,%d],\"bytes\":%lld,"
                    "\"frames\":%d,\"sub_exposure_ns\":%lld,\"stk_denoise\":%d,"
                    "\"film_exposure_ns\":%lld,\"iso\":%d,"
                    "\"equiv\":{\"brightness_ns\":%lld,\"noise_like_ns\":%lld},"
                    "\"saturated\":%s,\"ms\":{\"total\":%.1f}}",
               w, h, (long long)w * h * 2, n, (long long)sub_ns, stk_denoise,
               (long long)(base * sub_ns), g.r_iso,
               /* brightness equivalent: the film exposure (base frames) */
               (long long)(base * sub_ns),
               /* noise equivalent: a single exposure of the whole stacked time */
               (long long)sub_ns * n,
               /* a summed pixel saturates if every input frame clipped */
               (peak >= (double)white_level() * n) ? "true" : "false",
               t_written - t0);
        goto done;
    }

    /* Write the 32-bit sum (little-endian), row by row, via <path>.part. */
    f = fopen(tmp, "wb");
    if (!f) { rc = fail(out, out_size, IR_EFILE, "write", strerror(errno), errno); goto done; }
    {
        int y, x, werr = 0;
        for (y = 0; y < h && !werr; ++y) {
            for (x = 0; x < w; ++x) {
                uint32_t v = acc[(size_t)y * w + x];
                uint8_t b4[4] = { v, v >> 8, v >> 16, v >> 24 };
                if (fwrite(b4, 1, 4, f) != 4) werr = 1;
            }
        }
        if (fclose(f) != 0) werr = 1;
        if (werr) { unlink(tmp); rc = fail(out, out_size, IR_EFILE, "write", "short write", 0); goto done; }
    }
    if (rename(tmp, path) != 0) { rc = fail(out, out_size, IR_EFILE, "write", strerror(errno), errno); goto done; }

    f = fopen(side, "w");
    if (!f) { rc = fail(out, out_size, IR_EFILE, "sidecar", strerror(errno), errno); goto done; }
    fprintf(f, "{\n  \"indraw\": \"%s\",\n  \"camera_id\": \"%s\",\n"
               "  \"make\": \"%s\",\n  \"model\": \"%s\",\n  \"photo_orientation\": %d,\n"
               "  \"layout\": {\"format\": \"uint32le\", \"width\": %d, \"height\": %d},\n"
               "  \"stack\": {\"frames\": %d, \"sub_exposure_ns\": %lld, \"stk_denoise\": %d, "
               "\"film_exposure_ns\": %lld, \"iso\": %d},\n"
               "  \"image_timestamp\": %lld,\n  \"matches_result\": %s,\n  \"static\": {",
            IR_VERSION, g.camera_id, g.make, g.model, (int)photo_orientation_deg(), w, h, n, (long long)sub_ns, stk_denoise,
            (long long)(base * sub_ns), g.r_iso,
            (long long)img_ts, res_ts == img_ts ? "true" : "false");
    dump_tags(f, g.chars, k_raw_static, sizeof k_raw_static / sizeof k_raw_static[0]);
    fputs("\n  },\n  \"result\": {", f);          /* metadata of the FIRST frame    */
    dump_tags(f, first_meta ? first_meta : g.r_meta, k_raw_result,
              sizeof k_raw_result / sizeof k_raw_result[0]);
    fputs("\n  }\n}\n", f);
    if (fclose(f) != 0) { rc = fail(out, out_size, IR_EFILE, "sidecar", strerror(errno), errno); goto done; }

    /* A summed pixel saturates if any input frame was clipped: white x frames. */
    {
        double full = ((double)white_level() - black) * n;
        n_sat = full > 0 && peak - black * n >= full ? 1 : 0;
    }
    t_written = now_ms();
    emit_written(path_is_dng(path) ? "dng" : "stack", path);
    jw_init(&w_, out, out_size);
    jw_add(&w_, "{\"ok\":true,\"path\":");
    jw_str(&w_, path);                           /* JSON-escaped path            */
    jw_add(&w_, ",\"sidecar\":");
    jw_str(&w_, side);                           /* escaped sidecar path         */
    jw_add(&w_, ",\"size\":[%d,%d],"
                "\"bytes\":%lld,\"frames\":%d,\"sub_exposure_ns\":%lld,\"stk_denoise\":%d,"
                "\"film_exposure_ns\":%lld,\"iso\":%d,\"peak_above_black\":%.0f,"
                "\"saturated\":%s,\"ms\":{\"total\":%.1f}}",
           w, h, (long long)w * h * 4, n, (long long)sub_ns, stk_denoise,
           (long long)(base * sub_ns), g.r_iso, peak - black * n,
           n_sat ? "true" : "false", t_written - t0);

done:
    free(acc);
    free(sh_gains);
    if (first_meta) ACameraMetadata_free(first_meta);
    if (img) AImage_delete(img);
    if (g.af_continuous) {                       /* restore continuous AF on every path */
        uint8_t af_cont = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        ACaptureRequest_setEntry_u8(g.raw_request, ACAMERA_CONTROL_AF_MODE, 1, &af_cont);
    }
    preview_resume();                            /* harmless if already running    */
    return rc;
}

/* Builds a darktable noise profile by sweeping ISO.
 *
 * darktable's denoise(profiled) module reads its noise model from a
 * noiseprofiles.json indexed by maker/model, with, per ISO, a poissonian
 * coefficient a[3] and a gaussian b[3] for R,G,B: variance = a*value + b with
 * the value normalised to [0,1]. Camera2 reports SENSOR_NOISE_PROFILE per
 * capture as (scale S, offset O) per CFA channel, in the same normalised units,
 * so a = S and b = O (greens averaged for the single G). We take one RAW per
 * ISO step (the sensor measures its own noise; the scene does not matter) and
 * write the JSON with a comment telling the user where to install it.
 *   path     the .json to write
 *   sub_ns   per-shot exposure (0 -> 1/100 s; the profile is ISO-dependent only)
 * The report lists, per ISO, the a/b coefficients, so one can see at a glance
 * whether the HAL returns real per-ISO values or a fixed model. */
static int indraw_noiseprofile_impl(const char *path, int64_t sub_ns, char *out, size_t out_size)
{
    /* ISO steps to try; each is used only if it falls in the sensor's range. */
    static const int iso_steps[] = { 100, 200, 400, 800, 1600, 3200, 6400,
                                     12800, 19200, 25600, 51200, 102400 };
    const int n_steps = (int)(sizeof iso_steps / sizeof iso_steps[0]);
    ACameraMetadata_const_entry e;
    int min_iso = 0, max_iso = 0, i, written = 0, rc = IR_OK, p_cfa = 3;
    double t0 = now_ms();
    char tmp[1024];
    FILE *f;
    jw_t w;
    /* Collected per accepted ISO step, for the JSON and the report. */
    struct { int iso; double a[3], b[3]; int have; } rows[12];

    if (!path || !out || !out_size) return IR_EARG;
    if (!g.session || !g.raw_request)
        return fail(out, out_size, IR_ESTATE, "noiseprofile", "camera not open with a raw16 stream", 0);
    if (snprintf(tmp, sizeof tmp, "%s.part", path) >= (int)sizeof tmp)
        return fail(out, out_size, IR_EARG, "noiseprofile", "path too long", 0);
    if (sub_ns <= 0) sub_ns = 10000000;          /* 1/100 s default              */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) ==
            ACAMERA_OK && e.count == 2) { min_iso = e.data.i32[0]; max_iso = e.data.i32[1]; }
    if (!max_iso) return fail(out, out_size, IR_EARG, "noiseprofile", "manual ISO not supported", 0);
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) ==
            ACAMERA_OK && e.count) p_cfa = e.data.u8[0];

    /* Freeze focus for the whole sweep (the scene does not matter, but a moving
     * lens would waste time hunting). */
    {
        a3_report_t a3;
        uint8_t af_off = ACAMERA_CONTROL_AF_MODE_OFF;
        if (g.af_continuous) { run_3a(2000, &a3); af_release(IR_FOCUS_AUTO); } /* don't leave it locked */
        ACaptureRequest_setEntry_u8(g.raw_request, ACAMERA_CONTROL_AF_MODE, 1, &af_off);
    }
    preview_pause();

    for (i = 0; i < n_steps; ++i) {
        int iso = iso_steps[i];
        AImage *img = NULL;
        int cw, ch, cs;
        rows[i].have = 0;
        if (iso < min_iso || iso > max_iso) continue;   /* outside sensor range  */
        if (set_exposure(g.raw_request, sub_ns, iso, out, out_size)) { rc = IR_EARG; goto restore; }
        set_variant(g.raw_request, 0);
        if (capture_one_raw(10000, &img, &cw, &ch, &cs, out, out_size)) { rc = IR_ECAPTURE; goto restore; }
        AImage_delete(img);                       /* we only need the metadata     */
        /* Read the noise profile the HAL attached to this capture. */
        if (g.r_meta && ACameraMetadata_getConstEntry(g.r_meta, ACAMERA_SENSOR_NOISE_PROFILE, &e) ==
                ACAMERA_OK && e.type == ACAMERA_TYPE_DOUBLE && e.count >= 8) {
            /* (scale, offset) per CFA channel, in the sensor's mosaic order. Find
             * which of the 4 positions is R, B and the two greens from the CFA
             * arrangement, so this is right on any phone, not just BGGR. The 2x2
             * colours (row-major) per arrangement: */
            static const int cfa_col[4][4] = {    /* 0=R 1=G 2=B, per position   */
                { 0, 1, 1, 2 },  /* RGGB */
                { 1, 0, 2, 1 },  /* GRBG */
                { 1, 2, 0, 1 },  /* GBRG */
                { 2, 1, 1, 0 },  /* BGGR */
            };
            const int *col = cfa_col[p_cfa & 3];
            double gs = 0, go = 0;
            int k, ng = 0;
            for (k = 0; k < 4; ++k) {
                if (col[k] == 0) { rows[i].a[0] = e.data.d[k * 2]; rows[i].b[0] = e.data.d[k * 2 + 1]; } /* R */
                else if (col[k] == 2) { rows[i].a[2] = e.data.d[k * 2]; rows[i].b[2] = e.data.d[k * 2 + 1]; } /* B */
                else { gs += e.data.d[k * 2]; go += e.data.d[k * 2 + 1]; ++ng; } /* G (two of them) */
            }
            rows[i].a[1] = ng ? gs / ng : 0;      /* G scale = mean of greens    */
            rows[i].b[1] = ng ? go / ng : 0;      /* G offset                    */
            rows[i].iso = iso;
            rows[i].have = 1;
            ++written;
        }
    }

restore:
    preview_resume();
    if (g.af_continuous) {                         /* restore continuous AF        */
        uint8_t af_cont = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        ACaptureRequest_setEntry_u8(g.raw_request, ACAMERA_CONTROL_AF_MODE, 1, &af_cont);
    }
    if (rc) return rc;                             /* out already holds the error  */
    if (!written) return fail(out, out_size, IR_ECAPTURE, "noiseprofile", "no noise profile reported", 0);

    /* Write the darktable noiseprofiles.json. The leading "comment" tells the
     * user what it is and where to put it; darktable ignores unknown keys. */
    f = fopen(tmp, "w");
    if (!f) return fail(out, out_size, IR_EFILE, "write", strerror(errno), errno);
    fprintf(f, "{\n");
    fprintf(f, "  \"comment\": \"Generated by indraw for %s %s. "
               "To use in darktable: copy this file to the darktable config dir "
               "(Linux ~/.config/darktable/, replacing or merging noiseprofiles.json), "
               "restart darktable, then enable 'denoise (profiled)' on a raw from this camera. "
               "Values come from the HAL's SENSOR_NOISE_PROFILE per ISO.\",\n",
            g.make, g.model);
    fprintf(f, "  \"version\": 0,\n  \"noiseprofiles\": [\n");
    fprintf(f, "    {\n      \"maker\": \"%s\",\n      \"models\": [\n", g.make);
    fprintf(f, "        {\n          \"model\": \"%s\",\n          \"profiles\": [\n", g.model);
    {
        int first = 1, k;
        for (k = 0; k < n_steps; ++k) {
            if (!rows[k].have) continue;
            fprintf(f, "%s            {\n", first ? "" : ",\n");
            first = 0;
            fprintf(f, "              \"name\": \"%s %s iso %d\",\n", g.make, g.model, rows[k].iso);
            fprintf(f, "              \"iso\": %d,\n", rows[k].iso);
            fprintf(f, "              \"a\": [%.9g, %.9g, %.9g],\n",
                    rows[k].a[0], rows[k].a[1], rows[k].a[2]);
            fprintf(f, "              \"b\": [%.9g, %.9g, %.9g]\n",
                    rows[k].b[0], rows[k].b[1], rows[k].b[2]);
            fprintf(f, "            }");
        }
        fprintf(f, "\n");
    }
    fprintf(f, "          ]\n        }\n      ]\n    }\n  ]\n}\n");
    if (fclose(f) != 0) { unlink(tmp); return fail(out, out_size, IR_EFILE, "write", strerror(errno), errno); }
    if (rename(tmp, path) != 0) { unlink(tmp); return fail(out, out_size, IR_EFILE, "write", strerror(errno), errno); }

    /* Report: the coefficients per ISO, so the user can judge the HAL's model. */
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"path\":");
    jw_str(&w, path);
    jw_add(&w, ",\"maker\":");
    jw_str(&w, g.make);
    jw_add(&w, ",\"model\":");
    jw_str(&w, g.model);
    jw_add(&w, ",\"entries\":%d,\"iso\":[", written);
    {
        int k, first = 1;
        for (k = 0; k < n_steps; ++k)
            if (rows[k].have) { jw_add(&w, "%s%d", first ? "" : ",", rows[k].iso); first = 0; }
    }
    jw_add(&w, "],\"profiles\":[");
    {
        int k, first = 1;
        for (k = 0; k < n_steps; ++k) {
            if (!rows[k].have) continue;
            jw_add(&w, "%s{\"iso\":%d,\"a\":[%.6g,%.6g,%.6g],\"b\":[%.6g,%.6g,%.6g]}",
                   first ? "" : ",", rows[k].iso, rows[k].a[0], rows[k].a[1], rows[k].a[2],
                   rows[k].b[0], rows[k].b[1], rows[k].b[2]);
            first = 0;
        }
    }
    jw_add(&w, "],\"ms\":{\"total\":%.1f}}", now_ms() - t0);
    return IR_OK;
}

/* A burst of n JPEGs (2..8) taken as fast as the sensor allows: ONE capture
 * call with n copies of the still request, so the HAL takes them back to back
 * at its frame rate, then the files are written. Exposure and focus are the
 * same for all: a manual pair as given, or -- in automatic exposure -- the
 * preview's current pair frozen for the burst; the lens stays where the live
 * focus put it (no scan). paths[k] are the n files. The JPEG callback queues
 * the images while burst_on; each is copied out and released at once, so the
 * reader's two buffers keep flowing. Flash: none in a burst. */
static int indraw_burst_impl(const char *const *paths, int n, int quality, int nr, int edge,
                             int64_t exposure_ns, int iso, int timeout_ms, char *out, size_t out_size)
{
    ACaptureRequest *reqs[8];
    uint8_t *buf[8] = { 0 };
    size_t len[8] = { 0 };
    int k, got = 0, err = 0, rc = IR_OK;
    uint8_t u8;
    double t0, t_cap;
    struct timespec dl;
    camera_status_t st;
    jw_t w;
    if (!paths || n < 2 || n > 8 || !out || !out_size || quality < 1 || quality > 100) return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "burst", "camera not open", 0);
    if (exposure_ns <= 0) {                        /* automatic: freeze the live pair */
        pthread_mutex_lock(&g.lock);
        exposure_ns = g.live_exp_ns; iso = g.live_iso_v;
        pthread_mutex_unlock(&g.lock);
        if (exposure_ns <= 0 || iso <= 0)
            return fail(out, out_size, IR_ESTATE, "burst", "no live exposure yet", 0);
    }
    if (set_exposure(g.still_request, exposure_ns, iso, out, out_size)) return IR_EARG;
    u8 = (uint8_t)quality;
    ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_JPEG_QUALITY, 1, &u8);
    {
        int32_t jo = photo_orientation_deg();      /* upright on the ground */
        int use_nr = nr >= 0 ? nr : g.tpl_nr;       /* the shot's own, or the template's */
        int use_edge = edge >= 0 ? edge : g.tpl_edge;
        ACaptureRequest_setEntry_i32(g.still_request, ACAMERA_JPEG_ORIENTATION, 1, &jo);
        if (use_nr >= 0) { u8 = (uint8_t)use_nr; ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_NOISE_REDUCTION_MODE, 1, &u8); }
        if (use_edge >= 0) { u8 = (uint8_t)use_edge; ACaptureRequest_setEntry_u8(g.still_request, ACAMERA_EDGE_MODE, 1, &u8); }
    }
    flash_disarm(g.still_request);                 /* no flash in a burst            */
    pthread_mutex_lock(&g.lock);
    if (g.jpeg_image) { AImage_delete(g.jpeg_image); g.jpeg_image = NULL; }
    for (k = 0; k < g.jpeg_qn; ++k) AImage_delete(g.jpeg_q[k]);
    g.jpeg_qn = 0;
    g.burst_on = 1;
    g.capture_failed = 0;
    g.result_ready = 0;
    g.captured_owed = NULL;                  /* a stack frame / a burst signals by itself */
    pthread_mutex_unlock(&g.lock);
    for (k = 0; k < n; ++k) reqs[k] = g.still_request;
    t0 = now_ms();
    st = ACameraCaptureSession_capture(g.session, &g_still_cbs, n, reqs, NULL);
    if (st != ACAMERA_OK) {
        pthread_mutex_lock(&g.lock); g.burst_on = 0; pthread_mutex_unlock(&g.lock);
        set_exposure(g.still_request, 0, 0, out, out_size);
        return fail(out, out_size, IR_ECAPTURE, "burst", "capture refused", st);
    }
    /* Take the images as they come: copy, release, next. */
    dl = deadline_in(timeout_ms + 500 * n);
    while (got < n) {
        AImage *img = NULL;
        uint8_t *data = NULL;
        int dlen = 0, q;
        pthread_mutex_lock(&g.lock);
        while (g.jpeg_qn == 0 && !g.capture_failed && !g.device_error)
            if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
        if (g.jpeg_qn > 0) {
            img = g.jpeg_q[0];
            for (q = 1; q < g.jpeg_qn; ++q) g.jpeg_q[q - 1] = g.jpeg_q[q];
            g.jpeg_qn--;
        }
        pthread_mutex_unlock(&g.lock);
        if (!img) break;                           /* timeout or failure            */
        if (AImage_getPlaneData(img, 0, &data, &dlen) == AMEDIA_OK && dlen > 4 && data[0] == 0xFF && data[1] == 0xD8) {
            size_t l = (size_t)dlen;
            while (l >= 2 && !(data[l - 2] == 0xFF && data[l - 1] == 0xD9)) --l;   /* trim padding */
            if (l < 2) l = (size_t)dlen;
            buf[got] = malloc(l);
            if (buf[got]) { memcpy(buf[got], data, l); len[got] = l; got++; }
        }
        AImage_delete(img);
    }
    t_cap = now_ms();
    pthread_mutex_lock(&g.lock);
    g.burst_on = 0;
    for (k = 0; k < g.jpeg_qn; ++k) AImage_delete(g.jpeg_q[k]);
    g.jpeg_qn = 0;
    pthread_mutex_unlock(&g.lock);
    set_exposure(g.still_request, 0, 0, out, out_size);   /* the next photo starts in auto */
    if (got > 0) emit_event("captured kind=burst\n");
    /* Write them (Exif with the frozen pair, the same for all). */
    for (k = 0; k < got; ++k) {
        exif_info_t ex;
        char dt[24];
        memset(&ex, 0, sizeof ex);
        exif_datetime(dt, sizeof dt);
        ex.datetime = dt; ex.software = "RAWfish-ir " IR_VERSION;
        ex.wb_manual = g.wb_fixed || g.wb_shift || g.wb_tint;
        ex.exposure_manual = exposure_ns > 0;
        ex.exposure_ns = exposure_ns; ex.iso = iso;
        if (exif_write_jpeg(paths[k], buf[k], len[k], &ex) && (err = write_file(paths[k], buf[k], len[k]))) rc = IR_EFILE;
        else emit_written("jpeg", paths[k]);
        free(buf[k]);
    }
    if (got < n && rc == IR_OK) rc = IR_ECAPTURE;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":%s,\"burst\":%d,\"taken\":%d,\"exposure_ns\":%lld,\"iso\":%d,\"ms\":{\"capture\":%.1f,\"total\":%.1f}%s}",
           got == n && rc == IR_OK ? "true" : "false", n, got, (long long)exposure_ns, iso,
           t_cap - t0, now_ms() - t0, err ? ",\"error\":\"write failed\"" : "");
    return rc;
}

/* The photo entry points, wrapped so the exposure meter is paused (and any
 * in-flight measure frame drained) while a photo owns the HAL / RAW stream. */
IR_API int indraw_burst(const char *const *paths, int n, int quality, int nr, int edge,
                        int64_t exposure_ns, int iso, int timeout_ms, char *out, size_t out_size)
{
    int rc;
    meter_quiesce();
    rc = indraw_burst_impl(paths, n, quality, nr, edge, exposure_ns, iso, timeout_ms, out, out_size);
    meter_release();
    return rc;
}

IR_API int indraw_jpeg(const char *path, int quality, int nr, int edge,
                   int focus_mode, float focus_diopters, int ae_precapture,
                   int64_t exposure_ns, int iso, int variant, int timeout_ms,
                   char *out, size_t out_size)
{
    int rc;
    meter_quiesce();
    rc = indraw_jpeg_impl(path, quality, nr, edge, focus_mode, focus_diopters, ae_precapture, exposure_ns, iso, variant, timeout_ms, out, out_size);
    meter_release();
    return rc;
}

IR_API int indraw_raw(const char *path, const char *datetime, int focus_mode,
                  float focus_diopters, int ae_precapture, int64_t exposure_ns,
                  int iso, int variant, int timeout_ms, char *out, size_t out_size)
{
    int rc;
    meter_quiesce();
    rc = indraw_raw_impl(path, datetime, focus_mode, focus_diopters, ae_precapture, exposure_ns, iso, variant, timeout_ms, out, out_size);
    meter_release();
    return rc;
}

IR_API int indraw_stack(const char *path, const char *datetime, int64_t film_ns, int64_t sub_ns,
                    int stk_denoise, int iso, char *out, size_t out_size)
{
    int rc;
    meter_quiesce();
    rc = indraw_stack_impl(path, datetime, film_ns, sub_ns, stk_denoise, iso, out, out_size);
    meter_release();
    return rc;
}

IR_API int indraw_noiseprofile(const char *path, int64_t sub_ns, char *out, size_t out_size)
{
    int rc;
    meter_quiesce();
    rc = indraw_noiseprofile_impl(path, sub_ns, out, out_size);
    meter_release();
    return rc;
}

/* Stops the preview and releases the camera. */
/* Sets the focus of the running preview WITHOUT taking a photo, so a UI can move
 * the lens live (e.g. the distance wheel on the volume buttons) and show the
 * result in the viewfinder. mode/diopters as in set_focus. Reports the distance
 * the lens actually reached. */
/* Taps a point for metering and/or focus: a small AE/AF region centred on
 * (nx, ny), normalised 0..1 in the SENSOR frame (the UI maps the touch through
 * the same rotation it draws with). do_ae / do_af choose which. On the preview
 * request, so the viewfinder reacts; an AF region also fires one AF scan. The
 * region is a fraction of the active array (6% a side). A tap with do_ae=0,
 * do_af=0 clears both regions (back to full-frame average / the focus button).
 * In manual exposure the UI calls this with do_ae=0 (focus only). */
/* Crop helper, defined with indraw_zoom below. */
static void preview_crop(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t crop[4]);

/* The AF region while zoomed (see af_zoom_on): put on, or taken off, the
 * preview, trigger, photo and RAW requests; the caller then re-sends the
 * repeating request. Only this case: no point tapped for focus, and either
 * zoomed (the middle third of the zoom window) or our region to remove.
 * Nothing changes at zoom 1, in the loupe alone, or after a tap. */
static void photo_crop(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t crop[4]);
static void zoom_af_region(void)
{
    ACaptureRequest *all[4];
    int32_t ax, ay, aw, ah, crop[4], r[5];
    int n = 0, i, on;
    if (g.af_tapped || active_array(&ax, &ay, &aw, &ah, NULL)) return;
    on = g.zoom > 1.01f;
    if (!on && !g.af_zoom_on) return;                 /* never touched: leave the HAL's default */
    all[n++] = g.preview_request;
    all[n++] = g.trigger_request;
    all[n++] = g.still_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (on) {
        photo_crop(ax, ay, aw, ah, crop);             /* the zoom window, [x, y, w, h] */
        r[0] = crop[0] + crop[2] / 3; r[1] = crop[1] + crop[3] / 3;
        r[2] = r[0] + crop[2] / 3;    r[3] = r[1] + crop[3] / 3;   /* [xmin, ymin, xmax, ymax] */
        r[4] = 1000;
    }
    for (i = 0; i < n; ++i) {
        if (!all[i]) continue;
        if (on) ACaptureRequest_setEntry_i32(all[i], ACAMERA_CONTROL_AF_REGIONS, 5, r);
        else    ACaptureRequest_setEntry_i32(all[i], ACAMERA_CONTROL_AF_REGIONS, 0, NULL);   /* removed */
    }
    g.af_zoom_on = on;
}

IR_API int indraw_tap(float nx, float ny, int do_ae, int do_af, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    int32_t ax, ay, aw, ah, region[5], af_region[5], side, af_side;
    int32_t weight = 1000;                        /* MeteringRectangle weight     */
    uint8_t af_mode;
    int continuous = 0;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "tap", "camera not open", 0);
    /* The RAW meter follows the light reference: a tap with ae measures there
     * from now on, "clear" goes back to centre-weighted. */
    pthread_mutex_lock(&g.lock);                    /* the meters read it on their threads */
    g.spot_on = do_ae ? 1 : (do_af ? g.spot_on : 0);
    pthread_mutex_unlock(&g.lock);
    /* A new light reference (or none): the held exposure is freed, the HAL
     * meters again; the UI holds it once more when the AE has converged. */
    if (do_ae || (!do_ae && !do_af)) g.ae_lock = 0;
    if (do_af) { g.af_tapped = 1; g.af_zoom_on = 0; }  /* the tap's region replaces ours */
    else if (!do_ae) g.af_tapped = 0;                  /* cleared: ours again if zoomed */
    if (active_array(&ax, &ay, &aw, &ah, NULL))
        return fail(out, out_size, IR_ESTATE, "tap", "no active array size", 0);
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    {
        /* (nx, ny) is a point of the VIEWFINDER frame, i.e. of the preview crop
         * (zoom and loupe): mapped back to the active array here, in the one
         * place that knows the crop. The region is 6 % of the frame's short
         * side, so in the loupe it is as many times smaller as the loupe
         * magnifies: a precise spot. */
        int32_t pc[4], cx, cy;
        preview_crop(ax, ay, aw, ah, pc);
        cx = pc[0] + (int32_t)(nx * pc[2]);
        cy = pc[1] + (int32_t)(ny * pc[3]);
        side = (pc[2] < pc[3] ? pc[2] : pc[3]) * 6 / 100;
        if (side < 16) side = 16;
        if (do_ae) {                                /* the RAW meter's spot, array-normalised */
            pthread_mutex_lock(&g.lock);
            g.spot_x = (float)(cx - ax) / aw;
            g.spot_y = (float)(cy - ay) / ah;
            g.spot_hw = 0.06f * pc[2] / aw;         /* 12 % of the FRAME: smaller in the loupe */
            g.spot_hh = 0.06f * pc[3] / ah;
            pthread_mutex_unlock(&g.lock);
        }
        region[0] = cx - side / 2; if (region[0] < ax) region[0] = ax;
        region[1] = cy - side / 2; if (region[1] < ay) region[1] = ay;
        region[2] = region[0] + side; if (region[2] > ax + aw) region[2] = ax + aw;
        region[3] = region[1] + side; if (region[3] > ay + ah) region[3] = ay + ah;
        region[4] = weight;
        /* The FOCUS region never shrinks below 5 % of the array's short side:
         * a contrast autofocus needs texture to work on, and in the loupe the
         * exposure spot (above) becomes tiny -- with the same box the JP2
         * locked 2 taps out of 20 in the loupe, nearly every one outside it.
         * The light stays a precise spot; the focus keeps a window it can use. */
        af_side = (aw < ah ? aw : ah) * 5 / 100;
        if (af_side < side) af_side = side;
        af_region[0] = cx - af_side / 2; if (af_region[0] < ax) af_region[0] = ax;
        af_region[1] = cy - af_side / 2; if (af_region[1] < ay) af_region[1] = ay;
        af_region[2] = af_region[0] + af_side; if (af_region[2] > ax + aw) af_region[2] = ax + aw;
        af_region[3] = af_region[1] + af_side; if (af_region[3] > ay + ah) af_region[3] = ay + ah;
        af_region[4] = weight;
    }
    {
        /* The regions go on EVERY request -- the trigger request above all
         * (the HAL scans where the request carrying the trigger says, and a
         * trigger whose AF mode or region differs from the repeating request
         * was ignored on the JP2: only the first tap worked), and the photo
         * requests so the capture does not re-meter elsewhere. */
        int32_t none[5] = { ax, ay, ax + aw, ay + ah, 0 };
        ACaptureRequest *all[6];
        int n = 0, i;
        all[n++] = g.preview_request;
        all[n++] = g.trigger_request;
        all[n++] = g.still_request;
        if (g.raw_request) all[n++] = g.raw_request;
        if (g.meter_request) all[n++] = g.meter_request;
        /* The focus button's mode decides what a tap does: continuous modes
         * keep focusing on the region all the time (no lock, no trigger);
         * the others get one scan on it, then hold (AUTO). Read from the
         * preview request so the tap follows the button. */
        {
            ACameraMetadata_const_entry cur;
            uint8_t m = ACAMERA_CONTROL_AF_MODE_AUTO;
            if (ACaptureRequest_getConstEntry(g.preview_request, ACAMERA_CONTROL_AF_MODE, &cur) == ACAMERA_OK && cur.count)
                m = cur.data.u8[0];
            continuous = (m == ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE || m == ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO);
            af_mode = continuous ? m : ACAMERA_CONTROL_AF_MODE_AUTO;
        }
        for (i = 0; i < n; ++i) {
            uint8_t lk = g.ae_lock ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
            if (!all[i]) continue;
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AE_LOCK, 1, &lk);
            ACaptureRequest_setEntry_i32(all[i], ACAMERA_CONTROL_AE_REGIONS, 5, do_ae ? region : none);
            if (do_af) {
                ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AF_MODE, 1, &af_mode);
                ACaptureRequest_setEntry_i32(all[i], ACAMERA_CONTROL_AF_REGIONS, 5, af_region);
            }
        }
    }
    if (!do_ae && !do_af) zoom_af_region();            /* cleared while zoomed: the middle third */
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "tap", "setRepeatingRequest failed", st);
    /* A new scan: CANCEL first releases a lock from a previous tap (some HALs
     * ignore a START while FOCUSED_LOCKED), then START. No AE precapture: with
     * AE on, the HAL re-meters its region by itself. */
    if (do_af && !continuous) {
        send_trigger(ACAMERA_CONTROL_AF_TRIGGER_CANCEL, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        send_trigger(ACAMERA_CONTROL_AF_TRIGGER_START, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
    }
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"ae\":%s,\"af\":%s,\"continuous\":%s,\"region\":[%d,%d,%d,%d]}",
           do_ae ? "true" : "false", do_af ? "true" : "false", continuous ? "true" : "false",
           region[0], region[1], region[2], region[3]);
    return IR_OK;
}

IR_API int indraw_focus_opt(int mode, float diopters, int wait, char *out, size_t out_size)
{
    jw_t w;
    float reached;
    if (!out || !out_size) return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "focus", "camera not open", 0);
    set_preview_focus_opt(mode, diopters, wait);   /* moves the lens, waits if manual and asked */
    pthread_mutex_lock(&g.lock);
    reached = g.live_focus;                        /* what the lens reports now     */
    pthread_mutex_unlock(&g.lock);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"requested_diopters\":%.3f,\"reached_diopters\":%.3f,"
               "\"lens_state\":%d}", diopters, reached, g.live_lens_state);
    return IR_OK;
}
IR_API int indraw_focus(int mode, float diopters, char *out, size_t out_size)
{
    return indraw_focus_opt(mode, diopters, 1, out, out_size);
}

/* Emits the SF2M "caps" line once streaming starts: the sensor's limits, as
 * the UI needs them to build its wheels (ISO range, analog ceiling, exposure
 * range) and the single-exposure ceiling (observed_shutter_max_ns from the
 * override file when set, else the built-in 1/2 s) above which the UI stacks.
 * ev_step is the HAL's AE compensation step (n/d EV) and ev_min/max its range.
 *   caps iso_min=N iso_max=N iso_analog_max=N exp_min_ns=N exp_max_ns=N
 *        shutter_max_ns=N ev_min=N ev_max=N ev_step=n/d orientation=D
 *        black=N white=N raw=W,H */
static void emit_caps(void)
{
    ACameraMetadata_const_entry e;
    char line[1200];
    long long iso_min = -1, iso_max = -1, iso_analog = -1, exp_min = -1, exp_max = -1, shutter_max;
    int ev_min = 0, ev_max = 0, ev_n = 0, ev_d = 1, black = 64, white = 1023;
    float zoom_max = 1.0f, focal = 0.0f, focal_eq = 0.0f;
    char cams[64] = "", sizes[300] = "", awb[64] = "", scenes[96] = "";
    long long peak_edge, zebra_jpeg, zebra_raw, peak_width;
    int has_flash = 0, crop_free = 0;
    float aperture = 0.0f, min_focus = 0.0f;    /* the closest lens position, HAL diopters */
    if (!g.chars) return;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
        min_focus = e.data.f[0];
    /* 35 mm-equivalent focal length: f x 36 / sensor width (mm). */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) == ACAMERA_OK && e.count)
        focal = e.data.f[0];
    if (focal > 0 && ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &e) == ACAMERA_OK &&
        e.count >= 2 && e.data.f[0] > 0)
        focal_eq = focal * 36.0f / e.data.f[0];
    /* The camera ids the HAL lists, so the UI cycles only through real ones. */
    if (g.manager) {
        ACameraIdList *list = NULL;
        if (ACameraManager_getCameraIdList(g.manager, &list) == ACAMERA_OK && list) {
            int i; size_t n = 0;
            for (i = 0; i < list->numCameras && n < sizeof cams - 8; ++i)
                n += (size_t)snprintf(cams + n, sizeof cams - n, "%s%s", i ? "," : "", list->cameraIds[i]);
            ACameraManager_deleteCameraIdList(list);
        }
    }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &e) == ACAMERA_OK && e.count)
        zoom_max = e.data.f[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count == 2)
        { iso_min = e.data.i32[0]; iso_max = e.data.i32[1]; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_MAX_ANALOG_SENSITIVITY, &e) == ACAMERA_OK && e.count)
        iso_analog = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) == ACAMERA_OK && e.count == 2)
        { exp_min = e.data.i64[0]; exp_max = e.data.i64[1]; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &e) == ACAMERA_OK && e.count == 2)
        { ev_min = e.data.i32[0]; ev_max = e.data.i32[1]; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &e) == ACAMERA_OK && e.count)
        { ev_n = e.data.r[0].numerator; ev_d = e.data.r[0].denominator ? e.data.r[0].denominator : 1; }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count == 4)
        black = (e.data.i32[0] + e.data.i32[1] + e.data.i32[2] + e.data.i32[3]) / 4;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count)
        white = e.data.i32[0];
    shutter_max = g.override_shutter_max_ns > 0 ? g.override_shutter_max_ns : 500000000;
    if (exp_max > 0 && shutter_max > exp_max) shutter_max = exp_max;
    {   /* JPEG sizes "WxH,WxH,..." as listed, and the AWB modes available. */
        ACameraMetadata_const_entry se;
        size_t n = 0; uint32_t i;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &se) == ACAMERA_OK &&
            se.type == ACAMERA_TYPE_INT32)
            for (i = 0; i + 3 < se.count && n < sizeof sizes - 16; i += 4)
                if (se.data.i32[i] == AIMAGE_FORMAT_JPEG && se.data.i32[i + 3] == 0 &&
                    !(g.lim_jw > 0 && (se.data.i32[i + 1] > g.lim_jw || se.data.i32[i + 2] > g.lim_jh)))
                    n += (size_t)snprintf(sizes + n, sizeof sizes - n, "%s%dx%d", n ? "," : "",
                                          se.data.i32[i + 1], se.data.i32[i + 2]);
        n = 0;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AWB_AVAILABLE_MODES, &se) == ACAMERA_OK)
            for (i = 0; i < se.count && n < sizeof awb - 4; ++i)
                n += (size_t)snprintf(awb + n, sizeof awb - n, "%s%d", n ? "," : "", se.data.u8[i]);
        n = 0;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES, &se) == ACAMERA_OK)
            for (i = 0; i < se.count && n < sizeof scenes - 4; ++i)
                n += (size_t)snprintf(scenes + n, sizeof scenes - n, "%s%d", n ? "," : "", se.data.u8[i]);
    }
    /* Preview-aid thresholds: the device override or the built-in defaults. */
    peak_edge = g.override_peak_edge > 0 ? g.override_peak_edge : 40;
    zebra_jpeg = g.override_zebra_jpeg > 0 ? g.override_zebra_jpeg : 250;
    zebra_raw = g.override_zebra_raw > 0 ? g.override_zebra_raw : 992;
    peak_width = g.override_peak_width > 0 ? g.override_peak_width : 2;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_FLASH_INFO_AVAILABLE, &e) == ACAMERA_OK && e.count)
        has_flash = e.data.u8[0] ? 1 : 0;
    /* SCALER_CROPPING_TYPE: FREEFORM honours a crop anywhere; CENTER_ONLY
     * re-centres every crop (only its size counts). The loupe depends on it. */
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_CROPPING_TYPE, &e) == ACAMERA_OK && e.count)
        crop_free = e.data.u8[0] == ACAMERA_SCALER_CROPPING_TYPE_FREEFORM;
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_LENS_INFO_AVAILABLE_APERTURES, &e) == ACAMERA_OK && e.count)
        aperture = e.data.f[0];                 /* phones: one fixed aperture       */
    snprintf(line, sizeof line,
             "caps proto=1 iso_min=%lld iso_max=%lld iso_analog_max=%lld exp_min_ns=%lld exp_max_ns=%lld "
             "shutter_max_ns=%lld ev_min=%d ev_max=%d ev_step=%d/%d orientation=%d black=%d white=%d "
             "raw=%d,%d jpeg=%d,%d zoom_max=%.2f focal_mm=%.2f focal_eq=%.1f "
             "peak_edge=%lld zebra_jpeg=%lld zebra_raw=%lld peak_width=%lld flash=%d crop_free=%d aperture=%.2f min_focus=%.2f notch=%d cameras=%s jpeg_sizes=%s awb_modes=%s scene_modes=%s\n",
             iso_min, iso_max, iso_analog, exp_min, exp_max, shutter_max, ev_min, ev_max, ev_n, ev_d,
             g.sensor_orientation, black, white, g.raw_w, g.raw_h, g.jpeg_w, g.jpeg_h, zoom_max,
             focal, focal_eq, peak_edge, zebra_jpeg, zebra_raw, peak_width, has_flash, crop_free, aperture, min_focus, g_screen_notch, cams, sizes, awb, scenes);
    if (g.lim_raw_off || g.lim_jw > 0) {            /* a limited phone says so (the app shows it) */
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        snprintf(line + n, sizeof line - n, " limits=%s%s%s\n", g.lim_raw_off ? "raw_off" : "",
                 g.lim_raw_off && g.lim_jw > 0 ? "+" : "", g.lim_jw > 0 ? "jpeg_max" : "");
    }
    emit_event(line);
}

/* Starts streaming SF2P preview frames to file descriptor `fd` (the caller's
 * chosen output, e.g. stdout). Frames begin once the preview settles (3A), and
 * flow until indraw_stream_stop, a broken pipe, or close. Returns 0, or an
 * error if the camera is not open. */
IR_API int indraw_stream_start(int fd, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_reader)
        return fail(out, out_size, IR_ESTATE, "stream", "camera not open", 0);
    pthread_mutex_lock(&g.out_lock);                /* stream_fd lives under out_lock */
    g.stream_fd = fd;
    pthread_mutex_unlock(&g.out_lock);
    emit_caps();                                    /* first SF2M line: the limits */
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"streaming\":true,\"size\":[%d,%d],\"format\":\"SF2P_rgb888\"}",
           g.preview_w, g.preview_h);
    return IR_OK;
}

/* Stops streaming (the fd is not closed here; the caller owns it). */
IR_API int indraw_stream_stop(char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    pthread_mutex_lock(&g.out_lock);                /* no writer can hold the old fd after this */
    g.stream_fd = -1;
    pthread_mutex_unlock(&g.out_lock);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"streaming\":false}");
    return IR_OK;
}

/* Sets the session noise-reduction and edge (sharpening) modes live. They apply
 * to the PREVIEW request immediately (the viewfinder changes at once) and become
 * the default for photos, so the viewfinder matches the file (WYSIWYG). This is
 * the lever behind the UI's noise/sharpening menu. The JP2's default is
 * hq_noise/hq_edge, which "repaints" textures (the known detail-loss issue);
 * setting off_noise/off_edge here shows the real sensor detail. nr/edge are the
 * Camera2 enum values, or -1 to leave that one unchanged. Also (re)applies the
 * white balance to the preview so its colour matches the JPEG. */
IR_API int indraw_preview_settings(int nr, int edge, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    uint8_t u8;
    jw_t w;
    camera_status_t st;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "preview", "camera not open", 0);
    if (nr >= 0 && !u8_list_has(g.chars, ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nr))
        return fail(out, out_size, IR_EARG, "preview", "noise mode not listed", nr);
    if (edge >= 0 && !u8_list_has(g.chars, ACAMERA_EDGE_AVAILABLE_EDGE_MODES, edge))
        return fail(out, out_size, IR_EARG, "preview", "edge mode not listed", edge);

    /* The preview's processing only: it is there to judge framing, focus and
     * exposure (a clean, unsharpened view suits that best), while a photo's
     * noise reduction and sharpening come with the photo command. Before,
     * this command set both, and a choice made for the photos (no noise
     * reduction, sharpening on) turned a high-ISO preview into a maze. */
    if (nr >= 0) {
        g.pv_nr = nr;
        u8 = (uint8_t)nr;
        ACaptureRequest_setEntry_u8(g.preview_request, ACAMERA_NOISE_REDUCTION_MODE, 1, &u8);
    }
    if (edge >= 0) {
        g.pv_edge = edge;
        u8 = (uint8_t)edge;
        ACaptureRequest_setEntry_u8(g.preview_request, ACAMERA_EDGE_MODE, 1, &u8);
    }
    /* Ensure the preview uses auto white balance so its colour matches the HAL's
     * JPEG (the raw YUV->RGB without WB looked slightly warm). */
    u8 = g.awb_mode > 0 ? (uint8_t)g.awb_mode : ACAMERA_CONTROL_AWB_MODE_AUTO;
    ACaptureRequest_setEntry_u8(g.preview_request, ACAMERA_CONTROL_AWB_MODE, 1, &u8);

    /* Push the updated request so the change takes effect on the live preview. */
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "preview", "setRepeatingRequest failed", st);

    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"noise\":\"%s\",\"edge\":\"%s\"}",
           g.pv_nr >= 0 ? nr_name(g.pv_nr) : "template",
           g.pv_edge >= 0 ? edge_name(g.pv_edge) : "template");
    return IR_OK;
}

/* Sets the LIVE exposure of the preview (and of the meter frames): exposure_ns
 * and iso > 0 = manual pair (AE off), both 0 = back to the HAL's AE. The UI
 * composes semi-auto from this: it pins one value and sends the pair it
 * computed from the meter, so the viewfinder always shows the real exposure
 * (no display gain). Photos still take their own exp=/iso= arguments; the UI
 * sends the same pair. Reports what was requested. */
IR_API int indraw_preview_exposure(int64_t exposure_ns, int iso, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "exposure", "camera not open", 0);
    if (exposure_ns > 0) {
        int64_t cap = g.override_shutter_max_ns > 0 ? g.override_shutter_max_ns : 500000000;
        if (exposure_ns > cap)                        /* the HAL would lie above it */
            return fail(out, out_size, IR_EARG, "exposure", "above the single-exposure ceiling", 0);
    }
    if (exposure_ns <= 0) g.ae_lock = 0;          /* back to free automatic exposure */
    if (set_exposure(g.preview_request, exposure_ns, iso, out, out_size)) return IR_EARG;
    if (g.meter_request && set_exposure(g.meter_request, exposure_ns, iso, out, out_size)) return IR_EARG;
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "exposure", "setRepeatingRequest failed", st);
    pthread_mutex_lock(&g.lock);
    g.pv_exposure_ns = exposure_ns > 0 ? exposure_ns : 0;
    g.pv_iso = exposure_ns > 0 ? iso : 0;
    pthread_mutex_unlock(&g.lock);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"auto\":%s,\"exposure_ns\":%lld,\"iso\":%d}",
           exposure_ns > 0 ? "false" : "true", (long long)g.pv_exposure_ns, g.pv_iso);
    return IR_OK;
}

/* Sets the AE compensation (in the HAL's steps, see caps ev_step) for the
 * preview, the meter and every automatic photo. Ignored by the HAL when AE
 * is off, which is exactly the manual case (the UI then shows a number). */
IR_API int indraw_ev(int steps, char *out, size_t out_size)
{
    ACameraMetadata_const_entry e;
    ACaptureRequest *reqs[1];
    int32_t ev = steps;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "ev", "camera not open", 0);
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &e) == ACAMERA_OK &&
        e.count == 2 && (steps < e.data.i32[0] || steps > e.data.i32[1]))
        return fail(out, out_size, IR_EARG, "ev", "outside the compensation range", steps);
    pthread_mutex_lock(&g.lock);
    g.ev_steps = steps;
    pthread_mutex_unlock(&g.lock);
    ACaptureRequest_setEntry_i32(g.preview_request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &ev);
    if (g.meter_request)
        ACaptureRequest_setEntry_i32(g.meter_request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &ev);
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "ev", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"ev_steps\":%d}", steps);
    return IR_OK;
}

/* Digital zoom: SCALER_CROP_REGION, a centred window of the active array
 * 1/factor wide and high, on every request (preview, meter, trigger, JPEG,
 * RAW), so the viewfinder and the JPEG zoom together. Factor clamped to
 * [1, SCALER_AVAILABLE_MAX_DIGITAL_ZOOM]. The RAW16 output is NOT cropped by
 * the HAL (it is always the full sensor): a DNG keeps the whole frame -- a
 * DefaultCrop tag for the zoomed window is a later addition. */
/* The active array and the zoom limit, read once per call (they are static). */
static int active_array(int32_t *ax, int32_t *ay, int32_t *aw, int32_t *ah, float *max_zoom)
{
    ACameraMetadata_const_entry e;
    if (max_zoom) {
        *max_zoom = 1.0f;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &e) == ACAMERA_OK && e.count)
            *max_zoom = e.data.f[0];
    }
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) != ACAMERA_OK || e.count < 4)
        return -1;
    /* Crop regions, AE / AF regions and the crops the results report are all
     * in ACTIVE-ARRAY coordinates: (0, 0) is the active array's top-left
     * pixel, whatever its offset in the pixel array (Camera2's
     * SCALER_CROP_REGION). So the offset is 0 here. Adding it (0.47.25 and
     * before) made the full-frame crop overflow by the offset: the JP2's HAL
     * let it pass; the Xperia XA2's (active array at 8,8) raised a fatal
     * device error and the viewfinder froze after the first frames. */
    *ax = 0; *ay = 0; *aw = e.data.i32[2]; *ah = e.data.i32[3];
    return 0;
}

/* The crops, in active-array pixels, even-aligned (keeps the CFA phase).
 * - photo crop: the digital zoom, centred (all requests but the preview);
 * - preview crop: the zoom window, then the focus loupe inside it -- a
 *   window loupe times smaller, centred on (loupe_cx, loupe_cy), kept inside
 *   the zoom window. The loupe is limited so zoom x loupe stays within the
 *   HAL's maximum; beyond that the app magnifies the display.
 * The preview crop is the frame the viewfinder shows, so taps are given in
 * its coordinates (indraw_tap maps them back to the array). */
static void photo_crop(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t crop[4])
{
    float z = g.zoom >= 1.0f ? g.zoom : 1.0f;
    crop[2] = (int32_t)(aw / z) & ~1;
    crop[3] = (int32_t)(ah / z) & ~1;
    crop[0] = ax + ((aw - crop[2]) / 2 & ~1);
    crop[1] = ay + ((ah - crop[3]) / 2 & ~1);
}
static void preview_crop(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t crop[4])
{
    int32_t zc[4];
    float l = g.loupe >= 1.0f ? g.loupe : 1.0f;
    double cx, cy, x0, y0;
    photo_crop(ax, ay, aw, ah, zc);
    if (l <= 1.0f) { memcpy(crop, zc, sizeof zc); return; }
    crop[2] = (int32_t)(zc[2] / l) & ~1;
    crop[3] = (int32_t)(zc[3] / l) & ~1;
    cx = ax + g.loupe_cx * aw;
    cy = ay + g.loupe_cy * ah;
    x0 = cx - crop[2] / 2.0;
    y0 = cy - crop[3] / 2.0;
    if (x0 < zc[0]) x0 = zc[0];
    if (y0 < zc[1]) y0 = zc[1];
    if (x0 > zc[0] + zc[2] - crop[2]) x0 = zc[0] + zc[2] - crop[2];
    if (y0 > zc[1] + zc[3] - crop[3]) y0 = zc[1] + zc[3] - crop[3];
    crop[0] = (int32_t)x0 & ~1;
    crop[1] = (int32_t)y0 & ~1;
}

/* The focus loupe: crops the PREVIEW only (photos, RAW and the meter keep
 * the photo crop), so the viewfinder gets the sensor's real detail at the
 * magnified spot -- up to the HAL's maximum over the zoom; the app enlarges
 * the display beyond that. A preview request change, on a user gesture. */
IR_API int indraw_loupe(float factor, float cx, float cy, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1];
    int32_t ax, ay, aw, ah, crop[4];
    float max_zoom, z;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request) return fail(out, out_size, IR_ESTATE, "loupe", "camera not open", 0);
    if (active_array(&ax, &ay, &aw, &ah, &max_zoom)) return fail(out, out_size, IR_ESTATE, "loupe", "no active array size", 0);
    z = g.zoom >= 1.0f ? g.zoom : 1.0f;
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor * z > max_zoom) factor = max_zoom / z;
    if (factor < 1.0f) factor = 1.0f;
    pthread_mutex_lock(&g.lock);                    /* preview_crop reads these on the callbacks */
    g.loupe = factor;
    g.loupe_cx = cx < 0 ? 0 : cx > 1 ? 1 : cx;
    g.loupe_cy = cy < 0 ? 0 : cy > 1 ? 1 : cy;
    {   /* A CENTER_ONLY HAL re-centres the crop anyway: say so, by using it.
         * (The app then keeps the HAL crop wide enough to contain the panned
         * view and pans in the display.) */
        ACameraMetadata_const_entry ct;
        if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_CROPPING_TYPE, &ct) == ACAMERA_OK && ct.count &&
            ct.data.u8[0] != ACAMERA_SCALER_CROPPING_TYPE_FREEFORM) { g.loupe_cx = 0.5f; g.loupe_cy = 0.5f; }
    }
    pthread_mutex_unlock(&g.lock);
    preview_crop(ax, ay, aw, ah, crop);
    ACaptureRequest_setEntry_i32(g.preview_request, ACAMERA_SCALER_CROP_REGION, 4, crop);
    if (g.meter_request) ACaptureRequest_setEntry_i32(g.meter_request, ACAMERA_SCALER_CROP_REGION, 4, crop);
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK) return fail(out, out_size, IR_ECAPTURE, "loupe", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"loupe\":%.3f,\"crop\":[%d,%d,%d,%d]}", factor, crop[0], crop[1], crop[2], crop[3]);
    return IR_OK;
}

IR_API int indraw_zoom(float factor, char *out, size_t out_size)
{
    ACameraMetadata_const_entry e;
    ACaptureRequest *reqs[1], *all[5];
    int32_t crop[4], ax = 0, ay = 0, aw = 0, ah = 0, n = 0, i;
    float max_zoom = 1.0f;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "zoom", "camera not open", 0);
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &e) == ACAMERA_OK && e.count)
        max_zoom = e.data.f[0];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) != ACAMERA_OK || e.count < 4)
        return fail(out, out_size, IR_ESTATE, "zoom", "no active array size", 0);
    ax = 0; ay = 0; aw = e.data.i32[2]; ah = e.data.i32[3];   /* active-array coordinates (see active_array) */
    if (!(factor >= 1.0f)) factor = 1.0f;
    if (factor > max_zoom) factor = max_zoom;
    pthread_mutex_lock(&g.lock);
    g.zoom = factor;
    if (g.loupe * factor > max_zoom) g.loupe = max_zoom / factor;   /* keep the product legal */
    pthread_mutex_unlock(&g.lock);
    photo_crop(ax, ay, aw, ah, crop);
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    for (i = 0; i < n; ++i)
        if (all[i]) ACaptureRequest_setEntry_i32(all[i], ACAMERA_SCALER_CROP_REGION, 4, crop);
    {   /* the preview: the zoom window, and the loupe inside it if one is open.
         * The meter request takes it too: its RAW output ignores crops, and its
         * preview frame must look like the viewfinder's (it beat otherwise). */
        int32_t pc[4];
        preview_crop(ax, ay, aw, ah, pc);
        ACaptureRequest_setEntry_i32(g.preview_request, ACAMERA_SCALER_CROP_REGION, 4, pc);
        if (g.meter_request) ACaptureRequest_setEntry_i32(g.meter_request, ACAMERA_SCALER_CROP_REGION, 4, pc);
    }
    zoom_af_region();                                 /* zoomed, nothing tapped: focus in the frame */
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "zoom", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"zoom\":%.3f,\"max_zoom\":%.3f,\"crop\":[%d,%d,%d,%d]}",
           factor, max_zoom, crop[0], crop[1], crop[2], crop[3]);
    return IR_OK;
}

/* White balance (JPEG and preview; the RAW stays linear, its as-shot
 * multipliers are only written as metadata): CONTROL_AWB_MODE on every
 * request. mode is the Camera2 value (1 AUTO .. 8 SHADE); OFF is refused
 * because without manual gains it would leave the HAL's last state. */
IR_API int indraw_wb(int mode, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1], *all[5];
    uint8_t u8;
    int n = 0, i;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "wb", "camera not open", 0);
    if (mode <= 0 || !u8_list_has(g.chars, ACAMERA_CONTROL_AWB_AVAILABLE_MODES, mode))
        return fail(out, out_size, IR_EARG, "wb", "white balance mode not listed", mode);
    if (g.wb_fixed) {                              /* a measured white rules: the mode waits */
        g.awb_mode = mode;
        jw_init(&w, out, out_size);
        jw_add(&w, "{\"ok\":true,\"awb_mode\":%d,\"kept\":\"fixed white\"}", mode);
        return IR_OK;
    }
    u8 = (uint8_t)mode;
    all[n++] = g.preview_request;
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (g.meter_request) all[n++] = g.meter_request;
    for (i = 0; i < n; ++i)
        if (all[i]) ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AWB_MODE, 1, &u8);
    g.awb_mode = mode;
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "wb", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"awb_mode\":%d}", mode);
    return IR_OK;
}

/* Manual white balance, as a SHIFT from the HAL's own AWB (JPEG + preview;
 * the RAW stays linear, its as-shot neutral is only metadata): AWB off,
 * colour-correction TRANSFORM_MATRIX with the AWB's last transform and its
 * last gains shifted -- `shift` steps of 5 % on the red/blue ratio (positive
 * = warmer image), `tint` percent of green (negative = greener, positive =
 * magenta). shift 0 and tint 0 = back to AWB, colour correction back to the
 * HAL's own (FAST) so its gains apply again. Applied in the ISP, after the
 * sensor, so preview and JPEG agree and the RAW is untouched. */
static int wb_apply(int shift, int tint, char *out, size_t out_size);
/* The gains of a manual white: `base` (a fixed white, or the AWB's last)
 * shifted by `shift` steps of 5 % on red/blue and `tint` % of
 * green. */
static void wb_shifted(const float base[4], int shift, int tint, float gains[4])
{
    double k = 1.0, t = 1.0 - tint / 100.0;
    int i;
    for (i = 0; i < (shift < 0 ? -shift : shift); ++i) k *= 1.05;
    if (shift < 0) k = 1.0 / k;
    gains[0] = (float)(base[0] * k);
    gains[1] = (float)(base[1] * t);
    gains[2] = (float)(base[2] * t);
    gains[3] = (float)(base[3] / k);
}
IR_API int indraw_wb_manual(int shift, int tint, char *out, size_t out_size)
{
    return wb_apply(shift, tint, out, out_size);
}
/* The manual white balance from its base (the measured white when one is
 * fixed, else the AWB's last gains) and the shift / tint on top; with no
 * base fixed and no shift, back to the HAL's AWB. */
static int wb_apply(int shift, int tint, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1], *all[5];
    float gains[4] = { 1, 1, 1, 1 };
    uint8_t u8, cc;
    int n = 0, i;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "wb", "camera not open", 0);
    all[n++] = g.preview_request;
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (g.meter_request) all[n++] = g.meter_request;
    g.wb_shift = shift; g.wb_tint = tint;
    if (shift == 0 && tint == 0 && !g.wb_fixed) {  /* back to the HAL's AWB     */
        u8 = g.awb_mode > 0 ? (uint8_t)g.awb_mode : ACAMERA_CONTROL_AWB_MODE_AUTO;
        cc = ACAMERA_COLOR_CORRECTION_MODE_FAST;
        for (i = 0; i < n; ++i) {
            if (!all[i]) continue;
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AWB_MODE, 1, &u8);
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_COLOR_CORRECTION_MODE, 1, &cc);
        }
    } else {
        /* 1.05^shift by repeated multiplication (|shift| <= 20): exact for a table of steps. */
        double k = 1.0, t = 1.0 - tint / 100.0;
        float base[4];
        ACameraMetadata_rational tr[9];
        int seen;
        if (shift < -20 || shift > 20 || tint < -100 || tint > 100)
            return fail(out, out_size, IR_EARG, "wb", "shift -20..20, tint -100..100", shift);
        pthread_mutex_lock(&g.lock);                  /* written by the result thread */
        seen = g.awb_seen;
        memcpy(base, g.wb_fixed ? g.wb_fixed_gains : g.awb_gains, sizeof base);
        memcpy(tr, g.awb_transform, sizeof tr);
        pthread_mutex_unlock(&g.lock);
        if (!seen) return fail(out, out_size, IR_ESTATE, "wb", "no AWB result yet", 0);
        (void)k; (void)t;
        wb_shifted(base, shift, tint, gains);         /* R, G even, G odd, B */
        u8 = ACAMERA_CONTROL_AWB_MODE_OFF;
        cc = ACAMERA_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX;
        for (i = 0; i < n; ++i) {
            if (!all[i]) continue;
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AWB_MODE, 1, &u8);
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_COLOR_CORRECTION_MODE, 1, &cc);
            ACaptureRequest_setEntry_rational(all[i], ACAMERA_COLOR_CORRECTION_TRANSFORM, 9, tr);
            ACaptureRequest_setEntry_float(all[i], ACAMERA_COLOR_CORRECTION_GAINS, 4, gains);
        }
    }
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "wb", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"manual\":%s,\"fixed\":%s,\"shift\":%d,\"tint\":%d,\"gains\":[%.3f,%.3f,%.3f,%.3f]}",
           (shift || tint || g.wb_fixed) ? "true" : "false", g.wb_fixed ? "true" : "false", shift, tint, gains[0], gains[1], gains[2], gains[3]);
    return IR_OK;
}

/* Waits (up to 2 s) for the AWB's first result: its colour transform is the
 * one a manual white balance keeps. */
static int wb_wait_awb(void)
{
    int i, seen = 0;
    for (i = 0; i < 40 && !seen; ++i) {
        pthread_mutex_lock(&g.lock);
        seen = g.awb_seen;
        pthread_mutex_unlock(&g.lock);
        if (!seen) usleep(50000);
    }
    return seen;
}

/* A measured white balance, fixed (gains R, Ge, Go, B), or cleared (NULL):
 * the base of the manual white balance from now on; the shift and tint are
 * reset (they start again from the measured white). Restores a pick kept
 * across restarts. */
IR_API int indraw_wb_fixed(const float *gains, char *out, size_t out_size)
{
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request) return fail(out, out_size, IR_ESTATE, "wb", "camera not open", 0);
    if (gains) {
        int k;
        for (k = 0; k < 4; ++k)
            if (!(gains[k] > 0.05f && gains[k] < 20.0f)) return fail(out, out_size, IR_EARG, "wb", "gains out of range", k);
        if (!wb_wait_awb()) return fail(out, out_size, IR_ESTATE, "wb", "no AWB result yet", 0);
        pthread_mutex_lock(&g.lock);
        memcpy(g.wb_fixed_gains, gains, sizeof g.wb_fixed_gains);
        g.wb_fixed = 1;
        pthread_mutex_unlock(&g.lock);
    } else {
        pthread_mutex_lock(&g.lock);
        g.wb_fixed = 0;
        pthread_mutex_unlock(&g.lock);
    }
    return wb_apply(0, 0, out, out_size);
}

/* Measures the white on a neutral object -- the spot of a light reference,
 * else the middle of what is shown -- on one RAW frame, and fixes it (as
 * indraw_wb_fixed). The interface learns the outcome from the event
 * "wbpick ok=1 gains=R,Ge,Go,B" or "wbpick ok=0 why=clipped|dark|timeout|
 * no_raw". The camera's colour shading makes a pick off-centre slightly
 * less exact (the raw corners carry a colour cast the HAL corrects later). */
IR_API int indraw_wb_pick(char *out, size_t out_size)
{
    struct timespec dl = deadline_in(2500);
    int done = 0, status = 0;
    double rgb[3];
    float gains[4];
    char ev[120];
    if (!out || !out_size) return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "wb", "camera not open", 0);
    if (!g.meter_request) { emit_event("wbpick ok=0 why=no_raw\n"); return fail(out, out_size, IR_ESTATE, "wb", "no RAW stream", 0); }
    pthread_mutex_lock(&g.lock);
    g.wbpick_done = 0;
    g.wbpick_pending = 1;
    pthread_cond_broadcast(&g.cond);                /* wake the meter thread */
    while (!g.wbpick_done)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    done = g.wbpick_done; status = g.wbpick_status;
    memcpy(rgb, g.wbpick_rgb, sizeof rgb);
    g.wbpick_pending = 0;
    pthread_mutex_unlock(&g.lock);
    if (!done || status) {
        snprintf(ev, sizeof ev, "wbpick ok=0 why=%s\n", !done ? "timeout" : status == 1 ? "clipped" : "dark");
        emit_event(ev);
        return fail(out, out_size, IR_ECAPTURE, "wb", !done ? "no measure" : status == 1 ? "clipped" : "too dark", status);
    }
    /* the gains that make this camera RGB neutral, green at 1 */
    gains[0] = (float)(rgb[1] / rgb[0]); gains[1] = 1.0f; gains[2] = 1.0f; gains[3] = (float)(rgb[1] / rgb[2]);
    if (indraw_wb_fixed(gains, out, out_size) != IR_OK) {
        emit_event("wbpick ok=0 why=apply\n");
        return IR_ECAPTURE;
    }
    snprintf(ev, sizeof ev, "wbpick ok=1 gains=%.4f,%.4f,%.4f,%.4f\n", gains[0], gains[1], gains[2], gains[3]);
    emit_event(ev);
    return IR_OK;
}

/* Scene mode: CONTROL_MODE USE_SCENE_MODE + CONTROL_SCENE_MODE on every
 * request (mode > 0), or back to CONTROL_MODE AUTO (mode 0). A scene is a
 * grouped control: the HAL then drives AE / AWB / AF itself, so the UI greys
 * the atomic controls while one is on (the engine still accepts them, the
 * HAL ignores most). The mode is the Camera2 value (caps: scene_modes). */
IR_API int indraw_scene(int mode, char *out, size_t out_size)
{
    ACaptureRequest *reqs[1], *all[5];
    uint8_t ctrl, scene;
    int n = 0, i;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "scene", "camera not open", 0);
    if (mode > 0 && !u8_list_has(g.chars, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES, mode))
        return fail(out, out_size, IR_EARG, "scene", "scene mode not listed", mode);
    ctrl = mode > 0 ? ACAMERA_CONTROL_MODE_USE_SCENE_MODE : ACAMERA_CONTROL_MODE_AUTO;
    scene = mode > 0 ? (uint8_t)mode : ACAMERA_CONTROL_SCENE_MODE_DISABLED;
    all[n++] = g.preview_request;
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (g.meter_request) all[n++] = g.meter_request;
    for (i = 0; i < n; ++i) {
        if (!all[i]) continue;
        ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_MODE, 1, &ctrl);
        ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_SCENE_MODE, 1, &scene);
    }
    g.scene_mode = mode > 0 ? mode : 0;               /* set_variant keeps it on captures */
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "scene", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"scene_mode\":%d}", mode);
    return IR_OK;
}

/* How long a photo waits for the AF scan / AE precapture it asked for, in
 * ms (the "focus delay" of the menu); a timeout is not fatal, the shot is
 * taken as is and reported "timeout". 0 = the default 2000. */
IR_API int indraw_set_af_wait(int ms, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    g.af_wait_ms = ms > 0 ? ms : 0;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"af_wait_ms\":%d}", g.af_wait_ms > 0 ? g.af_wait_ms : 1500);
    return IR_OK;
}

/* The lens position the HAL last reported (LENS_FOCUS_DISTANCE of the latest
 * preview result, in the HAL's own "diopters"), for the focus calibration
 * assistant: after the user focused on a target at a known distance, this is
 * the value that distance maps to. Negative when unknown. */
IR_API float indraw_lens_position(void)
{
    float v;
    pthread_mutex_lock(&g.lock);
    v = g.live_focus;
    pthread_mutex_unlock(&g.lock);
    return v;
}

/* Writes the AE lock state on every request (automatic exposure only reads
 * it; a manual pair ignores it) and restarts the preview with it. */
static int apply_ae_lock(int on)
{
    ACaptureRequest *reqs[1], *all[5];
    uint8_t lk = on ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
    int n = 0, i;
    g.ae_lock = on;
    all[n++] = g.preview_request;
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (g.meter_request) all[n++] = g.meter_request;
    for (i = 0; i < n; ++i) if (all[i]) ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AE_LOCK, 1, &lk);
    reqs[0] = g.preview_request;
    return ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL) == ACAMERA_OK ? 0 : -1;
}

/* Holds (on) or frees (off) the automatic exposure. The UI holds it once the
 * HAL's AE has converged on a tapped point; a new tap, "tap ... clear" and
 * "exposure auto" free it. One request change per user action, none in
 * between: the preview's flow and the focus are never disturbed. */
IR_API int indraw_ae_lock(int on, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request) return fail(out, out_size, IR_ESTATE, "aelock", "camera not open", 0);
    if (apply_ae_lock(on ? 1 : 0)) return fail(out, out_size, IR_ECAPTURE, "aelock", "setRepeatingRequest failed", 0);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"ae_lock\":%s}", on ? "true" : "false");
    return IR_OK;
}

/* The orientation a photo must carry so it shows upright ON THE GROUND,
 * whatever the OS does with its screen: the sensor's mounting plus the
 * phone's physical rotation at the shot (gravity, read by the interface's
 * orientation sensor), the other way round for a front camera -- the
 * Camera2 rule for JPEG_ORIENTATION. In degrees, clockwise. */
static int photo_orientation_deg(void)
{
    int d = g.lens_front ? (g.sensor_orientation - g.device_rot + 360) % 360
                         : (g.sensor_orientation + g.device_rot) % 360;
    return d;
}
/* The same as a TIFF/DNG Orientation code (1 upright, 6 = 90 CW, 3 = 180, 8 = 270 CW). */
static int photo_orientation_tiff(void)
{
    switch (photo_orientation_deg()) {
    case 90:  return 6;
    case 180: return 3;
    case 270: return 8;
    default:  return 1;
    }
}

/* The window the interface shows, normalised in the active array (the
 * controller's display window): what the histograms cover -- "what you see
 * is what is measured". The HAL's own frame can be wider (a centre-only HAL
 * crops little away from the centre; a gesture keeps it at the zoom window). */
IR_API int indraw_view_window(double x, double y, double w, double h, char *out, size_t out_size)
{
    jw_t w_;
    if (!out || !out_size) return IR_EARG;
    if (!(w > 0) || !(h > 0) || x < 0 || y < 0 || x + w > 1.0001 || y + h > 1.0001)
        return fail(out, out_size, IR_EARG, "viewwin", "x y w h in 0..1", 0);
    pthread_mutex_lock(&g.lock);
    g.view_win[0] = x; g.view_win[1] = y; g.view_win[2] = w; g.view_win[3] = h;
    g.have_view_win = !(x <= 0 && y <= 0 && w >= 0.9999 && h >= 0.9999);
    pthread_mutex_unlock(&g.lock);
    jw_init(&w_, out, out_size);
    jw_add(&w_, "{\"ok\":true,\"view\":[%.4f,%.4f,%.4f,%.4f]}", x, y, w, h);
    return IR_OK;
}

/* The phone's physical rotation, from the interface (0, 90, 180, 270
 * clockwise from its natural portrait). Kept until the next; read at each
 * shot. Independent of the screen's orientation lock. */
IR_API int indraw_orient(int degrees, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270)
        return fail(out, out_size, IR_EARG, "orient", "0, 90, 180 or 270", degrees);
    g.device_rot = degrees;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"device_rot\":%d,\"photo_orientation\":%d}", degrees, photo_orientation_deg());
    return IR_OK;
}

/* Flash: off, auto, on (fires with the photo; in automatic exposure through
 * the AE mode and a precapture with pre-flash, in manual as a single flash),
 * or torch (continuous light, on the preview and the photos). Refused when
 * the camera has no flash unit (FLASH_INFO_AVAILABLE). */
/* A vendor (chipset) request key, from the device profile's "vendor_request"
 * list: tag id (0x8000xxxx), type (0 u8, 1 i32, 2 float, 3 i64), value, and
 * which requests take it (on: 0 preview only, 1 photo requests only, 2 all).
 * The engine never interprets such keys (they belong to one chipset, hence
 * to override-hal.json); it applies what the file says and reports whether
 * the NDK accepted it (a key the camera service has not published is
 * refused by the NDK). Applied at open, kept on the requests after. */
/* --- The measured lens-shading calibration -------------------------------
 * File name (so that users of the same phone can exchange it):
 *   shading_<manufacturer>-<model>_cam<ID>_<W>x<H>.json
 * from ro.product.manufacturer / ro.product.model (lowercase, anything but
 * letters and digits as '-'), the camera id and the raw size; looked for in
 * the user's dir, then in the one shipped with indRAW. Checked against the
 * sensor (CFA, size) before use. mode: 1 measured (if found), 0 HAL, 2 none. */
static void slug_into(const char *src, char *out, size_t n)
{
    size_t k = 0;
    for (; *src && k + 1 < n; ++src) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[k++] = c;
        else if (k > 0 && out[k - 1] != '-') out[k++] = '-';
    }
    while (k > 0 && out[k - 1] == '-') --k;
    out[k] = '\0';
}
/* "cal_done ok=1 path=..." / "cal_done ok=0 why=...": the calibration's
 * outcome as an event, so the interface's assistant (which does not see
 * command replies) learns it, like wbpick. Paths carry no spaces. */
static void cal_done_event(int ok, const char *path, const char *why)
{
    char ev[800], w[200];
    int i;
    if (ok) { snprintf(ev, sizeof ev, "cal_done ok=1 path=%s\n", path ? path : ""); emit_event(ev); return; }
    /* the event line is split on spaces: spaces in the reason become '_' */
    snprintf(w, sizeof w, "%s", why ? why : "");
    for (i = 0; w[i]; ++i) if (w[i] == ' ') w[i] = '_';
    snprintf(ev, sizeof ev, "cal_done ok=0 why=%s\n", w);
    emit_event(ev);
}

/* Reads a just-written flat-field DNG back into a capture grid. The file is
 * ours (indraw_raw wrote it a moment ago), so the path is trusted. */
static int cal_read_back(const char *path, int rows, int cols, double hal_d, double cct,
                         shading_capture_t *cap, char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *bytes;
    int rc;
    if (!f) { snprintf(err, errn, "cannot reopen the capture"); return -1; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || !(bytes = malloc((size_t)len))) { fclose(f); snprintf(err, errn, "capture unreadable"); return -1; }
    if (fread(bytes, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(bytes); snprintf(err, errn, "short read"); return -1; }
    fclose(f);
    rc = shading_read_dng(bytes, (size_t)len, rows, cols, hal_d, cct, cap, err, errn);
    free(bytes);
    return rc;
}

static double target_dn(void) { return 500.0; }   /* centre target on a 10-bit sensor */
/* Finds an exposure (returns ns and iso) giving a centre near `target` DN
 * on a flat field, not clipped: starts from the lowest ISO and a short time,
 * takes measure-only RAW frames and scales the exposure toward the target,
 * halving if it clips. Up to 6 tries. 0 on success. */
static int cal_find_exposure(int64_t *ns_out, int *iso_out, char *err, size_t errn)
{
    ACameraMetadata_const_entry e;
    int iso_min = 100, iso_max = 3200, iso = 400, tries;
    int64_t ns = 30000000, ns_max = 500000000;   /* start 1/33 s, ceiling 1/2 s */
    char out[1024];
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count >= 2)
        { iso_min = e.data.i32[0]; iso_max = e.data.i32[1]; }
    if (iso < iso_min) iso = iso_min;
    if (iso > iso_max) iso = iso_max;
    /* Walk exposure (time x iso) toward a centre near target_dn(), scaling by
     * the measured ratio each time -- time first up to 1/2 s, then iso up to
     * the maximum. Halve if it clips. Flat fields through a diffuser can be
     * dim, so this must reach far, not cap at x4 from the lowest ISO (0.47.40:
     * the first version started at iso_min, 1/50 s and never reached 500 DN). */
    for (tries = 0; tries < 10; ++tries) {
        double centre, clip, f;
        if (indraw_raw("-", "", IR_FOCUS_RAW, 0.0f, 0, ns, iso, 0, 15000, out, sizeof out) != IR_OK) {
            snprintf(err, errn, "metering failed"); return -1;
        }
        {
            const char *cm = strstr(out, "\"center_mean\":"), *cp = strstr(out, "\"center_clip_pct\":");
            centre = cm ? atof(cm + 14) : 0;
            clip = cp ? atof(cp + 18) : 0;
        }
        if (clip > 1.0) {                        /* too bright: less light */
            if (iso > iso_min) iso = iso / 2 < iso_min ? iso_min : iso / 2;
            else if (ns > 2000000) ns /= 2;
            else { snprintf(err, errn, "cannot avoid clipping"); return -1; }
            continue;
        }
        if (centre > target_dn() * 0.8 && centre < target_dn() * 1.2) { *ns_out = ns; *iso_out = iso; return 0; }
        f = centre > 1 ? target_dn() / centre : 8.0;  /* how much more light we need */
        if (f > 1.0) {                           /* brighter: time first, then iso */
            if (ns < ns_max) {
                ns = (int64_t)(ns * f);
                if (ns > ns_max) { iso = (int)(iso * (double)ns / ns_max); ns = ns_max; }
            } else {
                iso = (int)(iso * f);
            }
            if (iso > iso_max) iso = iso_max;
        } else {                                 /* darker: iso first, then time */
            if (iso > iso_min) { iso = (int)(iso * f); if (iso < iso_min) iso = iso_min; }
            else ns = (int64_t)(ns * f);
        }
        if (ns < 1000000) ns = 1000000;
    }
    snprintf(err, errn, "could not settle the exposure (too dark even at the maximum)");
    return -1;
}

/* Builds (or extends) a lens-shading calibration by capturing flat fields.
 * The caller gives the colour temperature of the light in front of the lens
 * (cct) and whether to measure the LUMINANCE too (full: the lens positions)
 * or only the COLOUR at this light (add a light to an existing file, one
 * position). lens_values: the HAL lens values to walk (full), ignored when
 * not full. shots: frames per position (averaged). The DNGs are written to
 * a temporary dir and removed. On success the file in `dir` is written (or
 * merged) and its path reported; progress is emitted as
 * "cal_progress done=N total=M". A frame is refused if clipped, too dark, or
 * its centre level strays from the first frame of its lens position by more
 * than 5 %, or a position's first frame from the previous position's by more
 * than 15 % (the light or the framing moved; shading_level_check). */
IR_API int indraw_shading_calibrate(const char *dir, int full, double cct,
                                    const float *lens_values, int nlens, int shots,
                                    char *out, size_t out_size)
{
    shading_capture_t caps[SHADING_MAX_POINTS * 4];
    shading_cal_t built, existing;
    char tmpl[] = "/tmp/indraw-cal-XXXXXX", rawpath[320], name[200], path[600], err[128] = "", mk[64], md[64];
    char dt[24] = "";
    int ncap = 0, i, k, total, done = 0, rc = IR_OK, have_existing = 0, rows = 12, cols = 16;
    double pos_centre = -1;     /* centre level of the first frame at this lens position */
    double prev_centre = -1;    /* ... and at the previous position */
    const int R = 12, C = 16;
    int64_t fix_ns = 0;
    int fix_iso = 0;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.chars) return fail(out, out_size, IR_ESTATE, "cal", "camera not open", 0);
    if (shots < 1) shots = 3;
    if (shots > 4) shots = 4;
    /* Fix the exposure once, for a flat field with the centre mid-range and
     * not clipped; every capture then uses this same pair (the AE drifting
     * between frames ruined the first attempt). */
    if (cal_find_exposure(&fix_ns, &fix_iso, err, sizeof err) != 0) {
        cal_done_event(0, NULL, err);
        return fail(out, out_size, IR_ESTATE, "cal", err, 0);
    }
    {
        char ev[96];
        snprintf(ev, sizeof ev, "cal_exposure exp_ns=%lld iso=%d\n", (long long)fix_ns, fix_iso);
        emit_event(ev);
    }
    if (!mkdtemp(tmpl)) return fail(out, out_size, IR_EFILE, "cal", "no temp dir", 0);
    if (full) {
        if (nlens < 1 || nlens > SHADING_MAX_POINTS) { rmdir(tmpl); return fail(out, out_size, IR_EARG, "cal", "lens positions out of range", nlens); }
    } else {
        nlens = 1;
    }
    total = nlens * shots;
    for (i = 0; i < nlens && rc == IR_OK; ++i) {
        float lv = full ? lens_values[i] : 0.0f;    /* add-a-light: infinity */
        if (pos_centre > 0) prev_centre = pos_centre;   /* the position just done */
        pos_centre = -1;                            /* each position is its own reference */
        for (k = 0; k < shots; ++k) {
            char rout[1024];
            snprintf(rawpath, sizeof rawpath, "%s/p%d_%d.dng", tmpl, i, k);
            /* raw capture, lens at the HAL value lv (IR_FOCUS_RAW = 8), AE off
             * if a manual pair is in force, else auto; let the engine pick. */
            if (indraw_raw(rawpath, dt, IR_FOCUS_RAW, lv, 0, fix_ns, fix_iso, 0, 15000, rout, sizeof rout) != IR_OK) {
                snprintf(err, sizeof err, "capture failed at position %d", i); rc = IR_ECAPTURE; break;
            }
            if (cal_read_back(rawpath, R, C, g.live_focus, cct, &caps[ncap], err, sizeof err) != 0) { rc = IR_EFILE; unlink(rawpath); break; }
            unlink(rawpath);
            /* guards: clipping, darkness, a wandering centre level */
            if (caps[ncap].clip_fraction > 0.02) { snprintf(err, sizeof err, "clipped: lower the exposure"); shading_capture_free(&caps[ncap]); rc = IR_EARG; break; }
            if (caps[ncap].centre_level < 40) { snprintf(err, sizeof err, "too dark: raise the exposure"); shading_capture_free(&caps[ncap]); rc = IR_EARG; break; }
            /* the corners must carry real signal, or the field is too dark at
             * the edges (diffuser not covering, or too strong a falloff): a
             * corner below 8 % of the centre is not measurable. */
            if (caps[ncap].corner_min_frac < 0.08) { snprintf(err, sizeof err, "the corners are too dark to measure (cover the whole frame, more light)"); shading_capture_free(&caps[ncap]); rc = IR_EARG; break; }
            /* Light/framing check (shading_level_check, host-tested): a frame
             * against the first of its own position (5 %), the first frame of
             * a position against the first of the previous one (15 %: the
             * falloff moves the centre a few % per step). 0.47.47 compared
             * within a position only, at 20 %; 0.47.46 against the very first
             * frame, which stopped a run at the closest position. */
            if (shading_level_check(caps[ncap].centre_level, pos_centre, prev_centre, err, sizeof err)) {
                shading_capture_free(&caps[ncap]); rc = IR_ESTATE; break;
            }
            if (pos_centre < 0) pos_centre = caps[ncap].centre_level;
            ++ncap; ++done;
            {
                char ev[64];
                snprintf(ev, sizeof ev, "cal_progress done=%d total=%d\n", done, total);
                emit_event(ev);
            }
        }
    }
    if (rc != IR_OK) {
        for (i = 0; i < ncap; ++i) shading_capture_free(&caps[i]);
        rmdir(tmpl);
        cal_done_event(0, NULL, err[0] ? err : "capture");
        return fail(out, out_size, rc, "cal", err[0] ? err : "capture", done);
    }
    /* the file name and path */
    slug_into(g.make, mk, sizeof mk); slug_into(g.model, md, sizeof md);
    snprintf(name, sizeof name, "shading_%s-%s_cam%s_%dx%d.json", mk, md, g.camera_id, g.raw_w, g.raw_h);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    /* A full calibration never overwrites a previous one: an existing file is
     * moved into previous/ with a timestamp. (Adding a light edits in place:
     * it is meant to extend the current file.) */
    if (full) {
        FILE *ef = fopen(path, "rb");
        if (ef) {
            char prevdir[700], prevpath[900], stamp[24];
            time_t t = time(NULL);
            struct tm tm;
            fclose(ef);
            snprintf(prevdir, sizeof prevdir, "%s/previous", dir);
            mkdir(prevdir, 0755);
            localtime_r(&t, &tm);
            strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
            snprintf(prevpath, sizeof prevpath, "%s/shading_%s-%s_cam%s_%dx%d_%s.json",
                     prevdir, mk, md, g.camera_id, g.raw_w, g.raw_h, stamp);
            rename(path, prevpath);              /* best effort */
        }
    }
    /* add-a-light: merge into the existing file if present */
    if (!full) {
        FILE *ef = fopen(path, "rb");
        if (ef) {
            long len; char *text;
            fseek(ef, 0, SEEK_END); len = ftell(ef); fseek(ef, 0, SEEK_SET);
            if (len > 0 && (text = malloc((size_t)len + 1))) {
                if (fread(text, 1, (size_t)len, ef) == (size_t)len) { text[len] = '\0'; if (shading_parse(text, &existing, err, sizeof err) == 0) have_existing = 1; }
                free(text);
            }
            fclose(ef);
        }
        if (!have_existing) {
            for (i = 0; i < ncap; ++i) shading_capture_free(&caps[i]);
            rmdir(tmpl);
            cal_done_event(0, NULL, "no existing calibration to add a light to");
            return fail(out, out_size, IR_ESTATE, "cal", "no existing calibration to add a light to", 0);
        }
    }
    if (shading_build_assemble(caps, ncap, &built, err, sizeof err) != 0) {
        for (i = 0; i < ncap; ++i) shading_capture_free(&caps[i]);
        if (have_existing) shading_free(&existing);
        rmdir(tmpl);
        cal_done_event(0, NULL, err);
        return fail(out, out_size, IR_EARG, "cal", err, 0);
    }
    rows = built.rows; cols = built.cols;
    if (have_existing) {
        /* keep the existing luminance; add this light's colour (replace a
         * light within 50 K). */
        int li, ci = -1;
        for (li = 0; li < existing.ncol; ++li) if (fabs(existing.col_k[li] - built.col_k[0]) < 50) ci = li;
        if (ci < 0 && existing.ncol < SHADING_MAX_POINTS) {
            ci = existing.ncol++;
            existing.col_r[ci] = malloc(sizeof(float) * (size_t)(rows * cols));
            existing.col_g[ci] = malloc(sizeof(float) * (size_t)(rows * cols));
            existing.col_b[ci] = malloc(sizeof(float) * (size_t)(rows * cols));
        }
        if (ci >= 0 && existing.col_r[ci] && existing.col_g[ci] && existing.col_b[ci]) {
            existing.col_k[ci] = built.col_k[0];
            memcpy(existing.col_r[ci], built.col_r[0], sizeof(float) * (size_t)(rows * cols));
            memcpy(existing.col_g[ci], built.col_g[0], sizeof(float) * (size_t)(rows * cols));
            memcpy(existing.col_b[ci], built.col_b[0], sizeof(float) * (size_t)(rows * cols));
        }
        rc = shading_write_file(path, &existing, g.make, g.model, g.camera_id) > 0 ? IR_OK : IR_EFILE;
        shading_free(&existing);
    } else {
        rc = shading_write_file(path, &built, g.make, g.model, g.camera_id) > 0 ? IR_OK : IR_EFILE;
    }
    for (i = 0; i < ncap; ++i) shading_capture_free(&caps[i]);
    shading_free(&built);
    rmdir(tmpl);
    if (rc != IR_OK) { cal_done_event(0, NULL, "could not write the file"); return fail(out, out_size, IR_EFILE, "cal", "could not write the file", 0); }
    cal_done_event(1, path, NULL);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"%s\":true,\"captures\":%d,\"path\":", full ? "full" : "added_light", ncap);
    jw_str(&w, path);
    jw_add(&w, "}");
    return IR_OK;
}

/* The calibration file name a device profile gives (one name for every
 * variant of a camera module: the XA2 single and dual SIM report different
 * models), tried after the name computed from this phone's make and model. */
static char g_shade_profile_name[200];
IR_API int indraw_shading_name(const char *name)
{
    snprintf(g_shade_profile_name, sizeof g_shade_profile_name, "%s", name ? name : "");
    return IR_OK;
}

IR_API int indraw_shading_load(const char *user_dir, const char *sys_dir, int mode, char *out, size_t out_size)
{
    static const char *const cfa_names[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
    char mk[64], md[64], name[200], path[600], err[128] = "";
    const char *dirs[4], *names[4];
    ACameraMetadata_const_entry e;
    int d, cfa = 3, nd = 0;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.chars) return fail(out, out_size, IR_ESTATE, "shading", "camera not open", 0);
    shading_free(&g.shade);
    free(g.shade_buf); g.shade_buf = NULL;
    g.shade_have = 0; g.shade_path[0] = '\0';
    g.shade_mode = mode < 0 || mode > 2 ? 1 : mode;
    jw_init(&w, out, out_size);
    if (g.shade_mode != 1) {
        jw_add(&w, "{\"ok\":true,\"shading\":\"%s\"}", g.shade_mode == 2 ? "none" : "hal");
        return IR_OK;
    }
    slug_into(g.make, mk, sizeof mk);
    slug_into(g.model, md, sizeof md);
    snprintf(name, sizeof name, "shading_%s-%s_cam%s_%dx%d.json", mk, md, g.camera_id, g.raw_w, g.raw_h);
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count)
        cfa = e.data.u8[0] & 3;
    /* in order: the user's own (computed name: what the assistant writes),
     * the user's under the profile's name, the shipped one under the
     * profile's name, the shipped one under the computed name */
    dirs[nd] = user_dir; names[nd++] = name;
    if (g_shade_profile_name[0]) { dirs[nd] = user_dir; names[nd++] = g_shade_profile_name; }
    if (g_shade_profile_name[0]) { dirs[nd] = sys_dir; names[nd++] = g_shade_profile_name; }
    dirs[nd] = sys_dir; names[nd++] = name;
    for (d = 0; d < nd && !g.shade_have; ++d) {
        FILE *f;
        long len;
        char *text;
        if (!dirs[d] || !dirs[d][0]) continue;
        snprintf(path, sizeof path, "%s/%s", dirs[d], names[d]);
        if (!(f = fopen(path, "rb"))) continue;
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > 4000000 || !(text = malloc((size_t)len + 1))) { fclose(f); snprintf(err, sizeof err, "unreadable"); continue; }
        if (fread(text, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(text); snprintf(err, sizeof err, "short read"); continue; }
        fclose(f);
        text[len] = '\0';
        if (shading_parse(text, &g.shade, err, sizeof err) == 0) {
            if (strcmp(g.shade.cfa, cfa_names[cfa]) || g.shade.width != g.raw_w || g.shade.height != g.raw_h) {
                snprintf(err, sizeof err, "not this sensor (cfa %s, %dx%d)", g.shade.cfa, g.shade.width, g.shade.height);
                shading_free(&g.shade);
            } else if (!(g.shade_buf = malloc(sizeof(float) * 4 * (size_t)(g.shade.rows * g.shade.cols)))) {
                snprintf(err, sizeof err, "no memory");
                shading_free(&g.shade);
            } else {
                g.shade_have = 1;
                snprintf(g.shade_path, sizeof g.shade_path, "%s", path);
            }
        }
        free(text);
    }
    if (!g.shade_have) {
        jw_add(&w, "{\"ok\":true,\"shading\":\"hal\",\"measured\":false,\"looked_for\":");
        jw_str(&w, name);
        if (err[0]) { jw_add(&w, ",\"refused\":"); jw_str(&w, err); }
        jw_add(&w, "}");
        return IR_OK;
    }
    jw_add(&w, "{\"ok\":true,\"shading\":\"measured\",\"lens_positions\":%d,\"lights\":%d,\"path\":",
           g.shade.nlum, g.shade.ncol);
    jw_str(&w, g.shade_path);
    jw_add(&w, "}");
    return IR_OK;
}
/* The user's dosing of the measured map (pivot and strength 0..100,
 * maximum gain >= 1); see shading.h. */
IR_API int indraw_shading_params(double balance, double strength, double max_gain, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    g.shade_balance = balance < 0 ? 0 : balance > 100 ? 100 : balance;
    g.shade_strength = strength < 0 ? 0 : strength > 100 ? 100 : strength;
    g.shade_max_gain = max_gain < 1 ? 1 : max_gain > 16 ? 16 : max_gain;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"balance\":%.0f,\"strength\":%.0f,\"max_gain\":%.2f}",
           g.shade_balance, g.shade_strength, g.shade_max_gain);
    return IR_OK;
}

/* What the preview results say of the vendor keys set so far: "tag=value"
 * pairs ("absent" when the result does not carry the key). */
IR_API int indraw_vendor_report(char *out, size_t out_size)
{
    jw_t w;
    int i;
    if (!out || !out_size) return IR_EARG;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"in_results\":{");
    pthread_mutex_lock(&g.lock);
    for (i = 0; i < g.vw_n; ++i)
        jw_add(&w, "%s\"0x%08x\":\"%s\"", i ? "," : "", g.vw_tag[i], g.vw_value[i][0] ? g.vw_value[i] : "no result yet");
    pthread_mutex_unlock(&g.lock);
    jw_add(&w, "}}");
    return IR_OK;
}

/* The phone's make and model as Android declares them, before any camera is
 * open: what a shipped device profile is matched against. */
IR_API int indraw_device_info(char *out, size_t out_size)
{
    char mk[92] = "", md[92] = "";
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    __system_property_get("ro.product.manufacturer", mk);
    __system_property_get("ro.product.model", md);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"make\":");
    jw_str(&w, mk);
    jw_add(&w, ",\"model\":");
    jw_str(&w, md);
    jw_add(&w, "}");
    return IR_OK;
}

/* Whether the screen has a cutout at the top, from the device profile
 * (-1 unknown: the interface then guesses from the screen's proportions);
 * passed to the interface in the caps line. */
IR_API int indraw_set_screen_notch(int notch)
{
    g_screen_notch = notch < 0 ? -1 : notch ? 1 : 0;
    return IR_OK;
}

/* Device profile, before indraw_open: a session parameter (tag, type as
 * indraw_vendor_set: 0 u8, 1 i32, 2 float, 3 i64, 4 double; value). Up to 8;
 * a tag given twice keeps the last value. */
IR_API int indraw_set_session_request(uint32_t tag, int type, double value)
{
    int i;
    for (i = 0; i < g.n_session_req; ++i) if (g.session_req[i].tag == tag) break;
    if (i == g.n_session_req) { if (g.n_session_req >= 8) return IR_EARG; g.n_session_req++; }
    g.session_req[i].tag = tag; g.session_req[i].type = type; g.session_req[i].value = value;
    return IR_OK;
}
/* Sets the session parameters on a request (the one handed to the HAL at
 * session creation). Returns the number set. */
static int apply_session_request(ACaptureRequest *req)
{
    int i, n = 0;
    for (i = 0; i < g.n_session_req; ++i) {
        uint8_t u8 = (uint8_t)g.session_req[i].value; int32_t i32 = (int32_t)g.session_req[i].value;
        float f = (float)g.session_req[i].value; int64_t i64 = (int64_t)g.session_req[i].value; double d = g.session_req[i].value;
        camera_status_t st;
        switch (g.session_req[i].type) {
        case 0: st = ACaptureRequest_setEntry_u8(req, g.session_req[i].tag, 1, &u8); break;
        case 1: st = ACaptureRequest_setEntry_i32(req, g.session_req[i].tag, 1, &i32); break;
        case 2: st = ACaptureRequest_setEntry_float(req, g.session_req[i].tag, 1, &f); break;
        case 3: st = ACaptureRequest_setEntry_i64(req, g.session_req[i].tag, 1, &i64); break;
        default: st = ACaptureRequest_setEntry_double(req, g.session_req[i].tag, 1, &d); break;
        }
        if (st == ACAMERA_OK) ++n;
    }
    return n;
}

/* Before indraw_open, from the device profile: raw_off = no RAW stream;
 * jpeg_max_w x jpeg_max_h = the largest JPEG asked for (0,0 = none). */
IR_API int indraw_set_stream_limits(int raw_off, int jpeg_max_w, int jpeg_max_h)
{
    if (jpeg_max_w < 0 || jpeg_max_h < 0 || (jpeg_max_w > 0) != (jpeg_max_h > 0)) return IR_EARG;
    g.lim_raw_off = raw_off ? 1 : 0;
    g.lim_jw = jpeg_max_w; g.lim_jh = jpeg_max_h;
    return IR_OK;
}

/* Device-profile pause before every capture, in ms (0..3000; 0 = none). */
IR_API int indraw_set_capture_delay(int ms)
{
    g.capture_delay_ms = ms < 0 ? 0 : ms > 3000 ? 3000 : ms;
    return IR_OK;
}

/* Device-profile switch, before indraw_open: build the photo/RAW requests
 * from the preview template (1) or the still template (0, default). */
IR_API int indraw_set_still_template(int preview)
{
    g.still_template_preview = preview ? 1 : 0;
    return IR_OK;
}

/* READ-ONLY: what the preview, photo and RAW requests carry after the HAL's
 * templates and our setup -- the AF mode, the capture intent, and every
 * vendor request key (tag >= 0x80000000 listed by the HAL) with its type
 * (u8/i32/float/i64/double/rational) and values. To see what a still
 * template adds that the preview's lacks. Changes nothing. */
static void report_request(jw_t *w, const char *name, const ACaptureRequest *req, const ACameraMetadata_const_entry *keys)
{
    ACameraMetadata_const_entry e;
    uint32_t i, k;
    static const char *const tn[] = { "u8", "i32", "float", "i64", "double", "rational" };
    jw_add(w, "\"%s\":{", name);
    if (!req) { jw_add(w, "\"absent\":true}"); return; }
    if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_AF_MODE, &e) == ACAMERA_OK && e.count)
        jw_add(w, "\"afMode\":%u,", e.data.u8[0]);
    if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_CAPTURE_INTENT, &e) == ACAMERA_OK && e.count)
        jw_add(w, "\"captureIntent\":%u,", e.data.u8[0]);
    if (ACaptureRequest_getConstEntry(req, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count)
        jw_add(w, "\"focusDistance\":%.3f,", e.data.f[0]);
    jw_add(w, "\"vendor\":{");
    {
        int first = 1;
        for (i = 0; keys && i < keys->count; ++i) {
            uint32_t tag = (uint32_t)keys->data.i32[i];
            if (tag < 0x80000000u) continue;
            jw_add(w, "%s\"0x%08x\":", first ? "" : ",", tag);
            first = 0;
            if (ACaptureRequest_getConstEntry(req, tag, &e) != ACAMERA_OK || !e.count) { jw_add(w, "\"unset\""); continue; }
            jw_add(w, "{\"type\":\"%s\",\"v\":[", e.type < 6 ? tn[e.type] : "?");
            for (k = 0; k < e.count && k < 8; ++k) {
                const char *sp = k ? "," : "";
                switch (e.type) {
                case ACAMERA_TYPE_BYTE: jw_add(w, "%s%u", sp, e.data.u8[k]); break;
                case ACAMERA_TYPE_INT32: jw_add(w, "%s%d", sp, e.data.i32[k]); break;
                case ACAMERA_TYPE_FLOAT: jw_add(w, "%s%g", sp, e.data.f[k]); break;
                case ACAMERA_TYPE_INT64: jw_add(w, "%s%lld", sp, (long long)e.data.i64[k]); break;
                case ACAMERA_TYPE_DOUBLE: jw_add(w, "%s%g", sp, e.data.d[k]); break;
                default: jw_add(w, "%s0", sp); break;
                }
            }
            jw_add(w, "]}");
        }
    }
    jw_add(w, "}}");
}
IR_API int indraw_template_report(char *out, size_t out_size)
{
    ACameraMetadata_const_entry keys;
    const ACameraMetadata_const_entry *kp = NULL;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.chars) return fail(out, out_size, IR_ESTATE, "templates", "camera not open", 0);
    if (ACameraMetadata_getConstEntry(g.chars, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS, &keys) == ACAMERA_OK &&
        keys.type == ACAMERA_TYPE_INT32) kp = &keys;
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"still_template\":\"%s\",", g.still_template_preview ? "preview" : "still");
    report_request(&w, "preview", g.preview_request, kp); jw_add(&w, ",");
    report_request(&w, "photo", g.still_request, kp); jw_add(&w, ",");
    report_request(&w, "raw", g.raw_request, kp);
    jw_add(&w, "}");
    return IR_OK;
}

IR_API int indraw_vendor_set(uint32_t tag, int type, double value, int on, char *out, size_t out_size)
{
    ACaptureRequest *all[5], *reqs[1];
    int n = 0, i, rc = 0, applied = 0;
    camera_status_t st = ACAMERA_OK;
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_request) return fail(out, out_size, IR_ESTATE, "vendor", "camera not open", 0);
    if (on == 0 || on == 2) { all[n++] = g.preview_request; if (g.meter_request) all[n++] = g.meter_request; }
    if (on == 1 || on == 2) { all[n++] = g.still_request; all[n++] = g.trigger_request; if (g.raw_request) all[n++] = g.raw_request; }
    for (i = 0; i < n; ++i) {
        if (!all[i]) continue;
        switch (type) {
        case 0: { uint8_t v = (uint8_t)value; st = ACaptureRequest_setEntry_u8(all[i], tag, 1, &v); break; }
        case 1: { int32_t v = (int32_t)value; st = ACaptureRequest_setEntry_i32(all[i], tag, 1, &v); break; }
        case 2: { float v = (float)value; st = ACaptureRequest_setEntry_float(all[i], tag, 1, &v); break; }
        case 3: { int64_t v = (int64_t)value; st = ACaptureRequest_setEntry_i64(all[i], tag, 1, &v); break; }
        default: return fail(out, out_size, IR_EARG, "vendor", "type: 0 u8, 1 i32, 2 float, 3 i64", type);
        }
        if (st != ACAMERA_OK) { rc = st; break; }
        applied++;
    }
    if (rc == 0) {                                   /* watch it in the results */
        int vi, known = 0;
        pthread_mutex_lock(&g.lock);
        for (vi = 0; vi < g.vw_n; ++vi) if (g.vw_tag[vi] == tag) known = 1;
        if (!known && g.vw_n < 8) { g.vw_tag[g.vw_n] = tag; g.vw_value[g.vw_n][0] = '\0'; g.vw_n++; }
        pthread_mutex_unlock(&g.lock);
    }
    if (rc == 0 && (on == 0 || on == 2)) {
        reqs[0] = g.preview_request;
        st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
        if (st != ACAMERA_OK) rc = st;
    }
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":%s,\"vendor_tag\":\"0x%08x\",\"type\":%d,\"value\":%g,\"on\":%d,\"requests\":%d%s}",
           rc == 0 ? "true" : "false", (unsigned)tag, type, value, on, applied, rc ? ",\"error\":\"refused by the NDK (unpublished key or wrong type)\"" : "");
    return rc == 0 ? IR_OK : IR_ECAPTURE;
}

IR_API int indraw_flash(int mode, char *out, size_t out_size)
{
    ACameraMetadata_const_entry e;
    ACaptureRequest *reqs[1], *all[5];
    int n = 0, i;
    uint8_t fm, ae;
    camera_status_t st;
    jw_t w;
    if (!out || !out_size || mode < 0 || mode > 3) return IR_EARG;
    if (!g.session || !g.preview_request)
        return fail(out, out_size, IR_ESTATE, "flash", "camera not open", 0);
    if (mode > 0 && !(ACameraMetadata_getConstEntry(g.chars, ACAMERA_FLASH_INFO_AVAILABLE, &e) == ACAMERA_OK &&
                      e.count && e.data.u8[0]))
        return fail(out, out_size, IR_EARG, "flash", "this camera has no flash", mode);
    g.flash_mode = mode;
    all[n++] = g.preview_request;
    all[n++] = g.still_request;
    all[n++] = g.trigger_request;
    if (g.raw_request) all[n++] = g.raw_request;
    if (g.meter_request) all[n++] = g.meter_request;
    fm = mode == 3 ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
    for (i = 0; i < n; ++i) {
        ACameraMetadata_const_entry am;
        if (!all[i]) continue;
        ACaptureRequest_setEntry_u8(all[i], ACAMERA_FLASH_MODE, 1, &fm);
        /* Requests in automatic exposure take the flash-aware AE mode. */
        if (ACaptureRequest_getConstEntry(all[i], ACAMERA_CONTROL_AE_MODE, &am) == ACAMERA_OK && am.count &&
            am.data.u8[0] != ACAMERA_CONTROL_AE_MODE_OFF) {
            ae = mode == 1 ? ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH
               : mode == 2 ? ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH : ACAMERA_CONTROL_AE_MODE_ON;
            ACaptureRequest_setEntry_u8(all[i], ACAMERA_CONTROL_AE_MODE, 1, &ae);
        }
    }
    reqs[0] = g.preview_request;
    st = ACameraCaptureSession_setRepeatingRequest(g.session, &g_preview_cbs, 1, reqs, NULL);
    if (st != ACAMERA_OK)
        return fail(out, out_size, IR_ECAPTURE, "flash", "setRepeatingRequest failed", st);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"flash\":\"%s\"}", mode == 0 ? "off" : mode == 1 ? "auto" : mode == 2 ? "on" : "torch");
    return IR_OK;
}

/* Turns the exposure meter on/off and sets its period (ms, >= 50). While on,
 * the engine captures a RAW16 frame every period, measures it (meter_frame)
 * and emits an SF2M "meter" line. Needs a RAW stream in the session. */
IR_API int indraw_meter(int on, int interval_ms, char *out, size_t out_size)
{
    jw_t w;
    if (!out || !out_size) return IR_EARG;
    if (!g.session) return fail(out, out_size, IR_ESTATE, "meter", "camera not open", 0);
    if (on && !g.meter_request) return fail(out, out_size, IR_ESTATE, "meter", "session has no raw16 stream", 0);
    if (interval_ms > 0 && interval_ms < 50) interval_ms = 50;
    pthread_mutex_lock(&g.lock);
    g.meter_on = on ? 1 : 0;
    if (interval_ms > 0) g.meter_interval_ms = interval_ms;
    pthread_cond_broadcast(&g.cond);              /* wake the thread now          */
    pthread_mutex_unlock(&g.lock);
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"meter\":%s,\"interval_ms\":%d}", g.meter_on ? "true" : "false", g.meter_interval_ms);
    return IR_OK;
}

/* Diagnostic (phase 1): grabs one live preview frame, converts it to RGB and
 * writes it as a PPM (a trivial, dependency-free image: a short text header then
 * raw RGB bytes) to `path`. Lets us check the preview content and, by comparing
 * its framing to a normal capture of the same scene, whether the viewfinder
 * will match the photo. Not the real frame stream yet -- that comes next. */
IR_API int indraw_preview_frame(const char *path, char *out, size_t out_size)
{
    jw_t jw;
    struct timespec dl;
    int err, w, h;

    if (!path || !out || !out_size) return IR_EARG;
    if (!g.session || !g.preview_reader)
        return fail(out, out_size, IR_ESTATE, "preview_frame", "camera not open", 0);

    /* First WAIT for the preview to settle: auto-exposure converged and (if the
     * device has continuous AF) focus not actively scanning. The first frames
     * after the stream starts are mid-convergence -- wrong exposure, wrong
     * white balance, and heavily denoised while the ISO is still high -- which
     * is why an early grab looked flat and colourless while a later grab of the
     * same scene looked fine. Then ask the listener to save the next frame. */
    pthread_mutex_lock(&g.lock);
    dl = deadline_in(3000);
    while (!(ae_done(g.live_ae) && (!g.af_continuous || af_done(g.live_af))) &&
           !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    /* Now request the frame (whether or not 3A fully converged before timeout). */
    g.pf_done = 0;
    g.pf_err = 0;
    snprintf(g.pf_path_buf, sizeof g.pf_path_buf, "%s", path);
    g.pf_path = g.pf_path_buf;                     /* request: save next frame     */
    dl = deadline_in(2000);
    while (!g.pf_done && !g.device_error)
        if (pthread_cond_timedwait(&g.cond, &g.lock, &dl) == ETIMEDOUT) break;
    err = g.pf_err;
    w = g.pf_w; h = g.pf_h;
    if (!g.pf_done) g.pf_path = NULL;              /* timed out: cancel request    */
    pthread_mutex_unlock(&g.lock);

    if (!g.pf_done) return fail(out, out_size, IR_ECAPTURE, "preview_frame", "no preview frame in time", 0);
    if (err) return fail(out, out_size, IR_EFILE, "preview_frame", "convert or write failed", 0);

    jw_init(&jw, out, out_size);
    jw_add(&jw, "{\"ok\":true,\"path\":");
    jw_str(&jw, path);
    jw_add(&jw, ",\"size\":[%d,%d],\"format\":\"ppm_rgb888\"}", w, h);
    return IR_OK;
}

IR_API int indraw_close(char *out, size_t out_size)
{
    double t0 = now_ms();
    jw_t w;
    if (!g.device && !g.manager) return fail(out, out_size, IR_ESTATE, "close", "not open", 0);
    close_all();
    jw_init(&w, out, out_size);
    jw_add(&w, "{\"ok\":true,\"ms\":{\"close\":%.1f}}", now_ms() - t0);
    return IR_OK;
}
