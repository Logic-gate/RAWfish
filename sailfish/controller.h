/*
 * controller.h -- the controller layer of indRAW (docs/ARCHITECTURE.md).
 *
 * Sits in the `rawfish-ir-cli` process between the engine (librawfish-ir.so, Android
 * side) and the interface (the app, on stdin/stdout). The engine sends its
 * state events (caps, ae, meter, captured, ...) to the controller through a
 * pipe; the controller reads them on its own thread, keeps the camera's
 * behaviour, and forwards what the interface must see with indraw_emit(),
 * which writes to the stream in order with the frames.
 *
 * Step 1 (this file): the pipe and the thread; every event is passed through
 * unchanged. The behaviour moves here piece by piece afterwards.
 *
 * Rules: calls go downwards only (the controller calls the engine, never
 * the other way round); the engine's Android threads never run controller
 * code (they only write() to the pipe).
 */
#ifndef INDRAW_CONTROLLER_H
#define INDRAW_CONTROLLER_H

#include <stddef.h>

/* The engine entry points the controller uses, resolved by the CLI. */
typedef int (*ctl_emit_fn)(const char *line);
typedef int (*ctl_set_event_fd_fn)(int fd);
/* Runs one engine command line ("exposure ...", "ev N", "meter ...") the
 * way the CLI runs a line from stdin; provided by the CLI. */
typedef void (*ctl_engine_line_fn)(const char *line);

/* Starts the controller: creates the event pipe, hands its write end to the
 * engine (set_event_fd) and starts the reader thread. Returns 0, or -1 with
 * a message on stderr (the engine then keeps writing events to the stream
 * itself, as without a controller). */
int ctl_start(ctl_emit_fn emit, ctl_set_event_fd_fn set_event_fd, ctl_engine_line_fn engine_line);

/* One lock for everything that touches the engine or the controller's
 * state, recursive: the CLI takes it around each stdin command, the
 * controller's reader thread around each event. Engine calls from two
 * threads at once are not allowed; this is what prevents them. */
void ctl_lock(void);
void ctl_unlock(void);

/* The CLI is about to run a long engine command (a photo, a burst, a stack)
 * WITHOUT holding the lock, so events keep flowing meanwhile: the controller
 * then defers its own engine commands, and re-applies its state when the
 * command is over (busy 0). */
void ctl_engine_busy(int busy);

/* Capture lines a controller command produced ("sh ...", step 6): the CLI
 * pops and runs them itself, as long commands (without the lock, the
 * controller told busy), after ctl_command() returned. Returns 1 and copies
 * the next line into buf, 0 when there is none. */
int ctl_pop_capture(char *buf, size_t n);

/* An interface command for the controller (step 3: "xp ..."). Returns 1 if
 * it was one (handled), 0 if the line is not a controller command. Call
 * under ctl_lock(). */
int ctl_command(const char *line);

/* Detaches from the engine, stops the thread, closes the pipe. Safe to call
 * when not started. */
void ctl_stop(void);

#endif
