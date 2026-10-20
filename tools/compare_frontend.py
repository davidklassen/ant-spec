#!/usr/bin/env python3
"""Compare antc's frontend with the reference parser, tools/antparse.py.

    python3 -I tools/compare_frontend.py lex   (--sample | --all | PATHS...) [-j N] [-v]
    python3 -I tools/compare_frontend.py parse (--sample | --all | PATHS...) [-j N] [-v]

For every selected file the reference side is rendered in a canonical text
form and compared with what `antc lex FILE` or `antc parse --dump FILE` prints
when antc runs under the bootstrap interpreter. A file the reference rejects
must be rejected by antc at the same line:col with the same message (the
reference's `unexpected character {c!r}` is Python's repr of the character;
antc writes '<c>', and the two are compared with that normalised); every other
file must produce the same dump. In parse mode a file under tests/err must be
rejected by both sides. The last line is always `<N> files, <M> failures`, and
the exit status is 0 iff M == 0.

Selection: `--sample` is tests/ok/*, tests/err/* and every 14th file of the
sorted suite under src/tests; `--all` is every .ant file in the repository;
PATHS are files, or directories expanded to their .ant files.

Lex dump, one line per token, in order:

    <line>:<col> <KIND>            when the value is empty (EOF)
    <line>:<col> <KIND> <value>    otherwise
      part <line>:<col> <text>     one per ${...} body of a TEMPLATE token

The value and the part text are escaped: `\\` for a backslash, `\\n` for a
newline, `\\r` for a carriage return, nothing else.

AST dump, an S-expression of the reference's tuple tree:

    None            nil
    string          quoted, with `\"` escaped on top of the lex escapes above
    list            [a b ...]
    tuple           (head b ...) where the head is printed bare when it is a
                    string: the node kind of a tagged node, and the name that
                    opens the untagged tuples (import items and destructure
                    fields `(name alias)`, type parameters `(name bound)`,
                    parameters `(name type default)`, record-type fields
                    `(name type)`); any other first element is rendered
                    normally, as in a signature `([...] ret)`

The top-level File is the only multi-line node: `(File`, `  [`, one import per
line indented four spaces, `  ]`, `  [`, one declaration per line, `  ]`, `)`.
"""
import argparse
import ast
import concurrent.futures
import difflib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import antparse  # noqa: E402

ANTC = [sys.executable, "-I", "bootstrap/antpy.py", "run", "src/antc/main.ant"]
ANTC_ARGS = {"lex": ["lex"], "parse": ["parse", "--dump"]}
DIAGNOSTIC = re.compile(r"^(.*):(\d+):(\d+): (.*)$")


# ----------------------------------------------------------------------------- selection

def ant_files(directory):
    """Every .ant file under directory, sorted, as paths relative to ROOT."""
    found = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, directory)):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for fn in filenames:
            if fn.endswith(".ant"):
                found.append(os.path.relpath(os.path.join(dirpath, fn), ROOT))
    return sorted(found)


def select(args):
    if args.sample:
        suite = ant_files("src/tests")
        return ant_files("tests/ok") + ant_files("tests/err") + suite[::14]
    if args.all:
        return ant_files(".")
    files = []
    for p in args.paths:
        full = os.path.abspath(p)
        if os.path.isdir(full):
            files.extend(ant_files(os.path.relpath(full, ROOT)))
        else:
            files.append(os.path.relpath(full, ROOT))
    return files


# ----------------------------------------------------------------------------- the two sides

def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def lex_dump(toks):
    lines = []
    for t in toks:
        head = f"{t.line}:{t.col} {t.kind}"
        lines.append(head if t.value == "" else f"{head} {esc(t.value)}")
        for line, col, text in t.parts or []:
            lines.append(f"  part {line}:{col} {esc(text)}")
    return "".join(line + "\n" for line in lines)


def quote(s):
    return '"' + esc(s).replace('"', '\\"') + '"'


def sexpr(node):
    if node is None:
        return "nil"
    if isinstance(node, str):
        return quote(node)
    if isinstance(node, list):
        return "[" + " ".join(sexpr(x) for x in node) + "]"
    head = node[0] if isinstance(node[0], str) else sexpr(node[0])
    return "(" + " ".join([head] + [sexpr(x) for x in node[1:]]) + ")"


def ast_dump(tree):
    _, imports, decls = tree
    lines = ["(File", "  ["] + ["    " + sexpr(i) for i in imports] + ["  ]", "  ["] \
        + ["    " + sexpr(d) for d in decls] + ["  ]", ")"]
    return "".join(line + "\n" for line in lines)


