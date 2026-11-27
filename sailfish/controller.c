/*
 * controller.c -- the controller layer: the event pipe from the engine, the
 * lock, and (step 3) the exposure behaviour wired to the engine's events and
 * the interface's commands. See controller.h and docs/ARCHITECTURE.md.
 */
#define _POSIX_C_SOURCE 200809L
#include "controller.h"
#include "ctl_exposure.h"
#include "ctl_focus.h"
#include "ctl_view.h"
#include "ctl_shoot.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>    /* O_NONBLOCK, F_SETPIPE_SZ on the event pipe */

#define CTL_HELD_MAX 8
#define CTL_HELD_LINE 96

static struct {
    int rd, wr;                 /* the pipe: the engine writes, we read       */
    pthread_t thread;
    int running;
    ctl_emit_fn emit;
    ctl_set_event_fd_fn set_event_fd;
    ctl_engine_line_fn engine_line;
    pthread_mutex_t lock;
    ctl_exposure_t *exp;
    ctl_focus_t *foc;
    ctl_view_t *view;
    int live_iso;               /* the last "ae" values, for vfmeter samples  */
    double live_exp_ns;
    int busy;                   /* a long engine command runs on the CLI thread */
    int deferred;               /* an exposure command was dropped meanwhile   */
    /* The focus and view commands decided while busy (see held_cmd), sent
     * when the capture is over: the last of each kind, in order. */
    char held_q[CTL_HELD_MAX][CTL_HELD_LINE];
    int held_n;
    /* The display window last sent to the engine ("viewwin"), as sent:
     * every view publish (a loupe move, a ref, the light point) re-sent it,
     * and the engine re-set its meter zones each time. Forgotten at a caps
     * line (a new session: the engine's own default again). */
    double viewwin_sent[4];
    int viewwin_known;
    int ui_paused, focus_paused; /* the two reasons to leave the exposure alone */
    ctl_caps_t caps;             /* the last caps, for the shoot decisions */
    ctl_log_fn log;              /* diagnostics to the log, or NULL           */
    int detail;                  /* the detailed log is on (ctl_set_detail)   */
    /* The focus trace (detailed log): from a focus touch, for 4 s, every
     * change of the HAL's AF state or lens position, with the time since the
     * touch. To see what a camera does with a search: whether the lens moves,
     * how far, and when the HAL gives up (the 18 mm, 14 Nov). */
    double trace_t0, trace_until;
    int trace_af;
    double trace_lens;
    /* The exposure trace (detailed log; 0.3.11, patch 0553): every change of the
     * exposure -- the pair in the HAL's results, its AE state, the pair the
     * wheels show, the viewfinder's brightness -- with the time, to see
     * where a pumping exposure comes from (the JP2's 18 mm, 26 Nov 2026:
     * the image and the wheels going back and forth, as in Jolla Camera).
     * ae_*: the values now; ael_*: as last written (ae_written 0: none yet,
     * the next change writes all). See ae_trace. */
    double ae_t0;
    int ae_written;
    int ae_iso, ae_state, ae_sh_iso;
    double ae_exp_ns, ae_sh_ns, ae_vf;
    int ael_iso, ael_state, ael_sh_iso;
    double ael_exp_ns, ael_sh_ns, ael_vf;
    char capture_q[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE];
    int capture_n, capture_i;
} c = { .rd = -1, .wr = -1,
};
/* The lock is recursive from its first use and initialised exactly once
 * (pthread_once): re-initialising it in every ctl_start would be undefined
 * on a stop/start cycle. */
static pthread_once_t lock_once = PTHREAD_ONCE_INIT;
static void lock_init(void)
{
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c.lock, &at);
    pthread_mutexattr_destroy(&at);
}

/* The exposure is left alone while EITHER the interface says so (the loupe
 * being moved) or a focus search runs: one flag in the exposure module, two
 * sources here, so neither can un-pause the other's reason. */
static void apply_pause(void) { if (c.exp) ctl_exp_set_paused(c.exp, c.ui_paused || c.focus_paused); }

void ctl_lock(void) { pthread_once(&lock_once, lock_init); pthread_mutex_lock(&c.lock); }
void ctl_unlock(void) { pthread_mutex_unlock(&c.lock); }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* key=value lookup in one event line (the engine's format). */
static const char *field(const char *line, const char *key)
{
    size_t kl = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ' ') && p[kl] == '=') return p + kl + 1;
        p += kl;
    }
    return NULL;
}
static double fieldd(const char *line, const char *key, double dflt)
{
    const char *v = field(line, key);
    return v ? atof(v) : dflt;
}

static void note(const char *fmt, ...);

/* --- the exposure behaviour's two callbacks --- */
static void exp_cmd(void *ctx, const char *engine_command)
{
    (void)ctx;
    if (c.busy) { c.deferred = 1; return; }    /* a capture runs: re-applied after */
    if (c.engine_line) c.engine_line(engine_command);
}

