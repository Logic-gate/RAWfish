# scripts/lockcheck.awk -- run by check-host.sh on android/indraw.c. A
# straight-line reading of each function (no control flow analysis), for the
# engine's lock contract (the header of android/indraw.c, docs/ARCHITECTURE.md):
#  1. g.lock is not recursive: taken while already held in the same function
#     is a self-deadlock (0.47.15's bug). A lock legitimately re-taken in
#     another branch carries "lockcheck: branch".
#  2. The only nesting order is g.lock -> out_lock: g.lock taken while
#     out_lock is held is refused (a deadlock waiting to happen against a
#     thread that holds g.lock and wants out_lock).
#  3. g.stream_fd and g.event_fd are read and written under out_lock only
#     (except in g's initializer). A line that is safe for another reason
#     says why with "lockcheck: fd".
# Since 0.1.3 rules 2 and 3 (October 2026 audit: the contract was written
# down but not checked).
/^[A-Za-z_].*\(/ && !/;[[:space:]]*$/ { fn = $0; depth = 0; out = 0 }
/pthread_mutex_lock\(&g\.lock\)/ {
    if (depth > 0 && $0 !~ /lockcheck: branch/) { printf "%s:%d: g.lock taken while held (in %s)\n", FILENAME, FNR, fn; bad = 1 }
    if (out > 0) { printf "%s:%d: g.lock taken while out_lock is held: the order is g.lock -> out_lock (in %s)\n", FILENAME, FNR, fn; bad = 1 }
    depth++
}
/pthread_mutex_unlock\(&g\.lock\)/ { if (depth > 0) depth-- }
/pthread_mutex_lock\(&g\.out_lock\)/ { out++ }
/pthread_mutex_unlock\(&g\.out_lock\)/ { if (out > 0) out-- }
/g\.(stream_fd|event_fd)/ && !/^\}[[:space:]]*g[[:space:]]*=/ && !/lockcheck: fd/ && !/^[[:space:]]*\/?\*/ {
    if (out == 0) { printf "%s:%d: g.stream_fd / g.event_fd used without out_lock (in %s)\n", FILENAME, FNR, fn; bad = 1 }
}
END { exit bad }
