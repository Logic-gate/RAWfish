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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>    /* O_NONBLOCK, F_SETPIPE_SZ on the event pipe */

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
    int deferred;               /* a controller command was dropped meanwhile  */
    int ui_paused, focus_paused; /* the two reasons to leave the exposure alone */
    ctl_caps_t caps;             /* the last caps, for the shoot decisions */
    char capture_q[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE];
    int capture_n, capture_i;
} c = { .rd = -1, .wr = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* The exposure is left alone while EITHER the interface says so (the loupe
 * being moved) or a focus search runs: one flag in the exposure module, two
 * sources here, so neither can un-pause the other's reason. */
static void apply_pause(void) { if (c.exp) ctl_exp_set_paused(c.exp, c.ui_paused || c.focus_paused); }

void ctl_lock(void) { pthread_mutex_lock(&c.lock); }
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

/* --- the exposure behaviour's two callbacks --- */
static void exp_cmd(void *ctx, const char *engine_command)
{
    (void)ctx;
    if (c.busy) { c.deferred = 1; return; }    /* a capture runs: re-applied after */
    if (c.engine_line) c.engine_line(engine_command);
}

void ctl_engine_busy(int busy)
{
    ctl_lock();
    c.busy = !!busy;
    if (!busy && c.deferred) {
        c.deferred = 0;
        if (c.exp) ctl_exp_restart(c.exp);          /* meter + the current pair, now */
    }
    ctl_unlock();
}
static void exp_state(void *ctx, const ctl_exp_state_t *s)
{
    char line[320];
    (void)ctx;
    snprintf(line, sizeof line,
             "xpstate iso=%d exp_ns=%.0f iso_manual=%d shutter_manual=%d full_auto=%d full_manual=%d "
             "fixed=%d spot=%d ev=%d offset=%.3f offset_valid=%d pv_ms=%.0f pv_slow=%d deficit=%.2f too_dark=%d ev100=%s\n",
             s->iso, s->shutter_ns, s->iso_manual, s->shutter_manual, s->full_auto, s->full_manual,
             s->fixed_pair, s->spot_hold, s->ev_steps, s->offset_stops, s->offset_valid,
             s->pv_ms, s->pv_slow, s->deficit_stops, s->too_dark,
             isnan(s->ev100) ? "nan" : "");
    if (!isnan(s->ev100)) {
        char *p = strstr(line, "ev100=");
        if (p) snprintf(p, (size_t)(line + sizeof line - p), "ev100=%.2f\n", s->ev100);
    }
    if (c.emit) c.emit(line);
}

/* --- the focus behaviour's callbacks --- */
static void foc_cmd(void *ctx, const char *engine_command) { exp_cmd(ctx, engine_command); }
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
static void view_cmd(void *ctx, const char *engine_command) { exp_cmd(ctx, engine_command); }
static void view_ref(void *ctx, double fx, double fy) { (void)ctx; if (c.foc) ctl_focus_ref(c.foc, fx, fy, now_ms()); }
static void view_state(void *ctx, const ctl_view_state_t *s)
{
    char line[200];
    (void)ctx;
    /* the engine's meters cover what is shown: tell it the display window */
    snprintf(line, sizeof line, "viewwin %.5f %.5f %.5f %.5f", s->view.x, s->view.y, s->view.w, s->view.h);
    exp_cmd(NULL, line);
    snprintf(line, sizeof line, "vwstate zoom=%.3f loupe=%.3f view=%.5f,%.5f,%.5f,%.5f ref=%d,%.5f,%.5f\n",
             s->zoom, s->loupe, s->view.x, s->view.y, s->view.w, s->view.h, s->has_ref, s->ref_u, s->ref_v);
    if (c.emit) c.emit(line);
}

/* One event line from the engine: read by the exposure behaviour, then
 * passed on to the interface as it is (it still draws from ae / vfmeter /
 * meter, and its histogram comes from them). */
static void on_event(const char *line)
{
    ctl_lock();
    if (c.exp) {
        if (!strncmp(line, "caps ", 5)) {
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
            ctl_exp_set_caps(c.exp, &caps);
            if (c.view) { ctl_view_set_caps(c.view, fieldd(line, "zoom_max", 1), (int)fieldd(line, "crop_free", 0)); ctl_view_restart(c.view); }
            if (c.foc) ctl_focus_restart(c.foc);         /* a (re)started engine: the mode again */
        } else if (!strncmp(line, "ae ", 3)) {
            c.live_iso = (int)fieldd(line, "iso", 0);
            c.live_exp_ns = fieldd(line, "exp_ns", 0);
            ctl_exp_live(c.exp, c.live_iso, c.live_exp_ns, (int)fieldd(line, "ae_state", 0));
            if (c.foc) ctl_focus_live(c.foc, (int)fieldd(line, "af_state", 0), fieldd(line, "focus_real", -1), now_ms());
            if (c.view) ctl_view_tick(c.view, now_ms());  /* a throttled loupe command, if any */
        } else if (!strncmp(line, "meter ", 6)) {
            ctl_exp_sample(c.exp, fieldd(line, "mean", 0), (int)fieldd(line, "iso", 0), fieldd(line, "exp_ns", 0), now_ms());
        } else if (!strncmp(line, "vfmeter ", 8)) {
            /* the frame's own pair when the engine knew it, else the live one */
            int iso = (int)fieldd(line, "iso", 0);
            double exp_ns = fieldd(line, "exp_ns", 0);
            if (iso <= 0 || exp_ns <= 0) { iso = c.live_iso; exp_ns = c.live_exp_ns; }
            ctl_exp_sample(c.exp, fieldd(line, "mean", 0), iso, exp_ns, now_ms());
        }
    }
    ctl_unlock();
    if (c.emit) c.emit(line);
}

/* The interface's commands for the exposure behaviour:
 *   xp auto | xp touch iso|speed | xp iso N | xp speed NS | xp ev N |
 *   xp spot on|off | xp pause on|off | xp source raw|vf */
/* The interface's commands for the focus behaviour:
 *   fc mode cont|once|manual | fc step N | fc distance D | fc ref FX FY |
 *   fc clear | fc scene on|off | fc state */
static int focus_command(const char *line)
{
    char a[32], b[32], d[32];
    int n;
    a[0] = b[0] = d[0] = '\0';
    n = sscanf(line + 3, "%31s %31s %31s", a, b, d);
    if (!c.foc || n < 1) return 1;
    if (!strcmp(a, "mode")) ctl_focus_set_mode(c.foc, !strcmp(b, "once") ? CTL_FOCUS_ONCE : !strcmp(b, "manual") ? CTL_FOCUS_MANUAL : CTL_FOCUS_CONT);
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
    else if (!strcmp(a, "state")) { ctl_view_state_t s; ctl_view_state(c.view, &s); view_state(NULL, &s); }
    return 1;
}

/* "sh name=N type=jpg|raw q=Q noise=M edge=M bracket=off|0|1|2|3 denoise_sigma=D":
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
    if ((v = field(line, "type"))) r.raw = !strncmp(v, "raw", 3);
    if ((v = field(line, "q"))) r.quality = atoi(v);
    if ((v = field(line, "noise"))) sscanf(v, "%23s", r.noise);
    if ((v = field(line, "edge"))) sscanf(v, "%23s", r.edge);
    if ((v = field(line, "bracket"))) r.bracket = !strncmp(v, "off", 3) ? -1 : atoi(v);
    /* denoise_sigma: the Σ stack's poses per frame (renamed from 'denoise',
     * which read like a noise-reduction mode; the old name still accepted) */
    if ((v = field(line, "denoise_sigma")) || (v = field(line, "denoise"))) r.denoise = atoi(v);
    if (!c.exp || !c.foc) return 1;
    ctl_exp_state(c.exp, &xs);
    ctl_focus_state(c.foc, &fs);
    c.capture_i = 0;
    c.capture_n = ctl_shoot_lines(&r, &xs, c.caps.shutter_max_ns, c.caps.exp_min_ns, &fs, c.capture_q);
    return 1;
}
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
    (void)arg;
    for (;;) {
        ssize_t n = read(c.rd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;                              /* writer closed           */
        for (ssize_t i = 0; i < n; ++i) {
            if (have < sizeof line - 1) line[have++] = buf[i];
            if (buf[i] == '\n') {
                line[have] = '\0';
                on_event(line);
                have = 0;
            }
        }
    }
    return NULL;
}

