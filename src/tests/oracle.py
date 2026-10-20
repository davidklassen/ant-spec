#!/usr/bin/env python3
"""Fake implementation that obeys each test's own header. See README.md.

    oracle.py check <file>
    oracle.py run <file> [args...]
    oracle.py run-unchecked <file> [args...]

ORACLE_SABOTAGE=1 makes every answer wrong, which proves that the runner notices.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run as runner  # noqa: E402


def main():
    mode, path = sys.argv[1], os.path.abspath(sys.argv[2])
    sabotage = os.environ.get("ORACLE_SABOTAGE") == "1"
    tier = os.path.relpath(path, HERE).split(os.sep)[0]
    t = runner.Test(tier, path, HERE)
    t.files = runner.files_under(t.dir) if t.is_dir else [path]
    runner.load(t)

    if mode == "check":
        reject = t.tier in ("check", "unchecked")
        if sabotage:
            reject = not reject
        if not reject:
            return 0
        markers = sorted(t.errors.items())
        if not markers:
            sys.stderr.write(f"{path}:1:1: rejected\n")
        for (base, line), phrase in markers:
            sys.stderr.write(f"{os.path.join(t.dir, base)}:{line}:1: {phrase or 'error'}\n")
        return 1

    if mode == "run" and t.tier in ("check", "unchecked"):
        sys.stderr.write("oracle: a rejected program was run with checking on\n")
        return 1
    panicking = t.tier == "panic" or (t.tier == "unchecked" and bool(t.get("panic")))
    out = t.expected_stdout
    if sabotage:
        out += "sabotage\n"
    sys.stdout.write(out)
    if t.get("stderr"):
        sys.stderr.write(t.get("stderr") + "\n")
    if panicking:
        sys.stderr.write(f"panic: {t.get('panic')}\n  at main\n")
        return 0 if sabotage else 2
    return int(t.get("exit") or 0)


if __name__ == "__main__":
    sys.exit(main())
