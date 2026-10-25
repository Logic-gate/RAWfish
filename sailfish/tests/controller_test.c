/* Host test of the controller layer (no Android, no Qt): a fake engine writes
 * event lines into the pipe the controller gave it; the controller must hand
 * every line, whole and in order, to the fake interface. Run by
 * scripts/check-host.sh. */
#include "../controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static int engine_fd = -1;
static char got[64][128];
static int ngot = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int nstates = 0;
static int fake_emit(const char *line)
{
    pthread_mutex_lock(&lock);
    /* the controller's own state lines are counted apart: the engine's
     * events must still pass through whole and in order */
    if (!strncmp(line, "xpstate ", 8) || !strncmp(line, "fcstate ", 8) || !strncmp(line, "vwstate ", 8)) nstates++;
    else if (ngot < 64) snprintf(got[ngot++], sizeof got[0], "%s", line);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int fake_set_event_fd(int fd) { engine_fd = fd; return 0; }

int main(void)
{
    const char *lines[] = { "caps a=1\n", "ae iso=100 exp_ns=1000\n", "written kind=jpeg path=/x.jpg\n" };
    int i, ok = 1;
    if (ctl_start(fake_emit, fake_set_event_fd, NULL) != 0 || engine_fd < 0) { fprintf(stderr, "start failed\n"); return 1; }
    /* the engine writes, in pieces and with two lines in one write */
    if (write(engine_fd, lines[0], 4) < 0 || write(engine_fd, lines[0] + 4, strlen(lines[0]) - 4) < 0) return 1;
    {
        char two[256];
        snprintf(two, sizeof two, "%s%s", lines[1], lines[2]);
        if (write(engine_fd, two, strlen(two)) < 0) return 1;
    }
    ctl_stop();                         /* closes the pipe, joins the reader: all delivered */
    if (ngot != 3) { fprintf(stderr, "got %d lines, expected 3\n", ngot); ok = 0; }
    for (i = 0; i < 3 && i < ngot; ++i)
        if (strcmp(got[i], lines[i])) { fprintf(stderr, "line %d: got '%s' expected '%s'\n", i, got[i], lines[i]); ok = 0; }
    if (engine_fd != -1) { fprintf(stderr, "engine not detached after stop\n"); ok = 0; }
    if (nstates < 1) { fprintf(stderr, "no xpstate from the 'ae' event\n"); ok = 0; }
    puts(ok ? "controller: OK (3 events passed through in order, controller states emitted)" : "controller: FAILED");
    return ok ? 0 : 1;
}