int ctl_start(ctl_emit_fn emit, ctl_set_event_fd_fn set_event_fd, ctl_engine_line_fn engine_line)
{
    int fds[2];
    pthread_mutexattr_t at;
    if (c.running) return 0;
    if (!emit || !set_event_fd) { fprintf(stderr, "indraw: controller: engine entry points missing\n"); return -1; }
    if (pipe(fds) != 0) { perror("indraw: controller: pipe"); return -1; }
    /* The engine writes events from its camera threads: the write end is
     * non-blocking (a full pipe drops a state line rather than stalling the
     * preview, see emit_event), and the pipe is enlarged to 1 MB where Linux
     * allows (F_SETPIPE_SZ; best effort) so that a reader busy for a second
     * -- a focus command waiting for the lens -- does not fill it. */
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
#ifdef F_SETPIPE_SZ
    fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
#endif
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c.lock, &at);
    pthread_mutexattr_destroy(&at);
    c.rd = fds[0]; c.wr = fds[1];
    c.emit = emit; c.set_event_fd = set_event_fd; c.engine_line = engine_line;
    c.exp = ctl_exp_new(exp_cmd, exp_state, NULL);
    c.foc = ctl_focus_new(foc_cmd, foc_state, foc_pause, foc_spot, NULL);
    c.view = ctl_view_new(view_cmd, view_state, view_ref, NULL);
    if (pthread_create(&c.thread, NULL, reader, NULL) != 0) {
        perror("indraw: controller: thread");
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