/* The command's kind: its first word, and for "tap" its last (af,
 * afclear, ae, aeclear: a focus search and a light point are not the same
 * order). A later command of a kind replaces the earlier one. */
static size_t kind_len(const char *cmd, const char **tail)
{
    size_t n = strcspn(cmd, " ");
    *tail = "";
    if (n == 3 && !strncmp(cmd, "tap", 3)) { const char *sp = strrchr(cmd, ' '); if (sp) *tail = sp + 1; }
    return n;
}
static int same_kind(const char *a, const char *b)
{
    const char *ta, *tb;
    size_t na = kind_len(a, &ta), nb = kind_len(b, &tb);
    return na == nb && !strncmp(a, b, na) && !strcmp(ta, tb);
}

/* A focus or view command while a capture runs: held, and sent once the
 * capture is over -- the last of each kind, in the order decided. Unlike
 * the exposure (re-derived from its state afterwards), these are decisions
 * taken on the HAL's own reports (a touch's "focus hold" when its search
 * ends during the photo, the return to the manual distance, a throttled
 * loupe): dropped, they were lost while the controller went on as if the
 * engine had them. */
static void held_cmd(const char *engine_command)
{
    int i;
    if (!c.busy) { if (c.engine_line) c.engine_line(engine_command); return; }
    for (i = 0; i < c.held_n; ++i)
        if (same_kind(c.held_q[i], engine_command)) {        /* superseded: out, the new one last */
            memmove(c.held_q[i], c.held_q[i + 1], (size_t)(c.held_n - i - 1) * sizeof c.held_q[0]);
            c.held_n--;
            break;
        }
    if (c.held_n == CTL_HELD_MAX) {                            /* more kinds than one capture ever sees */
        note("busy: a held command dropped (%.40s)", c.held_q[0]);
        memmove(c.held_q[0], c.held_q[1], (size_t)(CTL_HELD_MAX - 1) * sizeof c.held_q[0]);
        c.held_n--;
    }
    if (strlen(engine_command) >= CTL_HELD_LINE) { note("busy: a command too long to hold (%.40s)", engine_command); return; }
    snprintf(c.held_q[c.held_n++], CTL_HELD_LINE, "%s", engine_command);
}

void ctl_engine_busy(int busy)
{
    ctl_lock();
    c.busy = !!busy;
    if (!busy) {
        int i, n = c.held_n;
        if (c.deferred) {
            c.deferred = 0;
            if (c.exp) ctl_exp_restart(c.exp);      /* meter + the current pair, now */
        }
        c.held_n = 0;
        for (i = 0; i < n; ++i) if (c.engine_line) c.engine_line(c.held_q[i]);
    }
    ctl_unlock();
}
static void ae_trace(void);
static void exp_state(void *ctx, const ctl_exp_state_t *s)
{
    char line[420], ev100[16] = "nan";            /* "nan": no measure yet */
    (void)ctx;
    /* the pair the wheels show, for the exposure trace */
    ctl_lock();
    c.ae_sh_iso = s->iso; c.ae_sh_ns = s->shutter_ns;
    ae_trace();
    ctl_unlock();
    if (!isnan(s->ev100)) snprintf(ev100, sizeof ev100, "%.2f", s->ev100);
    snprintf(line, sizeof line,
             "xpstate stops=%.2f stops_valid=%d stops_clip=%d iso=%d exp_ns=%.0f iso_manual=%d shutter_manual=%d full_auto=%d full_manual=%d "
             "fixed=%d spot=%d ev=%d offset=%.3f offset_valid=%d pv_ms=%.0f pv_slow=%d deficit=%.2f too_dark=%d ev100=%s\n",
             s->stops, s->stops_valid, s->stops_clip, s->iso, s->shutter_ns, s->iso_manual, s->shutter_manual, s->full_auto, s->full_manual,
             s->fixed_pair, s->spot_hold, s->ev_steps, s->offset_stops, s->offset_valid,
             s->pv_ms, s->pv_slow, s->deficit_stops, s->too_dark, ev100);
    if (c.emit) c.emit(line);
}

/* --- the focus behaviour's callbacks --- */
static void foc_cmd(void *ctx, const char *engine_command)
{
    /* a focus search starts ("tap X Y af"; not "afclear"): the trace begins */
    size_t n = strlen(engine_command);
    if (c.detail && !strncmp(engine_command, "tap ", 4) &&
        n > 3 && !strcmp(engine_command + n - 3, " af")) {
        c.trace_t0 = now_ms(); c.trace_until = c.trace_t0 + 4000.0;
        c.trace_af = -1; c.trace_lens = -1;
        if (c.log) c.log("af trace: search started");
    }
    (void)ctx;
    held_cmd(engine_command);
}
static void foc_pause(void *ctx, int paused) { (void)ctx; c.focus_paused = !!paused; apply_pause(); }
static void foc_spot(void *ctx, int on) { (void)ctx; if (c.exp) { if (on) ctl_exp_spot_start(c.exp); else ctl_exp_spot_stop(c.exp); } }
static void foc_state(void *ctx, const ctl_focus_state_t *s)
{
    char line[160];
    (void)ctx;
    snprintf(line, sizeof line, "fcstate mode=%d d=%.3f circle=%d searching=%d light=%d\n",
             s->mode, s->diopters, s->circle, s->searching, s->light_ref);
    if (c.emit) c.emit(line);
}

