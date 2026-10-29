/* Concurrency test of the controller (no Android, no Qt), for ThreadSanitizer
 * (and Helgrind): a fake engine thread floods the event pipe with the state
 * lines the real engine sends at 10-30 Hz ("ae", "vfmeter", "meter", "caps"),
 * while the main thread plays the CLI: commands from the interface under
 * ctl_lock(), as sailfish/indraw.c does, which drive the exposure, focus and
 * view controllers that the reader thread updates at the same time. The
 * engine commands the controller sends come back on engine_line (counted).
 * Passes when it runs to the end without a sanitizer report; check-host runs
 * it under -fsanitize=thread (October 2026 audit: the engine itself cannot
 * run under TSan, the controller can). */
#include "../controller.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int engine_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long emitted, engine_lines;

static int fake_emit(const char *line) { (void)line; pthread_mutex_lock(&lock); ++emitted; pthread_mutex_unlock(&lock); return 0; }
static int fake_set_event_fd(int fd) { pthread_mutex_lock(&lock); engine_fd = fd; pthread_mutex_unlock(&lock); return 0; }
static void fake_engine_line(const char *line) { (void)line; pthread_mutex_lock(&lock); ++engine_lines; pthread_mutex_unlock(&lock); }

static void *engine(void *arg)
{
    static const char *const ev[] = {
        "caps proto=1 iso_min=100 iso_max=6400 iso_analog_max=1600 exp_min_ns=20000 exp_max_ns=500000000 shutter_max_ns=500000000 ev_min=-12 ev_max=12 ev_step=1/6 min_focus=10.00 cameras=0\n",
        "ae exposure_ns=10000000 iso=200 ae_state=converged af_state=passive_focused ev=0 lens=2.5 focus_real=0.40 cct=5200 duv=0.002\n",
        "vfmeter mean=0.35 clip=0.00\n",
        "meter mean=0.30 clip_r=0.0 clip_g=0.0 clip_b=0.0\n",
    };
    int fd, i;
    (void)arg;
    pthread_mutex_lock(&lock); fd = engine_fd; pthread_mutex_unlock(&lock);
    for (i = 0; i < 4000; ++i) {
        const char *l = ev[i == 0 ? 0 : 1 + i % 3];
        if (write(fd, l, strlen(l)) < 0) break;
        if (i % 200 == 0) usleep(1000);
    }
    return NULL;
}

int main(void)
{
    static const char *const cmds[] = {
        "xp source raw", "xp touch speed", "xp speed 33333333", "xp touch iso", "xp iso 400", "xp ev 2",
        "xp spot on", "xp spot off", "xp source vf", "xp auto", "fc mode manual", "fc dist 1.0",
        "fc mode cont", "vw zoom 2.00", "vw zoom 1.00", "vw tap 0.5 0.5", "xp pause on", "xp pause off",
    };
    pthread_t t;
    int i;
    if (ctl_start(fake_emit, fake_set_event_fd, fake_engine_line) != 0) { fprintf(stderr, "start failed\n"); return 1; }
    if (pthread_create(&t, NULL, engine, NULL) != 0) return 1;
    for (i = 0; i < 2000; ++i) {                 /* the CLI: one command at a time, under the lock */
        ctl_lock();
        ctl_command(cmds[i % (int)(sizeof cmds / sizeof cmds[0])]);
        ctl_unlock();
    }
    pthread_join(t, NULL);
    ctl_stop();
    pthread_mutex_lock(&lock);
    printf("controller stress: OK (%ld lines emitted, %ld engine commands, two threads)\n", emitted, engine_lines);
    pthread_mutex_unlock(&lock);
    return 0;
}
