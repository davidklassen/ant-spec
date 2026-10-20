#!/usr/bin/env python3
"""Runner for the Ant semantic test suite. See README.md.

    python3 -I src/tests/run.py                       parse mode: reference parser + header validation
    python3 -I src/tests/run.py --check CMD --run CMD --run-unchecked CMD
    python3 -I src/tests/run.py --oracle [--sabotage]  drive the suite through oracle.py
"""
import argparse
import difflib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import antparse  # noqa: E402

TIERS = ("run", "panic", "check", "unchecked", "programs", "io")
TIER_WORD = {"run": "run", "panic": "panic", "check": "check",
             "unchecked": "unchecked", "programs": "program", "io": "io"}
KNOWN_KEYS = {"spec", "source", "gap", "args", "env", "panic", "stderr", "exit", "timeout"}
ERROR_RE = re.compile(r'//\s*ERROR(?:\s+"([^"]*)")?\s*$')
REPORT_RE = re.compile(r'^(?P<path>[^:\n]+?):(?P<line>\d+)(?::(?P<col>\d+))?:\s*(?P<msg>.*)$')
DIRECTIVE_RE = re.compile(r'^\s?([a-z][a-z-]*):(?:\s(.*))?$')
GAP_ID_RE = re.compile(r'^## (G\d+)\b', re.M)
GAP_REF_RE = re.compile(r'^(G\d+|GNEW-[a-z0-9-]+)$')
CHECKLIST_ITEM_RE = re.compile(r'`([^`]+)`')


class Test:
    def __init__(self, tier, path, root):
        self.tier = tier
        self.path = path
        self.dir = os.path.dirname(path)
        self.is_dir = os.path.basename(path) == "main.ant"
        self.rel = os.path.relpath(self.dir if self.is_dir else path, root) + ("/" if self.is_dir else "")
        self.word = None
        self.directives = {}
        self.output = None
        self.errors = {}      # (basename, line) -> phrase or None
        self.files = []
        self.sources = {}

    def get(self, key, default=None):
        vals = self.directives.get(key)
        return vals[0] if vals else default

    @property
    def args(self):
        a = self.get("args")
        return shlex.split(a) if a else []

    @property
    def gaps(self):
        out = []
        for v in self.directives.get("gap", []):
            out += [g.strip() for g in v.split(",") if g.strip()]
        return out

    @property
    def expected_stdout(self):
        if not self.output:
            return ""
        return "\n".join(self.output) + "\n"

    timeout_override = None

    @property
    def timeout(self):
        if Test.timeout_override:
            return Test.timeout_override
        t = self.get("timeout")
        return float(t) if t else 60.0


# ----------------------------------------------------------------------------- loading

def parse_header(test, src):
    in_output = False
    for idx, ln in enumerate(src.split("\n")):
        if not ln.startswith("//"):
            break
        body = ln[2:]
        if in_output:
            if body.startswith("  "):
                test.output.append(body[2:])
                continue
            if body.strip() == "":
                test.output.append("")
                continue
            in_output = False
        if idx == 0:
            test.word = body.strip()
            continue
        if body.strip() == "output:":
            test.output = []
            in_output = True
            continue
        m = DIRECTIVE_RE.match(body)
        if m:
            test.directives.setdefault(m.group(1), []).append((m.group(2) or "").strip())


def load(test):
    for f in test.files:
        with open(f, encoding="utf-8") as fh:
            test.sources[f] = fh.read()
    parse_header(test, test.sources[test.path])
    for f in test.files:
        for n, ln in enumerate(test.sources[f].split("\n"), 1):
            m = ERROR_RE.search(ln)
            if m:
                test.errors[(os.path.basename(f), n)] = m.group(1)


def files_under(d):
    out = []
    for dp, _, fns in os.walk(d):
        out += [os.path.join(dp, f) for f in fns if f.endswith(".ant")]
    return sorted(out)


def discover(root, tiers, filt, exclude=None):
    tests = []
    for tier in tiers:
        base = os.path.join(root, tier)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            if "main.ant" in filenames:
                t = Test(tier, os.path.join(dirpath, "main.ant"), root)
                t.files = files_under(dirpath)
                tests.append(t)
                dirnames[:] = []
                continue
            for fn in sorted(filenames):
                if fn.endswith(".ant"):
                    t = Test(tier, os.path.join(dirpath, fn), root)
                    t.files = [t.path]
                    tests.append(t)
    if filt:
        tests = [t for t in tests if any(f in t.rel for f in filt)]
    if exclude:
        tests = [t for t in tests if not any(x in t.rel for x in exclude)]
    for t in tests:
        load(t)
    return sorted(tests, key=lambda t: t.rel)