/* --- the view geometry's callbacks --- */
static void view_cmd(void *ctx, const char *engine_command) { (void)ctx; held_cmd(engine_command); }
static void view_ref(void *ctx, double fx, double fy) { (void)ctx; if (c.foc) ctl_focus_ref(c.foc, fx, fy, now_ms()); }
static void view_state(void *ctx, const ctl_view_state_t *s)
{
    char line[200];
    (void)ctx;
    /* the engine's meters cover what is shown: tell it the display window */
    if (!c.viewwin_known || c.viewwin_sent[0] != s->view.x || c.viewwin_sent[1] != s->view.y ||
        c.viewwin_sent[2] != s->view.w || c.viewwin_sent[3] != s->view.h) {   /* only a changed window */
        c.viewwin_sent[0] = s->view.x; c.viewwin_sent[1] = s->view.y;
        c.viewwin_sent[2] = s->view.w; c.viewwin_sent[3] = s->view.h;
        c.viewwin_known = 1;
        snprintf(line, sizeof line, "viewwin %.5f %.5f %.5f %.5f", s->view.x, s->view.y, s->view.w, s->view.h);
        held_cmd(line);
    }
    snprintf(line, sizeof line, "vwstate zoom=%.3f loupe=%.3f view=%.5f,%.5f,%.5f,%.5f ref=%d,%.5f,%.5f light=%d,%.5f,%.5f\n",
             s->zoom, s->loupe, s->view.x, s->view.y, s->view.w, s->view.h, s->has_ref, s->ref_u, s->ref_v,
             s->has_light, s->light_u, s->light_v);
    if (c.emit) c.emit(line);
}

/* (The interface still draws from ae / vfmeter / meter, and its histogram
 * comes from them.) */
void ctl_set_log(ctl_log_fn log) { c.log = log; }
void ctl_set_detail(int on) { ctl_lock(); c.detail = !!on; if (!on) c.trace_until = 0; c.ae_written = 0; ctl_unlock(); }

/* One "ae" line during a focus trace: written when the AF state or the lens
 * position (HAL value, 0.02 apart) changed. The states, Camera2's: 0
 * inactive, 1 passive scan, 2 passive focused, 3 active scan, 4 focused
 * and locked, 5 not focused and locked (the search failed), 6 passive
 * unfocused. Called on the reader thread under the controller's lock, as the
 * touch that starts a trace (foc_cmd) is: the trace fields are never read
 * and written at once. */
static void af_trace(const char *line)
{
    double now = now_ms(), lens = fieldd(line, "focus", -1);
    int af = (int)fieldd(line, "af_state", -1);
    static const char *const names[] = { "inactive", "passive scan", "passive focused", "active scan",
                                         "focused locked", "not focused locked", "passive unfocused" };
    char t[160];
    if (!c.detail || !c.log || now > c.trace_until) return;
    if (af == c.trace_af && fabs(lens - c.trace_lens) < 0.02) return;
    c.trace_af = af; c.trace_lens = lens;
    snprintf(t, sizeof t, "af trace +%4.0f ms: state %d (%s), lens %.3f", now - c.trace_t0, af,
             af >= 0 && af <= 6 ? names[af] : "?", lens);
    c.log(t);
}

/* A relative change of more than `tol` between a and b (both > 0); a value
 * appearing or going counts as a change. */
static int changed(double a, double b, double tol)
{
    if ((a > 0) != (b > 0)) return 1;
    return a > 0 && fabs(a - b) > tol * b;
}

/* A speed as the wheel says it ("1/50 s", "0.50 s"), into t. */
static void speed_text(double ns, char *t, size_t n)
{
    if (ns <= 0) snprintf(t, n, "?");
    else if (ns < 4e8) snprintf(t, n, "1/%.0f s", 1e9 / ns);
    else snprintf(t, n, "%.2f s", ns / 1e9);
}

/* The exposure trace (detailed log; see c.ae_*): one line when the HAL's
 * pair (more than 3 % on the time or the ISO) or its AE state changed,
 * when the wheels' pair did, or when the viewfinder's mean (vfmeter)
 * moved by more than 5 % -- a steady exposure writes nothing. "t x ISO"
 * is log2(time in s x ISO / 100), the light the pair takes in, in stops:
 * a pumping AE shows it going up and down, a mean that pumps with a steady
 * pair would be the image's own processing. Under the controller's lock
 * (taken: it is recursive). */
