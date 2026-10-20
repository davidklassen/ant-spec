"""The Os and Time namespaces of stdlib.md."""
import os
import sys
import time

from antpy.values import VOID
from antpy.library import need_int, need_string


# ----------------------------------------------------------------------------- Os

def os_args(interp, frame):
    """A fresh array each call (section 11: a function returns a new value),
    holding only the arguments after the entry file: no program name (G39)."""
    return list(interp.args)


def os_env(name):
    return os.environ.get(need_string(name))      # None is Ant's null


def os_exit(status):
    """Ends the process at once, from any goroutine, without running deferred
    blocks (G39). Flushes first: print writes to a block-buffered pipe."""
    need_int(status)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(status & 0xFF)


OS = {
    "args": ([], os_args, True),
    "env": (["name"], os_env),
    "exit": (["status"], os_exit),
}


# ----------------------------------------------------------------------------- Time

NS_PER_MS = 1_000_000
MAX_NAP_NS = 3600 * 1_000_000_000     # time.sleep overflows at about 2**63 ns; nap an hour at most


def time_now():
    """Milliseconds since the Unix epoch."""
    return time.time_ns() // NS_PER_MS


def time_monotonic():
    """Milliseconds from an arbitrary origin; never decreases."""
    return time.monotonic_ns() // NS_PER_MS


def time_sleep(ms):
    """Blocks only the caller: time.sleep releases the GIL. A count of zero
    or less returns at once.

    Sleeps until a deadline on the clock `Time.monotonic` reads, in whole
    nanoseconds, so two `Time.monotonic()` readings around `Time.sleep(ms)`
    differ by at least `ms` even after the millisecond truncation: a single
    time.sleep(ms / 1000) can wake a few microseconds early as that clock
    measures it, and 49.998 ms would read as 49."""
    need_int(ms)
    deadline = time.monotonic_ns() + ms * NS_PER_MS
    while True:
        remaining = deadline - time.monotonic_ns()
        if remaining <= 0:
            return VOID
        time.sleep(min(remaining, MAX_NAP_NS) / 1e9)


TIME = {
    "now": ([], time_now),
    "sleep": (["ms"], time_sleep),
    "monotonic": ([], time_monotonic),
}