def gap_ids_in(path):
    """Ids declared in GAPS.md, and the subset marked '(untested)' in their heading."""
    if not os.path.isfile(path):
        return set(), set()
    ids, untested = set(), set()
    with open(path, encoding="utf-8") as fh:
        for ln in fh:
            m = re.match(r'^## (G\d+)\b(.*)$', ln)
            if m:
                ids.add(m.group(1))
                if "(untested)" in m.group(2):
                    untested.add(m.group(1))
    return ids, untested


# ----------------------------------------------------------------------------- validation

def validate(test, gap_ids, strict):
    problems, warnings = [], []
    want = TIER_WORD[test.tier]
    if test.word != want:
        problems.append(f"first line must be '// {want}', found '// {test.word}'")
    if not test.get("spec"):
        problems.append("missing '// spec:'")
    for k in test.directives:
        if k not in KNOWN_KEYS:
            problems.append(f"unknown directive '{k}:'")
    if test.tier == "check" and not test.errors:
        problems.append("no // ERROR marker")
    if test.tier == "panic" and not test.get("panic"):
        problems.append("missing '// panic:'")
    if test.tier in ("run", "programs", "io") and test.get("panic"):
        problems.append("'panic:' is only for the panic and unchecked tiers")
    if test.tier in ("run", "panic", "check") and test.get("exit") is not None:
        problems.append("'exit:' is only for program, io, and unchecked tests")
    if test.tier == "check" and (test.output or test.get("stderr") or test.get("args")):
        problems.append("check tests take no output:, stderr:, or args:")
    if test.tier not in ("check", "unchecked") and test.errors:
        problems.append("// ERROR markers belong to the check and unchecked tiers")
    e = test.get("exit")
    if e is not None and not e.isdigit():
        problems.append("'exit:' must be an integer")
    for g in test.gaps:
        if not GAP_REF_RE.match(g):
            problems.append(f"bad gap id {g!r}")
        elif g not in gap_ids:
            (problems if strict else warnings).append(f"gap {g} is not in GAPS.md")
    return problems, warnings


def parse_check(test):
    for f in test.files:
        try:
            antparse.parse(test.sources[f])
        except antparse.ParseError as e:
            return f"{os.path.relpath(f, HERE)}: {e}"
    return None


# ----------------------------------------------------------------------------- running

class Impl:
    def __init__(self, check, run, run_unchecked, env=None):
        self.check, self.run, self.run_unchecked = check, run, run_unchecked
        self.env = env or {}

    def invoke(self, template, test, args, tmpdir=None):
        cmd = template.replace("{file}", shlex.quote(test.path))
        cmd = cmd.replace("{args}", " ".join(shlex.quote(a) for a in args))
        cmd = cmd.replace("{dir}", shlex.quote(tmpdir or ""))
        env = dict(os.environ)
        env.update(self.env)
        for pair in shlex.split(test.get("env") or ""):
            k, _, v = pair.partition("=")
            env[k] = v
        try:
            p = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                               timeout=test.timeout, cwd=REPO, env=env)
        except subprocess.TimeoutExpired:
            return None, "", "timeout"
        return p.returncode, p.stdout, p.stderr


def first_line(s):
    s = s.strip()
    return s.split("\n", 1)[0][:200] if s else ""


def diff(want, got):
    d = difflib.unified_diff(want.splitlines(), got.splitlines(), "expected", "actual", lineterm="", n=1)
    return "\n".join("      " + ln for ln in list(d)[:40])