static void ae_trace(void)
{
    static const char *const st[] = { "inactive", "searching", "converged", "locked", "flash required", "precapture" };
    char t[300], hs[24], ss[24], vf[16] = "none";
    double now;
    ctl_lock();
    if (!c.detail || !c.log || c.ae_iso <= 0 || c.ae_exp_ns <= 0) { ctl_unlock(); return; }
    if (c.ae_written && !changed(c.ae_iso, c.ael_iso, 0.03) && !changed(c.ae_exp_ns, c.ael_exp_ns, 0.03)
        && c.ae_state == c.ael_state && c.ae_sh_iso == c.ael_sh_iso && !changed(c.ae_sh_ns, c.ael_sh_ns, 0.005)
        && !changed(c.ae_vf, c.ael_vf, 0.05)) { ctl_unlock(); return; }
    now = now_ms();
    if (!c.ae_written) c.ae_t0 = now;
    speed_text(c.ae_exp_ns, hs, sizeof hs);
    speed_text(c.ae_sh_ns, ss, sizeof ss);
    if (c.ae_vf >= 0) snprintf(vf, sizeof vf, "%.3f", c.ae_vf);
    snprintf(t, sizeof t, "ae trace +%6.0f ms: HAL ISO %d %s (t x ISO %+.2f), AE %d (%s); wheels ISO %d %s; viewfinder mean %s",
             now - c.ae_t0, c.ae_iso, hs, log2(c.ae_exp_ns / 1e9 * c.ae_iso / 100.0), c.ae_state,
             c.ae_state >= 0 && c.ae_state <= 5 ? st[c.ae_state] : "?", c.ae_sh_iso, ss, vf);
    c.log(t);
    c.ae_written = 1;
    c.ael_iso = c.ae_iso; c.ael_exp_ns = c.ae_exp_ns; c.ael_state = c.ae_state;
    c.ael_sh_iso = c.ae_sh_iso; c.ael_sh_ns = c.ae_sh_ns; c.ael_vf = c.ae_vf;
    ctl_unlock();
}

/* A diagnostic: to the log (ctl_set_log, "# ..." in rawfish-ir.log) and to
 * stderr (the system journal: the interface forwards the CLI's stderr). */
static void note(const char *fmt, ...)
{
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (c.log) c.log(t);
    fprintf(stderr, "rawfish-ir-cli: controller: %s\n", t);
}

/* --- "written" events held for a correction (the lens fix, lensfix.c) ---
 * The CLI rewrites a camera JPEG after the engine wrote it; the interface
 * must hear of the file only once it is final. Before the capture the CLI
 * names the paths (ctl_hold_written); the engine's "written ... path=P"
 * for one of them waits here, and goes out at ctl_release_written(P) --
 * which may come before the event (the pipe is read on another thread):
 * then the event, when it comes, goes straight through. */
#define CTL_HOLD_MAX 8
static struct { char path[1200]; char line[1300]; int state; } held_w[CTL_HOLD_MAX];   /* state: 0 free, 1 waiting, 2 event held, 3 released */

static int written_path_is(const char *line, const char *path)
{
    const char *p = strstr(line, " path=");
    size_t n = strlen(path);
    if (!p) return 0;
    p += 6;
    return !strncmp(p, path, n) && (p[n] == '\0' || p[n] == ' ' || p[n] == '\n');
}

void ctl_hold_written(const char *path)
{
    int i;
    if (!path || strlen(path) >= sizeof held_w[0].path) return;
    ctl_lock();
    for (i = 0; i < CTL_HOLD_MAX; ++i)
        if (!held_w[i].state) break;
    if (i == CTL_HOLD_MAX)                          /* full: reuse one released whose event never came */
        for (i = 0; i < CTL_HOLD_MAX; ++i) if (held_w[i].state == 3) break;
    if (i < CTL_HOLD_MAX) { snprintf(held_w[i].path, sizeof held_w[i].path, "%s", path); held_w[i].state = 1; }
    ctl_unlock();
}

void ctl_release_written(const char *path)
{
    int i;
    char line[1300] = "";
    ctl_lock();
    for (i = 0; i < CTL_HOLD_MAX; ++i)
        if (held_w[i].state && !strcmp(held_w[i].path, path)) {
            if (held_w[i].state == 2) { snprintf(line, sizeof line, "%s", held_w[i].line); held_w[i].state = 0; }
            else if (held_w[i].state == 1) held_w[i].state = 3;      /* the event, when it comes, passes */
            break;
        }
    ctl_unlock();
    if (line[0] && c.emit) c.emit(line);
}

/* 1 if the event is held (not to be forwarded now). */
static int hold_written(const char *line)
{
    int i, held = 0;
    if (strncmp(line, "written ", 8)) return 0;
    ctl_lock();
    for (i = 0; i < CTL_HOLD_MAX; ++i)
        if (held_w[i].state && written_path_is(line, held_w[i].path)) {
            if (held_w[i].state == 1) {
                size_t n = strlen(line);
                if (n >= sizeof held_w[i].line) n = sizeof held_w[i].line - 1;   /* paths are under 1200 bytes */
                memcpy(held_w[i].line, line, n); held_w[i].line[n] = '\0';
                held_w[i].state = 2; held = 1;
            }
            else if (held_w[i].state == 3) held_w[i].state = 0;
            break;
        }
    ctl_unlock();
    return held;
}

