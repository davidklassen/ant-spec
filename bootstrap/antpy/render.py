"""String.from (section 11; GAPS G33, G55)."""
import math

from antpy.values import (
    KIND, INT_KINDS, FLOAT_KINDS, VOID, MODULE_TAG, Record, Tag, round_f32,
)
from antpy.equal import AntMap


def render(v):
    """String.from: a string renders bare at the top level, and every other
    value as it does inside a container."""
    if type(v) is str:
        return v
    if type(v) is int:
        return str(v)
    return render_in(v, set())


QUOTE_TABLE = str.maketrans({"\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t", "\r": "\\r"})


def quote(s):
    return '"' + s.translate(QUOTE_TABLE) + '"'


def render_in(v, active):
    """Renders `v` as an element of a container: a string quoted. `active`
    holds the ids of the containers being rendered (they stay alive while
    they are in it), so a cycle renders as `...`."""
    t = type(v)
    k = KIND[t]
    if k == "string":
        return quote(v)
    if k in INT_KINDS:
        return str(v)
    if k in FLOAT_KINDS:
        return render_float(v, k == "float32")
    if k == "bool":
        return "true" if v else "false"
    if v is None:
        return "null"
    if v is VOID:
        return "void"
    if k == "func":
        return "func"
    if t is Tag:
        return v.name
    if k == "sync" or k == "file":
        return t.__name__     # the class names are the namespace names
    if t is Record and v.tag is MODULE_TAG:
        return "Module"
    if id(v) in active:
        return "..."
    active.add(id(v))
    try:
        if t is list:
            return "[" + ", ".join(render_in(x, active) for x in v) + "]"
        if t is AntMap:
            if v.size == 0:
                return "Map{}"
            return "Map{ " + ", ".join(
                render_in(a, active) + ": " + render_in(b, active) for a, b in v.items()) + " }"
        if not v.fields:
            return v.tag.name + "{}"
        return v.tag.name + "{ " + ", ".join(
            n + ": " + render_in(x, active) for n, x in v.fields.items()) + " }"
    finally:
        active.discard(id(v))


def render_float(x, f32):
    if x != x:
        return "NaN"
    if x == math.inf:
        return "Infinity"
    if x == -math.inf:
        return "-Infinity"
    sign = "-" if math.copysign(1.0, x) < 0 else ""
    if x == 0.0:
        return sign + "0.0"
    digits, n = shortest_digits(abs(x), f32)
    return sign + layout(digits, n)


def shortest_digits(x, f32):
    """The shortest decimal digit string that reads back as x, and the position
    n of the decimal point: x = 0.<digits> * 10**n."""
    if f32:
        for precision in range(1, 10):
            s = "%.*g" % (precision, x)
            if round_f32(float(s)) == x:
                break
    else:
        s = repr(x)
    if "e" in s:
        mantissa, exponent = s.split("e")
        exponent = int(exponent)
    else:
        mantissa, exponent = s, 0
    if "." in mantissa:
        whole, fraction = mantissa.split(".")
    else:
        whole, fraction = mantissa, ""
    digits = whole + fraction
    n = len(whole) + exponent
    stripped = digits.lstrip("0")
    n -= len(digits) - len(stripped)
    return stripped.rstrip("0"), n


def layout(digits, n):
    """JavaScript's Number-to-string layout (G55), always with a point or an
    exponent (G33), and no `+` in the exponent."""
    k = len(digits)
    if k <= n <= 21:
        return digits + "0" * (n - k) + ".0"
    if 0 < n <= 21:
        return digits[:n] + "." + digits[n:]
    if -6 < n <= 0:
        return "0." + "0" * (-n) + digits
    mantissa = digits[0] + ("." + digits[1:] if k > 1 else "")
    return mantissa + "e" + str(n - 1)