def match_reports(test, stderr):
    if not test.errors:
        return []
    reported = []
    for ln in stderr.splitlines():
        m = REPORT_RE.match(ln.strip())
        if m:
            reported.append((os.path.basename(m.group("path")), int(m.group("line")), m.group("msg")))
    single = len(test.files) == 1
    fails = []
    for (base, line), phrase in sorted(test.errors.items()):
        hits = [r for r in reported if r[1] == line and (single or r[0] == base)]
        if not hits:
            fails.append(f"no error reported at {base}:{line}" + (f" (want {phrase!r})" if phrase else ""))
        elif phrase and not any(phrase.lower() in r[2].lower() for r in hits):
            fails.append(f"{base}:{line}: reported {hits[0][2]!r}, want substring {phrase!r}")
    for base, line, msg in reported:
        if not any(l == line and (single or b == base) for (b, l) in test.errors):
            fails.append(f"unexpected error at {base}:{line}: {msg}")
    return fails


def expect_run(test, code, out, err, panicking):
    if code is None:
        return ["timed out"]
    fails = []
    if panicking:
        if code == 0:
            fails.append("expected a panic but the exit status was 0")
        phrase = test.get("panic")
        if phrase and phrase not in err:
            fails.append(f"stderr lacks {phrase!r}: {first_line(err)!r}")
    else:
        want = int(test.get("exit") or 0)
        if code != want:
            fails.append(f"exit status {code}, want {want}: {first_line(err)!r}")
    s = test.get("stderr")
    if s and s not in err:
        fails.append(f"stderr lacks {s!r}")
    got = out.replace("\r\n", "\n")
    if got != test.expected_stdout:
        fails.append("stdout differs:\n" + diff(test.expected_stdout, got))
    return fails


def run_test(test, impl, consistency):
    fails = []
    args = test.args
    tmpdir = None
    if test.tier == "io":
        tmpdir = tempfile.mkdtemp(prefix="ant-io-")
        args = [tmpdir] + args
    try:
        if test.tier in ("check", "unchecked"):
            if impl.check:
                code, out, err = impl.invoke(impl.check, test, args)
                if code is None:
                    fails.append("checker timed out")
                elif code == 0:
                    fails.append("checker accepted a program it must reject")
                else:
                    fails += match_reports(test, err)
            if test.tier == "unchecked" and impl.run_unchecked:
                code, out, err = impl.invoke(impl.run_unchecked, test, args, tmpdir)
                fails += expect_run(test, code, out, err, panicking=bool(test.get("panic")))
        else:
            if impl.check:
                code, out, err = impl.invoke(impl.check, test, args)
                if code is None:
                    fails.append("checker timed out")
                elif code != 0:
                    fails.append("checker rejected: " + first_line(err))
            if impl.run:
                code, out, err = impl.invoke(impl.run, test, args, tmpdir)
                fails += expect_run(test, code, out, err, panicking=(test.tier == "panic"))
                if consistency and test.tier in ("run", "panic") and impl.run_unchecked:
                    code2, out2, _ = impl.invoke(impl.run_unchecked, test, args, tmpdir)
                    if code2 is None or (code2 == 0) != (code == 0) or out2 != out:
                        fails.append(f"unchecked run differs: exit {code2} vs {code}, stdout {'same' if out2 == out else 'differs'}")
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)
    return fails


# ----------------------------------------------------------------------------- reports

def checklist_items():
    with open(os.path.join(HERE, "README.md"), encoding="utf-8") as fh:
        readme = fh.read()
    section = readme.split("## Coverage checklist", 1)[1]
    items = []
    for ln in section.splitlines():
        if ln.startswith("- "):
            items += CHECKLIST_ITEM_RE.findall(ln)
    return items


def coverage(tests):
    items = checklist_items()
    corpus = "\n".join(src for t in tests for src in t.sources.values())
    phrases = [t.get("panic") for t in tests if t.get("panic")]
    missing = []
    for it in items:
        if it.startswith("panic:"):
            if not any(it[6:] in p for p in phrases):
                missing.append(it)
        elif it not in corpus:
            missing.append(it)
    return items, missing