/* One event line from the engine: read by the exposure behaviour, then
 * passed on to the interface as it is -- except a diagnostic meant for the
 * log only (vfpace: the viewfinder's frame rate and delays, with the
 * detailed log), which goes to the log and nowhere else. */
static void on_event(const char *line)
{
    if (!strncmp(line, "hwprobe ", 8)) {       /* the zero-copy probe (branch zero_copy): the log only */
        char t[420];
        snprintf(t, sizeof t, "%.410s", line);
        t[strcspn(t, "\n")] = '\0';
        if (c.log) c.log(t);
        return;
    }
    if (!strncmp(line, "lensdiag ", 9)) {      /* the lens before / after a photo (lens_note): the log only */
        char t[260];
        snprintf(t, sizeof t, "lens %.250s", line + 9);
        t[strcspn(t, "\n")] = '\0';
        if (c.log) c.log(t);
        return;
    }
    if (!strncmp(line, "vfpace ", 7)) {        /* the viewfinder's pace (detailed log, see vf_pace) */
        char t[200];
        snprintf(t, sizeof t, "viewfinder pace %.160s", line + 7);
        t[strcspn(t, "\n")] = '\0';
        if (c.log) c.log(t);
        return;
    }
    if (hold_written(line)) return;            /* a file the CLI is still correcting */
    ctl_lock();
    if (c.exp) {
        if (!strncmp(line, "caps ", 5)) {
            c.ae_written = 0; c.ae_vf = -1;             /* a new session: the exposure trace starts again */
            ctl_caps_t caps;
            const char *ev = field(line, "ev_step");
            int n = 1, d = 3;
            memset(&caps, 0, sizeof caps);
            caps.iso_min = (int)fieldd(line, "iso_min", 0);
            caps.iso_max = (int)fieldd(line, "iso_max", 0);
            caps.exp_min_ns = fieldd(line, "exp_min_ns", 1000000);
            caps.shutter_max_ns = fieldd(line, "shutter_max_ns", 500000000);
            if (ev && sscanf(ev, "%d/%d", &n, &d) == 2 && d > 0) caps.ev_step = (double)n / d; else caps.ev_step = 1.0 / 3;
            caps.focal_eq = fieldd(line, "focal_eq", 26);
            caps.aperture = fieldd(line, "aperture", 0);
            c.caps = caps;
            /* Without an ISO range the exposure behaviour stays off (not
             * ready) and sends nothing: said once per caps line, so that a
             * cut or malformed caps line does not leave it silently dead. */
            if (caps.iso_min <= 0 || caps.iso_max < caps.iso_min)
                note("caps without a valid ISO range (iso_min=%d iso_max=%d): exposure control off", caps.iso_min, caps.iso_max);
            ctl_exp_set_caps(c.exp, &caps);
            c.viewwin_known = 0;                        /* a new session: the window sent again */
            if (c.view) { ctl_view_set_caps(c.view, fieldd(line, "zoom_max", 1), (int)fieldd(line, "crop_free", 0)); ctl_view_restart(c.view); }
            if (c.foc) {
                /* the closest distance this camera reaches (min_focus_real,
                 * else its HAL value, else the old 10 D) */
                ctl_focus_set_max(c.foc, fieldd(line, "min_focus_real", fieldd(line, "min_focus", 10.0)));
                ctl_focus_set_search(c.foc, (int)fieldd(line, "touch_focus", 1) != 0);   /* 0: no search at all (the 18 mm) */
                ctl_focus_set_step_pct(c.foc, fieldd(line, "focus_step_pct", 0));   /* the profile's step (the 18 mm) */
                ctl_focus_restart(c.foc);                /* a (re)started engine: the mode again */
            }
        } else if (!strncmp(line, "ae ", 3)) {
            c.live_iso = (int)fieldd(line, "iso", 0);
            c.live_exp_ns = fieldd(line, "exp_ns", 0);
            ctl_exp_live(c.exp, c.live_iso, c.live_exp_ns, (int)fieldd(line, "ae_state", 0));
            if (c.foc) ctl_focus_live(c.foc, (int)fieldd(line, "af_state", 0), fieldd(line, "focus_real", -1), now_ms());
            af_trace(line);                              /* detailed log: the focus trace */
            c.ae_iso = c.live_iso; c.ae_exp_ns = c.live_exp_ns; c.ae_state = (int)fieldd(line, "ae_state", -1);
            ae_trace();                                  /* detailed log: the exposure trace */
            if (c.view) ctl_view_tick(c.view, now_ms());  /* a throttled loupe command, if any */
        } else if (!strncmp(line, "meter ", 6)) {
            ctl_exp_meter_clip(c.exp, fieldd(line, "mclip_lo", 0), fieldd(line, "mclip_hi", 0));
            ctl_exp_sample(c.exp, fieldd(line, "mean", 0), (int)fieldd(line, "iso", 0), fieldd(line, "exp_ns", 0), now_ms());
        } else if (!strncmp(line, "vfmeter ", 8)) {
            /* the frame's own pair when the engine knew it, else the live one */
            int iso = (int)fieldd(line, "iso", 0);
            double exp_ns = fieldd(line, "exp_ns", 0);
            if (iso <= 0 || exp_ns <= 0) { iso = c.live_iso; exp_ns = c.live_exp_ns; }
            ctl_exp_meter_clip(c.exp, fieldd(line, "mclip_lo", 0), fieldd(line, "mclip_hi", 0));
            ctl_exp_sample(c.exp, fieldd(line, "mean", 0), iso, exp_ns, now_ms());
            c.ae_vf = fieldd(line, "mean", -1);
            ae_trace();                                  /* detailed log: the exposure trace */
        }
    }
    ctl_unlock();
    if (c.emit) c.emit(line);
}

