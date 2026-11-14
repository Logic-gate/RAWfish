/* Host test of the controller while a capture runs (ctl_engine_busy): the
 * focus and view commands decided meanwhile, on the HAL's reports, reach the
 * engine once the capture is over, in order -- none while it runs. Until
 * 0.3.8 they were dropped (only the exposure was re-applied): a touch's
 * "focus hold", a throttled loupe. Run by scripts/check-host.sh. */
#define _POSIX_C_SOURCE 200809L
#include "../controller.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int engine_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int nae;                         /* "ae" lines through: the reader is done with them */
static char sent[128][96];
static int nsent, busy_now, sent_busy;

static int fake_emit(const char *line)
{
    pthread_mutex_lock(&lock);
    if (!strncmp(line, "ae ", 3)) nae++;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int fake_set_event_fd(int fd) { engine_fd = fd; return 0; }
/* Called under the controller's lock (reader thread or ctl_engine_busy). */
static void fake_engine(const char *cmd)
{
    pthread_mutex_lock(&lock);
    if (busy_now) sent_busy++;
    if (nsent < 128) snprintf(sent[nsent++], sizeof sent[0], "%s", cmd);
    pthread_mutex_unlock(&lock);
}

static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
/* One "ae" event, waited for until the reader has handled it. */
static int ae(int af_state, double focus_real)
{
    char l[160];
    int want, i;
    pthread_mutex_lock(&lock); want = nae + 1; pthread_mutex_unlock(&lock);
    snprintf(l, sizeof l, "ae iso=100 exp_ns=10000000 ae_state=2 af_state=%d focus_real=%.3f\n", af_state, focus_real);
    if (write(engine_fd, l, strlen(l)) < 0) return 0;
    for (i = 0; i < 2000; ++i) {
        int n;
        pthread_mutex_lock(&lock); n = nae; pthread_mutex_unlock(&lock);
        if (n >= want) return 1;
        sleep_ms(1);
    }
    return 0;
}
static void cmd(const char *line) { ctl_lock(); ctl_command(line); ctl_unlock(); }
static void busy(int on) { pthread_mutex_lock(&lock); busy_now = on; pthread_mutex_unlock(&lock); ctl_engine_busy(on); }
/* The index of the first command sent from `from` on that starts with p, or -1. */
static int find(int from, const char *p)
{
    int i;
    for (i = from; i < nsent; ++i) if (!strncmp(sent[i], p, strlen(p))) return i;
    return -1;
}

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

static int start(void)
{
    char caps[256];
    nsent = 0; sent_busy = 0;
    if (ctl_start(fake_emit, fake_set_event_fd, fake_engine) != 0 || engine_fd < 0) return 0;
    snprintf(caps, sizeof caps, "caps iso_min=100 iso_max=3200 exp_min_ns=100000 shutter_max_ns=500000000 "
             "aperture=1.9 zoom_max=8 crop_free=1\n");
    if (write(engine_fd, caps, strlen(caps)) < 0) return 0;
    return ae(0, 1.0);                  /* the caps were read before it */
}

int main(void)
{
    int mark, a;

    /* 1. Manual focus, a touch, the shutter pressed during the search: the
     *    search locks while the photo is taken. */
    if (!start()) { puts("controller busy: start failed"); return 1; }
    cmd("fc mode manual");
    cmd("fc ref 0.4 0.4");
    ae(0, 1.0);                                         /* the CANCEL shows (inactive) */
    ae(3, 1.0);                                         /* the scan is seen */
    busy(1);
    mark = nsent;
    ae(4, 2.0);                                         /* locked, during the capture */
    CHECK(sent_busy == 0, "touch: nothing sent to the engine while the capture runs");
    busy(0);
    CHECK(find(mark, "focus hold") >= 0, "touch: 'focus hold' sent once the capture is over (it was lost)");
    ctl_stop();

    /* 2. A loupe move throttled just before the press (the 80 ms limit),
     *    released by a tick during the capture: sent after it, the last one. */
    if (!start()) { puts("controller busy: start failed"); return 1; }
    cmd("vw loupe 2");
    cmd("vw loupe 3");                                  /* within 80 ms: pending */
    busy(1);
    mark = nsent;
    sleep_ms(100);
    ae(0, 1.0);                                         /* the tick that sends it */
    CHECK(sent_busy == 0, "loupe: nothing sent while the capture runs");
    busy(0);
    a = find(mark, "loupe 3.000");
    CHECK(a >= 0 && find(a + 1, "loupe ") < 0, "loupe: the pending loupe (x3) sent after the capture, once");
    ctl_stop();

    /* 3. The display window goes to the engine when it changes, not at each
     *    view publish (0.3.8: every loupe move and state re-sent it). */
    if (!start()) { puts("controller busy: start failed"); return 1; }
    a = find(0, "viewwin ");
    cmd("vw state");
    cmd("vw state");
    CHECK(a >= 0 && find(a + 1, "viewwin ") < 0, "viewwin: sent once at the caps, not again for an unchanged window");
    cmd("vw zoom 2");
    CHECK(find(a + 1, "viewwin ") >= 0, "viewwin: sent again when the window changes (zoom x2)");
    ctl_stop();

    printf(fails ? "controller busy: FAILED (%d)\n" : "controller busy: OK (held focus and view commands sent after the capture)\n", fails);
    return fails ? 1 : 0;
}
