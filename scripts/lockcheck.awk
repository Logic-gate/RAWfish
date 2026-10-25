# scripts/lockcheck.awk -- run by check-host.sh on android/indraw.c.
# A non-reentrant g.lock taken twice in a row within one function, in a
# straight-line reading: a self-deadlock (0.47.15's bug). A lock that is
# legitimately re-taken in another branch carries "lockcheck: branch".
/^[A-Za-z_].*\(/ && !/;[[:space:]]*$/ { fn = $0; depth = 0 }
/pthread_mutex_lock\(&g\.lock\)/ {
    if (depth > 0 && $0 !~ /lockcheck: branch/) { printf "%s:%d: g.lock taken while held (in %s)\n", FILENAME, FNR, fn; bad = 1 }
    depth++
}
/pthread_mutex_unlock\(&g\.lock\)/ { if (depth > 0) depth-- }
END { exit bad }