/* The interface's commands for the exposure behaviour:
 *   xp auto | xp touch iso|speed | xp iso N | xp speed NS | xp ev N |
 *   xp spot on|off | xp pause on|off | xp source raw|vf */
/* The interface's commands for the focus behaviour:
 *   fc mode cont|once|manual | fc mode manual search | fc mode manual here |
 *   fc search | fc step N | fc distance D |
 *   fc ref FX FY | fc clear | fc scene on|off | fc state
 * (the interface offers cont and manual; "once" stays for the command line) */
static int focus_command(const char *line)
{
    char a[32], b[32], d[32];
    int n;
    a[0] = b[0] = d[0] = '\0';
    n = sscanf(line + 3, "%31s %31s %31s", a, b, d);
    if (!c.foc || n < 1) return 1;
    if (!strcmp(a, "search")) {
        /* The focus button entering manual: one search in the middle of the
         * frame; the distance found becomes the manual one (as a touch does,
         * without a circle: the view keeps no reference for it). */
        if (!ctl_focus_ref(c.foc, 0.5, 0.5, now_ms()))
            note("fc search: this camera takes no focus search (device profile): manual from the distance shown");
    }
    else if (!strcmp(a, "mode") && !strcmp(b, "manual") && !strcmp(d, "search")) {
        /* "fc mode manual search": the FOCUS button entering manual, in one
         * command: the search starts from where the lens is (see
         * ctl_focus_manual_search), not after a trip to the mode's distance */
        ctl_focus_manual_search(c.foc, now_ms());
        if (c.view) ctl_view_clear_ref(c.view);
    }
    else if (!strcmp(a, "mode") && !strcmp(b, "manual") && !strcmp(d, "here")) {
        /* "fc mode manual here": a touch or a volume key in auto -- manual
         * from where the continuous focus left the lens, which does not
         * move (a touch then searches from there: "vw tap" follows) */
        ctl_focus_manual_here(c.foc);
        if (c.view) ctl_view_clear_ref(c.view);
    }
    else if (!strcmp(a, "mode")) {
        /* A focus mode change drops the focus point and its marker; the light
         * point (the sun) belongs to the exposure and stays, with its marker:
         * changing how to focus must not change the exposure. */
        ctl_focus_set_mode(c.foc, !strcmp(b, "once") ? CTL_FOCUS_ONCE : !strcmp(b, "manual") ? CTL_FOCUS_MANUAL : CTL_FOCUS_CONT);
        if (c.view) ctl_view_clear_ref(c.view);
    }
    else if (!strcmp(a, "step")) ctl_focus_step(c.foc, atoi(b));
    else if (!strcmp(a, "distance")) ctl_focus_set_distance(c.foc, atof(b));
    else if (!strcmp(a, "ref") && n >= 3) ctl_focus_ref(c.foc, atof(b), atof(d), now_ms());
    else if (!strcmp(a, "clear")) { ctl_focus_clear(c.foc); if (c.view) ctl_view_clear_ref(c.view); }
    else if (!strcmp(a, "scene")) ctl_focus_set_scene(c.foc, !strcmp(b, "on"));
    else if (!strcmp(a, "state")) { ctl_focus_state_t s; ctl_focus_state(c.foc, &s); foc_state(NULL, &s); }
    return 1;
}

/* The interface's commands for the view geometry (display-window units,
 * the view rotation undone by the interface):
 *   vw zoom Z | vw loupe L | vw pan DU DV | vw gesture on|off | vw tap U V |
 *   vw clear | vw state */
