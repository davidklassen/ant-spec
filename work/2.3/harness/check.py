"""Compares the runtime's float rendering and case mapping (through harness)
with the bootstrap's render_float / case_map over powers of two, random
doubles and floats, and every code point."""
import random
import struct
import subprocess
import sys

sys.path.insert(0, sys.argv[1])          # <repo>/bootstrap
from antpy.render import render_float   # noqa: E402
from antpy.library import case_map      # noqa: E402

HARNESS = sys.argv[2]


def run(lines):
    data = "\n".join(lines) + "\n"
    out = subprocess.run([HARNESS], input=data.encode(), capture_output=True, check=True)
    return out.stdout.decode().split("\n")[:-1]


def dbl_bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f32_round(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


# --- doubles
rng = random.Random(1)
doubles = [2.0 ** e for e in range(-1074, 1024)]
doubles += [10.0 ** e for e in range(-300, 300)]
doubles += [1e23, 5e-324, 1.7976931348623157e308, 2.2250738585072014e-308, 0.1 + 0.7, 100.0 / 3.0,
            9.999999999999999e22, 99999.99999999999, 0.30000000000000004, 123456789012345680000.0]
for _ in range(300000):
    bits = rng.getrandbits(64)
    x = struct.unpack("<d", struct.pack("<Q", bits))[0]
    if x == x and abs(x) != float("inf"):
        doubles.append(x)
for _ in range(100000):
    doubles.append(rng.random() * 10 ** rng.randint(-30, 30))
lines = ["f %016x" % dbl_bits(x) for x in doubles]
got = run(lines)
bad = 0
longer = 0
for x, g in zip(doubles, got):
    want = render_float(x, False)
    if g != want:
        bad += 1
        if bad <= 10:
            print("DOUBLE %r: runtime %s, bootstrap %s" % (x, g, want))
print("doubles: %d checked, %d differ" % (len(doubles), bad))

# --- float32
floats = [f32_round(2.0 ** e) for e in range(-149, 128)]
floats += [f32_round(0.1), f32_round(1.0 / 3), f32_round(16777216.0), f32_round(3.4028235e38), f32_round(1e-45)]
for _ in range(200000):
    bits = rng.getrandbits(32)
    x = struct.unpack("<f", struct.pack("<I", bits))[0]
    if x == x and abs(x) != float("inf"):
        floats.append(x)
lines = ["g %08x" % f32_bits(x) for x in floats]
got = run(lines)
bad = 0
shorter = 0
boot_crash = 0
for x, g in zip(floats, got):
    try:
        want = render_float(x, True)
    except OverflowError:          # bootstrap bug near FLT_MAX (work/2.2/report.md)
        assert f32_round(float(g)) == x, (x, g)
        boot_crash += 1
        continue
    if g != want:
        # the runtime may be shorter (the bootstrap has no neighbour test); check it round-trips
        assert f32_round(float(g)) == x, (x, g)
        assert len(g) < len(want), (x, g, want)
        shorter += 1
        if shorter <= 5:
            print("FLOAT32 %r: runtime %s (shorter, round-trips), bootstrap %s" % (x, g, want))
print("float32: %d checked, %d where the runtime is shorter, %d where the bootstrap crashes (runtime round-trips), 0 wrong" % (len(floats), shorter, boot_crash))

# --- case mapping, every code point
cps = [cp for cp in range(0x110000) if not (0xD800 <= cp <= 0xDFFF) and cp not in (0, 10, 13)]
for mode, fn in (("u", str.upper), ("l", str.lower)):
    lines = ["%s %s" % (mode, chr(cp)) for cp in cps]
    got = run(lines)
    bad = 0
    for cp, g in zip(cps, got):
        want = case_map(chr(cp), fn)
        if g != want:
            bad += 1
            if bad <= 10:
                print("CASE %s U+%04X: runtime %r, bootstrap %r" % (mode, cp, g, want))
    print("case %s: %d code points checked, %d differ" % (mode, len(cps), bad))
# mixed strings
mixed = ["héllo wörld ß ı İ ǅ σς Σ я Я ａＡ 𐐀𐐨 Ⴀა ᲐᲑ", "ABCxyz", "", "ß", "İstanbul", "ΣΊΣΥΦΟΣ"]
for mode, fn in (("u", str.upper), ("l", str.lower)):
    got = run(["%s %s" % (mode, s) for s in mixed])
    for s, g in zip(mixed, got):
        want = case_map(s, fn)
        print("%s %r -> %r %s" % (mode, s, g, "ok" if g == want else "WANT %r" % want))
