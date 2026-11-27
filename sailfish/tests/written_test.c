/* Host test of the controller's hold of "written" events (ctl_hold_written,
 * ctl_release_written; the lens fix's camera JPEG, docs/LENS-DISTORTION.md):
 * a fake engine writes event lines into the controller's pipe, the reader
 * thread handles them, the fake interface records what reaches it. Each
 * step ends with a marker line and waits until the marker has come
 * through: every line before it has been handled (in order) by then.
 * Run by scripts/check-host.sh. */
#define _POSIX_C_SOURCE 200809L
#include "../controller.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int engine_fd = -1, fails, marks;
static char got[128][160];
static int ngot;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAIL: "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int fake_emit(const char *line)
{
    pthread_mutex_lock(&lock);
    if (!strncmp(line, "mark ", 5)) marks++;
    else if (ngot < 128) snprintf(got[ngot++], sizeof got[0], "%s", line);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int fake_set_event_fd(int fd) { engine_fd = fd; return 0; }

static void engine_says(const char *line)
{
    if (write(engine_fd, line, strlen(line)) < 0) { perror("write"); exit(1); }
}
/* Waits until everything written so far has been handled. */
static void settle(void)
{
    static int sent;
    char m[32];
    int i, n;
    snprintf(m, sizeof m, "mark %d\n", ++sent);
    engine_says(m);
    for (i = 0; i < 2000; ++i) {                       /* 2 s at most */
        struct timespec ts = { 0, 1000000 };
        pthread_mutex_lock(&lock); n = marks; pthread_mutex_unlock(&lock);
        if (n >= sent) return;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "the reader did not handle the lines\n");
    exit(1);
}
/* How many times `path`'s written event reached the interface. */
static int seen(const char *path)
{
    int i, n = 0;
    char want[200];
    snprintf(want, sizeof want, "written kind=jpeg path=%s\n", path);
    pthread_mutex_lock(&lock);
    for (i = 0; i < ngot; ++i) if (!strcmp(got[i], want)) n++;
    pthread_mutex_unlock(&lock);
    return n;
}
static void written(const char *path)
{
    char ev[200];
    snprintf(ev, sizeof ev, "written kind=jpeg path=%s\n", path);
    engine_says(ev);
}

int main(void)
{
    char p[16][32];
    int i;
    printf("written hold:\n");
    if (ctl_start(fake_emit, fake_set_event_fd, NULL) != 0 || engine_fd < 0) { fprintf(stderr, "start failed\n"); return 1; }

    /* 1. hold, the event, then the release (the usual order: the file is
     *    written, then corrected) */
    ctl_hold_written("/p/a.jpg");
    written("/p/a.jpg"); settle();
    CHECK(seen("/p/a.jpg") == 0, "held: the event waits while the file is corrected");
    ctl_release_written("/p/a.jpg");
    CHECK(seen("/p/a.jpg") == 1, "released: the event goes, once");

    /* 2. hold, the release, then the event (the reader thread late) */
    ctl_hold_written("/p/b.jpg");
    ctl_release_written("/p/b.jpg");
    written("/p/b.jpg"); settle();
    CHECK(seen("/p/b.jpg") == 1, "released before its event came: the event goes straight through, once");
    written("/p/b.jpg"); settle();
    CHECK(seen("/p/b.jpg") == 2, "and the slot is free again: a later event of that path is not held");

    /* 3. a release of a path never held; an event never held */
    ctl_release_written("/p/never.jpg");
    written("/p/c.jpg"); settle();
    CHECK(seen("/p/c.jpg") == 1, "a path never held: its event passes, the stray release changed nothing");

    /* a path that only starts like a held one is not held */
    ctl_hold_written("/p/d.jpg");
    written("/p/d.jpg2"); settle();
    CHECK(seen("/p/d.jpg2") == 1, "a longer path with the same start is not held");
    ctl_release_written("/p/d.jpg");                  /* no event came: the slot is kept as released */
    written("/p/d.jpg"); settle();
    CHECK(seen("/p/d.jpg") == 1, "(its own event, after the release: passes)");

    /* 4. the 8 slots full */
    for (i = 0; i < 9; ++i) snprintf(p[i], sizeof p[i], "/p/full%d.jpg", i);
    for (i = 0; i < 9; ++i) ctl_hold_written(p[i]);
    for (i = 0; i < 9; ++i) written(p[i]);
    settle();
    CHECK(seen(p[8]) == 1, "a 9th hold with the 8 slots taken: not held, its event passes (never lost)");
    for (i = 0; i < 8 && !seen(p[i]); ++i) {}
    CHECK(i == 8, "the 8 held ones wait");
    for (i = 0; i < 8; ++i) ctl_release_written(p[i]);
    for (i = 0; i < 8 && seen(p[i]) == 1; ++i) {}
    CHECK(i == 8, "released, each goes once");
    /* a full table whose released slots never got their event: reused */
    for (i = 0; i < 8; ++i) { snprintf(p[i], sizeof p[i], "/p/lost%d.jpg", i); ctl_hold_written(p[i]); ctl_release_written(p[i]); }
    ctl_hold_written("/p/new.jpg");
    written("/p/new.jpg"); settle();
    CHECK(seen("/p/new.jpg") == 0, "8 slots released and never used: one is taken again, the new file held");
    ctl_release_written("/p/new.jpg");
    CHECK(seen("/p/new.jpg") == 1, "and released");

    ctl_stop();
    printf(fails ? "written hold: %d FAILED\n" : "written hold: all passed\n", fails);
    return fails != 0;
}