static int view_command(const char *line)
{
    char a[32], b[32], d[32];
    int n;
    a[0] = b[0] = d[0] = '\0';
    n = sscanf(line + 3, "%31s %31s %31s", a, b, d);
    if (!c.view || n < 1) return 1;
    if (!strcmp(a, "zoom")) ctl_view_set_zoom(c.view, atof(b), now_ms());
    else if (!strcmp(a, "loupe")) ctl_view_set_loupe(c.view, atof(b), now_ms());
    else if (!strcmp(a, "pan") && n >= 3) ctl_view_pan(c.view, atof(b), atof(d), now_ms());
    else if (!strcmp(a, "gesture")) ctl_view_gesture(c.view, !strcmp(b, "on"), now_ms());
    else if (!strcmp(a, "tap") && n >= 3) ctl_view_tap(c.view, atof(b), atof(d));
    else if (!strcmp(a, "clear")) ctl_view_clear_ref(c.view);
    else if (!strcmp(a, "light") && n >= 3) {          /* a long press: the exposure's point */
        double fx, fy;
        ctl_view_light(c.view, atof(b), atof(d), &fx, &fy);
        if (c.foc) ctl_focus_light(c.foc, fx, fy);
    }
    else if (!strcmp(a, "lightclear")) { ctl_view_clear_light(c.view); if (c.foc) ctl_focus_light_clear(c.foc); }
    else if (!strcmp(a, "state")) { ctl_view_state_t s; ctl_view_state(c.view, &s); view_state(NULL, &s); }
    return 1;
}

/* "sh name=N type=jpg|raw|yuv q=Q noise=M edge=M bracket=off|0|1|2|3 bracket_n=3|5|7 denoise_sigma=D
 *     frames=1|2|4|8 nr=classic|tiles colour=0..5 median=0..4 luma=0..3 sharpen=0..5 dng=0|1"
 *     (frames to dng for yuv):
 * a press of the shutter (the self-timer is the interface's). The capture
 * lines go to the queue the CLI drains. */
static int shoot_command(const char *line)
{
    ctl_shoot_req_t r;
    ctl_exp_state_t xs;
    ctl_focus_state_t fs;
    const char *v;
    memset(&r, 0, sizeof r);
    r.bracket = -1; r.denoise = 1; r.quality = 95;
    if ((v = field(line, "name"))) sscanf(v, "%63s", r.name);
    if ((v = field(line, "type"))) { r.raw = !strncmp(v, "raw", 3); r.yuv = !strncmp(v, "yuv", 3); }
    r.frames = 1; r.colour = 2; r.median = 0; r.sharpen = 0;
    if ((v = field(line, "frames"))) r.frames = atoi(v);
    r.nr_tiles = (v = field(line, "nr")) && !strncmp(v, "tiles", 5);
    if ((v = field(line, "colour"))) r.colour = atoi(v);
    if ((v = field(line, "median"))) r.median = atoi(v);
    if ((v = field(line, "sharpen"))) r.sharpen = atoi(v);
    if ((v = field(line, "luma"))) { r.luma_set = 1; r.luma = atoi(v); }   /* absent: the device profile's default */
    r.dng = (v = field(line, "dng")) && atoi(v) == 1;
    r.snap = (v = field(line, "snap")) && atoi(v) == 1;
    if ((v = field(line, "q"))) r.quality = atoi(v);
    if ((v = field(line, "noise"))) sscanf(v, "%23s", r.noise);
    if ((v = field(line, "edge"))) sscanf(v, "%23s", r.edge);
    if ((v = field(line, "bracket"))) r.bracket = !strncmp(v, "off", 3) ? -1 : atoi(v);
    r.bracket_n = (v = field(line, "bracket_n")) ? atoi(v) : 3;   /* bracket 0: 3, 5 or 7 frames */
    /* denoise_sigma: the Σ stack's poses per frame (renamed from 'denoise',
     * which read like a noise-reduction mode; the old name still accepted) */
    if ((v = field(line, "denoise_sigma")) || (v = field(line, "denoise"))) r.denoise = atoi(v);
    if (!c.exp || !c.foc) return 1;
    ctl_exp_state(c.exp, &xs);
    ctl_focus_state(c.foc, &fs);
    c.capture_i = 0;
    c.capture_n = ctl_shoot_lines(&r, &xs, c.caps.shutter_max_ns, c.caps.exp_min_ns, &fs, c.capture_q);
    /* Nothing to take (no name, or a line that would not fit): said, instead
     * of a press that silently does nothing. The interface does not wait for
     * an answer to "sh", so this changes nothing else; the log says it. */
    if (c.capture_n == 0) {
        note("sh: nothing taken (no name, or a capture line too long)");
        if (c.emit) c.emit("{\"ok\":false,\"error\":\"sh: nothing taken (no name, or a capture line too long)\"}\n");
    }
    return 1;
}
int ctl_capture_count(void) { return c.capture_n; }
int ctl_pop_capture(char *buf, size_t n)
{
    if (c.capture_i >= c.capture_n) return 0;
    snprintf(buf, n, "%s", c.capture_q[c.capture_i++]);
    return 1;
}

