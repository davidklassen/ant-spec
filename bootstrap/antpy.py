#!/usr/bin/env python3
"""antpy: the bootstrap interpreter for Ant (ROADMAP.md, M0).

    python3 -I bootstrap/antpy.py run [--unchecked] FILE [ARGS...]

Runs the module at FILE as the entry module. `--unchecked` is accepted and
identical, since the bootstrap has no checker. Exit status: 0 when `main`
returns, 2 on a panic (`panic: <message>` and a trace on stderr), 1 on a load
error (`path:line:col: message` on stderr).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
# Under `python3 -I` neither the script directory nor tools/ is on sys.path.
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(REPO, "tools"))

USAGE = "usage: antpy.py run [--unchecked] FILE [ARGS...]"


def main(argv):
    if len(argv) < 2 or argv[0] != "run":
        sys.stderr.write(USAGE + "\n")
        return 2
    rest = argv[1:]
    if rest[0] == "--unchecked":
        rest = rest[1:]
    if not rest:
        sys.stderr.write(USAGE + "\n")
        return 2
    from antpy.interp import run_program
    return run_program(rest[0], rest[1:])


if __name__ == "__main__":
    os._exit(main(sys.argv[1:]))
