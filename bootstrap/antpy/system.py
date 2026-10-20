"""The Os namespace of stdlib.md (Os.args, Os.env, Os.exit). The other I/O
namespaces (File, Json, Time.now/monotonic) arrive in roadmap step 0.6."""
import os
import sys

from antpy.library import need_int, need_string


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