int ctl_command(const char *line)
{
    char a[32], b[32];
    int n;
    if (!strncmp(line, "sh ", 3)) return shoot_command(line);
    if (!strncmp(line, "vw ", 3)) return view_command(line);
    if (!strncmp(line, "fc ", 3)) return focus_command(line);
    if (strncmp(line, "xp ", 3)) return 0;
    a[0] = b[0] = '\0';
    n = sscanf(line + 3, "%31s %31s", a, b);
    if (!c.exp || n < 1) return 1;
    if (!strcmp(a, "auto")) ctl_exp_reset_auto(c.exp);
    else if (!strcmp(a, "touch") && !strcmp(b, "iso")) ctl_exp_touch_iso(c.exp);
    else if (!strcmp(a, "touch") && !strcmp(b, "speed")) ctl_exp_touch_shutter(c.exp);
    else if (!strcmp(a, "iso")) ctl_exp_set_iso(c.exp, atoi(b));
    else if (!strcmp(a, "speed")) ctl_exp_set_shutter(c.exp, atof(b));
    else if (!strcmp(a, "ev")) ctl_exp_set_ev(c.exp, atoi(b));
    else if (!strcmp(a, "spot")) { if (!strcmp(b, "on")) ctl_exp_spot_start(c.exp); else ctl_exp_spot_stop(c.exp); }
    else if (!strcmp(a, "pause")) { c.ui_paused = !strcmp(b, "on"); apply_pause(); }
    else if (!strcmp(a, "source")) ctl_exp_set_source(c.exp, !strcmp(b, "raw"));
    else if (!strcmp(a, "pvfps")) ctl_exp_set_preview_fps(c.exp, atof(b));
    else if (!strcmp(a, "state")) { ctl_exp_state_t s; ctl_exp_state(c.exp, &s); exp_state(NULL, &s); }
    return 1;
}

/* Reads the pipe until it closes, cutting it into lines. */
static void *reader(void *arg)
{
    char buf[4096], line[4096];
    size_t have = 0;
    int overlong = 0;                                   /* the line did not fit */
    (void)arg;
    for (;;) {
        ssize_t n = read(c.rd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;                              /* writer closed           */
        for (ssize_t i = 0; i < n; ++i) {
            if (have < sizeof line - 1) line[have++] = buf[i];
            else overlong = 1;
            if (buf[i] == '\n') {
                line[have] = '\0';
                /* A line longer than the buffer is dropped and said in the
                 * log, never read cut (a field missing, a number cut short).
                 * No line of today's engine comes near (check-host.sh: the
                 * longest, vfmeter, is 2600 bytes), and the interface drops
                 * SF2M packets over 4096 bytes anyway. */
                if (overlong) note("an event line over %zu bytes dropped", sizeof line - 1);
                else on_event(line);
                have = 0;
                overlong = 0;
            }
        }
    }
    return NULL;
}

int ctl_start(ctl_emit_fn emit, ctl_set_event_fd_fn set_event_fd, ctl_engine_line_fn engine_line)
{
    int fds[2];
    if (c.running) return 0;
    if (!emit || !set_event_fd) { fprintf(stderr, "rawfish-ir-cli: controller: engine entry points missing\n"); return -1; }
    if (pipe(fds) != 0) { perror("rawfish-ir-cli: controller: pipe"); return -1; }
    /* The engine writes events from its camera threads: the write end is
     * non-blocking (a full pipe drops a state line rather than stalling the
     * preview, see emit_event), and the pipe is enlarged to 1 MB where Linux
     * allows (F_SETPIPE_SZ; best effort) so that a reader held up a while
     * (a long engine command on the reader's own line) does not fill it. */
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
#ifdef F_SETPIPE_SZ
    fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
#endif
    c.rd = fds[0]; c.wr = fds[1];
    c.busy = 0; c.deferred = 0; c.held_n = 0; c.viewwin_known = 0;
    c.emit = emit; c.set_event_fd = set_event_fd; c.engine_line = engine_line;
    c.exp = ctl_exp_new(exp_cmd, exp_state, NULL);
    c.foc = ctl_focus_new(foc_cmd, foc_state, foc_pause, foc_spot, NULL);
    c.view = ctl_view_new(view_cmd, view_state, view_ref, NULL);
    /* A failed allocation, or no thread: everything made here is undone. */
    if (!c.exp || !c.foc || !c.view || pthread_create(&c.thread, NULL, reader, NULL) != 0) {
        if (!c.exp || !c.foc || !c.view) fprintf(stderr, "rawfish-ir-cli: controller: out of memory\n");
        else perror("rawfish-ir-cli: controller: thread");
        ctl_view_free(c.view); c.view = NULL;
        ctl_focus_free(c.foc); c.foc = NULL;
        ctl_exp_free(c.exp); c.exp = NULL;
        close(c.rd); close(c.wr); c.rd = c.wr = -1;
        return -1;
    }
    c.running = 1;
    set_event_fd(c.wr);                                  /* from now on, events come here */
    return 0;
}

void ctl_stop(void)
{
    if (!c.running) return;
    c.set_event_fd(-1);                                  /* engine back to the stream     */
    close(c.wr); c.wr = -1;                              /* the reader sees end of file   */
    pthread_join(c.thread, NULL);
    close(c.rd); c.rd = -1;
    ctl_view_free(c.view); c.view = NULL;
    ctl_focus_free(c.foc); c.foc = NULL;
    ctl_exp_free(c.exp); c.exp = NULL;
    c.running = 0;
}