def main():
    ap = argparse.ArgumentParser(description="Ant semantic test runner")
    ap.add_argument("--tier", action="append", choices=TIERS, help="restrict to a tier (repeatable)")
    ap.add_argument("--filter", action="append", help="substring of the test path (repeatable, any match)")
    ap.add_argument("--exclude", action="append", help="skip tests whose path contains this (repeatable)")
    ap.add_argument("--timeout", type=float, help="per-test timeout in seconds (overrides the directive)")
    ap.add_argument("--parse", action="store_true", help="parse mode (the default without an implementation)")
    ap.add_argument("--check", help="checker command template, e.g. 'antc check {file}'")
    ap.add_argument("--run", help="run command template, e.g. 'antc run {file} {args}'")
    ap.add_argument("--run-unchecked", dest="run_unchecked", help="run-without-checker command template")
    ap.add_argument("--oracle", action="store_true", help="drive the suite through oracle.py")
    ap.add_argument("--sabotage", action="store_true", help="with --oracle: misbehave on purpose")
    ap.add_argument("--consistency", action="store_true", help="run and panic tiers must behave the same unchecked")
    ap.add_argument("--strict", action="store_true", help="fail on gap ids missing from GAPS.md and vice versa")
    ap.add_argument("--coverage", action="store_true", help="list checklist entries no test mentions")
    ap.add_argument("--gaps", action="store_true", help="list tests per gap id")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    tiers = a.tier or list(TIERS)
    Test.timeout_override = a.timeout
    tests = discover(HERE, tiers, a.filter, a.exclude)
    gaps_path = os.path.join(HERE, "GAPS.md")
    gap_ids, untested_gaps = gap_ids_in(gaps_path)

    if a.list:
        for t in tests:
            print(f"{t.tier:9} {t.rel}")
        return 0
    if a.gaps:
        by = {}
        for t in tests:
            for g in t.gaps:
                by.setdefault(g, []).append(t.rel)
        for g in sorted(set(by) | gap_ids, key=lambda s: (len(s), s)):
            print(f"{g}: " + (", ".join(by.get(g, [])) or "(no test)"))
        return 0
    if a.coverage:
        items, missing = coverage(tests)
        print(f"{len(items) - len(missing)} of {len(items)} checklist entries covered")
        for m in missing:
            print("  missing:", m)
        return 1 if missing else 0

    impl = None
    if a.oracle:
        oracle = os.path.join(HERE, "oracle.py")
        py = shlex.quote(sys.executable)
        impl = Impl(f"{py} -I {shlex.quote(oracle)} check {{file}}",
                    f"{py} -I {shlex.quote(oracle)} run {{file}} {{args}}",
                    f"{py} -I {shlex.quote(oracle)} run-unchecked {{file}} {{args}}",
                    env={"ORACLE_SABOTAGE": "1"} if a.sabotage else {})
    elif a.check or a.run or a.run_unchecked:
        impl = Impl(a.check, a.run, a.run_unchecked)

    failures, warnings = [], []
    count = 0
    skipped = 0
    for f in files_under(os.path.join(HERE, "lib")):
        count += 1
        try:
            with open(f, encoding="utf-8") as fh:
                antparse.parse(fh.read())
        except antparse.ParseError as e:
            failures.append((os.path.relpath(f, HERE), f"parse: {e}"))

    for t in tests:
        count += 1
        probs, warns = validate(t, gap_ids, a.strict)
        if probs:
            failures.append((t.rel, "; ".join(probs)))
            continue
        warnings += [(t.rel, w) for w in warns]
        err = parse_check(t)
        if err:
            failures.append((t.rel, "parse: " + err))
            continue
        if impl and t.tier == "check" and not impl.check:
            skipped += 1
            if a.verbose:
                print("skip", t.rel, "(no --check command)")
            continue
        if impl:
            fails = run_test(t, impl, a.consistency)
            if fails:
                failures.append((t.rel, "\n    ".join(fails)))
                continue
        if a.verbose:
            print("ok  ", t.rel)

    if a.strict and not a.filter and not a.tier:
        referenced = {g for t in tests for g in t.gaps}
        for g in sorted(gap_ids - referenced - untested_gaps, key=lambda s: (len(s), s)):
            failures.append(("GAPS.md", f"{g} is referenced by no test and not marked (untested)"))
        for g in sorted(untested_gaps & referenced, key=lambda s: (len(s), s)):
            failures.append(("GAPS.md", f"{g} is marked (untested) but a test references it"))

    for rel, why in failures:
        print(f"FAIL {rel}\n    {why}")
    for rel, w in warnings:
        print(f"warn {rel}: {w}")
    mode = "oracle" if a.oracle else ("implementation" if impl else "parse")
    skip_note = f", {skipped} check-tier tests skipped (no --check)" if skipped else ""
    print(f"{count} checks, {len(failures)} failures, {len(warnings)} warnings ({mode} mode, {len(tests)} tests{skip_note})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