def reference(mode, src):
    """("ok", dump) or ("error", line, col, message) from the reference parser."""
    try:
        if mode == "lex":
            return ("ok", lex_dump(antparse.Lexer(src).tokenize()))
        return ("ok", ast_dump(antparse.parse(src)))
    except antparse.ParseError as e:
        return ("error", e.line, e.col, e.msg)


def antc(mode, path):
    """("ok", stdout), ("error", line, col, message) or ("crash", what, stderr) from antc."""
    try:
        p = subprocess.run(ANTC + ANTC_ARGS[mode] + [path], cwd=ROOT, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return ("crash", "timed out after 120 s", "")
    out = p.stdout.decode("utf-8", errors="replace")
    err = p.stderr.decode("utf-8", errors="replace")
    if p.returncode == 0 and err == "":
        return ("ok", out)
    if p.returncode == 1 and out == "":
        m = DIAGNOSTIC.match(err.split("\n", 1)[0])
        if m:
            return ("error", int(m.group(2)), int(m.group(3)), m.group(4))
    return ("crash", f"exit {p.returncode}", err if err else out)


# ----------------------------------------------------------------------------- comparison

def describe(result):
    if result[0] == "ok":
        return "accepted"
    if result[0] == "error":
        return f"error at {result[1]}:{result[2]}: {result[3]}"
    return f"crashed ({result[1]})"


UNEXPECTED = "unexpected character "


def same_message(ref_msg, got_msg):
    """Whether antc reproduced the reference's message. The reference writes
    `unexpected character {c!r}` with Python's repr of the character, which antc
    renders as '<c>'; every other message must match verbatim."""
    if ref_msg.startswith(UNEXPECTED):
        ref_msg = f"{UNEXPECTED}'{ast.literal_eval(ref_msg[len(UNEXPECTED):])}'"
    return ref_msg == got_msg


def compare(mode, path):
    """Returns (path, ok, detail lines)."""
    with open(os.path.join(ROOT, path), "rb") as f:
        src = f.read().decode("utf-8", errors="replace")
    ref = reference(mode, src)
    got = antc(mode, path)
    if mode == "parse" and path.startswith("tests/err/"):
        accepted = [side for side, result in (("reference", ref), ("antc", got)) if result[0] == "ok"]
        if accepted:
            return (path, False, [f"tests/err file accepted by {side}" for side in accepted]
                    + [f"reference: {describe(ref)}", f"antc:      {describe(got)}"])
    if ref[0] == "ok" and got[0] == "ok":
        if ref[1] == got[1]:
            return (path, True, [])
        diff = difflib.unified_diff(ref[1].splitlines(), got[1].splitlines(),
                                    "reference", "antc", lineterm="")
        return (path, False, ["dumps differ"] + list(diff)[:30])
    detail = [f"reference: {describe(ref)}", f"antc:      {describe(got)}"]
    if ref[0] == "error" and got[0] == "error":
        same = (ref[1], ref[2]) == (got[1], got[2]) and same_message(ref[3], got[3])
        return (path, same, detail)
    if got[0] == "crash":
        detail += ["  " + line for line in got[2].splitlines()[:10]]
    return (path, False, detail)


def main():
    ap = argparse.ArgumentParser(description="compare antc's frontend with tools/antparse.py")
    ap.add_argument("mode", choices=["lex", "parse"])
    ap.add_argument("--sample", action="store_true", help="tests/ok, tests/err and every 14th suite file")
    ap.add_argument("--all", action="store_true", help="every .ant file in the repository")
    ap.add_argument("paths", nargs="*", help="files, or directories expanded to their .ant files")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, help="parallel antc runs")
    ap.add_argument("-v", "--verbose", action="store_true", help="also print the files that agree")
    args = ap.parse_args()
    if not (args.sample or args.all or args.paths):
        ap.error("give --sample, --all or at least one path")

    files = select(args)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for path, ok, detail in pool.map(lambda p: compare(args.mode, p), files):
            if ok:
                if args.verbose:
                    print(f"ok   {path}")
                    for line in detail:
                        print("     " + line)
                continue
            failures += 1
            print(f"FAIL {path}")
            for line in detail:
                print("     " + line)
    print(f"{len(files)} files, {failures} failures")
    sys.exit(0 if failures == 0 else 1)


if __name__ == "__main__":
    main()
