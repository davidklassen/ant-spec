"""The core library's data namespaces (section 11): Array, Map, String, Iter,
and Math as Python functions over the values of values.py, with the argument
checks they share. prelude.py binds them in the prelude scope.

Every function checks the kinds of its arguments first (`wrong kind`), then
the value-dependent panics the spec marks with a diamond, then does the work.
Kind checks use `type(v) is ...`, never isinstance: bool subclasses int and
Float32 subclasses float. A function that is not marked `mutates` in section
11 returns a new array, map, or string.

A registry (ARRAY, MAP, ...) maps a field name to (parameter names, function)
or (parameter names, function, True) for one that needs the interpreter: it
receives (interp, frame, *args) and calls back into Ant with
interp.call(f, [arg], (), frame). Any other value is a constant field, like
Math.PI."""
import functools
import math
import re

from antpy.values import (
    VOID, MISSING, Record, Native, YieldFn, UInt8, OK, ERR, ERROR, ENTRY, INDEXED,
    KIND, INT_KINDS, NUMBER_KINDS, INT_RANGE, panic,
)
from antpy.equal import AntMap, eq
from antpy import arith
from antpy.render import render


# ----------------------------------------------------------------------------- argument checks

def need(v, t, what):
    if type(v) is not t:
        panic("wrong kind: expected " + what)
    return v


def need_array(v):
    return need(v, list, "an array")


def need_map(v):
    return need(v, AntMap, "a map")


def need_string(v):
    return need(v, str, "a string")


def need_int(v):
    return need(v, int, "an int")


def need_record(v):
    return need(v, Record, "a record")


def need_number(v):
    if KIND[type(v)] not in NUMBER_KINDS:
        panic("wrong kind: expected a number")
    return v


def need_func(v):
    if KIND[type(v)] != "func":
        panic("wrong kind: expected a function")
    return v


def need_bool_result(v):
    if type(v) is not bool:
        panic("not a bool")
    return v


def need_float64(v):
    return need(v, float, "a float64")     # Float32 is its own type, so a float32 is `wrong kind`


def need_bytes(v):
    """An array of bytes (`uint8`) as Python bytes. The array is copied
    before the check so that another goroutine cannot change it between the
    check and the conversion (section 12: a data race cannot crash the
    interpreter)."""
    items = list(need_array(v))
    for b in items:
        need(b, UInt8, "a byte")
    return bytes(items)


def byte_array(data):
    """Python bytes as an Ant array of `uint8`."""
    return [UInt8(b) for b in data]


ORDERED_KINDS = NUMBER_KINDS | frozenset(("string",))


def need_ordered(v):
    if KIND[type(v)] not in ORDERED_KINDS:
        panic("wrong kind: expected a number or a string")
    return v


def record_field(r, name):
    """A field read that panics `no field` when the field is absent (section
    12, `a.b`): an unchecked program can build an `Ok{}` without its `value`."""
    v = r.fields.get(name, MISSING)
    if v is MISSING:
        panic("no field: " + name)
    return v


def compare_values(a, b):
    """A three-way comparison through the `<` operator, so that two values of
    different kinds panic `kind mismatch` and a non-Ordered one `not a number`."""
    if arith.binary("<", a, b):
        return -1
    if arith.binary("<", b, a):
        return 1
    return 0


# ----------------------------------------------------------------------------- Result constructors

# The prelude's Result shape (section 8): `Ok{ value }`, `Err{ error }`, and
# the `Error{ message }` the I/O namespaces fail with. One definition, shared
# by Result.ok/err and every native that returns a Result.

def ok(v):
    return Record(OK, {"value": v})


def err(e):
    return Record(ERR, {"error": e})


def error(message):
    return Record(ERROR, {"message": message})


# ----------------------------------------------------------------------------- Array

def array_get(xs, i):
    need_array(xs)
    need_int(i)
    if i < 0:
        return None
    try:
        return xs[i]
    except IndexError:                    # out of range, or shrunk by another goroutine
        return None


def array_push(xs, v):
    need_array(xs).append(v)
    return VOID


def array_pop(xs):
    need_array(xs)
    try:
        return xs.pop()
    except IndexError:                    # empty, or emptied by another goroutine
        return None


def check_slice(length, start, end):
    """Array.slice and String.slice take `int` bounds with 0 <= from <= to <= length."""
    need_int(start)
    need_int(end)
    if not 0 <= start <= end <= length:
        panic("index out of range")


def array_slice(xs, start, end):
    need_array(xs)
    check_slice(len(xs), start, end)
    return xs[start:end]


def array_concat(xs, ys):
    return need_array(xs) + need_array(ys)


def array_map(interp, frame, xs, f):
    need_array(xs)
    need_func(f)
    return [interp.call(f, [x], (), frame) for x in xs]


def array_filter(interp, frame, xs, f):
    need_array(xs)
    need_func(f)
    return [x for x in xs if need_bool_result(interp.call(f, [x], (), frame))]


def array_reduce(interp, frame, xs, init, f):
    need_array(xs)
    need_func(f)
    acc = init
    for x in xs:
        acc = interp.call(f, [acc, x], (), frame)
    return acc


def array_find(interp, frame, xs, f):
    need_array(xs)
    need_func(f)
    for x in xs:
        if need_bool_result(interp.call(f, [x], (), frame)):
            return x
    return None


def array_some(interp, frame, xs, f):
    need_array(xs)
    need_func(f)
    for x in xs:
        if need_bool_result(interp.call(f, [x], (), frame)):
            return True
    return False


def array_every(interp, frame, xs, f):
    need_array(xs)
    need_func(f)
    for x in xs:
        if not need_bool_result(interp.call(f, [x], (), frame)):
            return False
    return True


def array_contains(xs, v):
    return array_index_of(xs, v) is not None


def array_index_of(xs, v):
    need_array(xs)
    for i, x in enumerate(xs):
        if eq(x, v):                      # by ==, so a NaN is never found
            return i
    return None


def array_sort(xs):
    """Ascending and stable. Every element must be Ordered (`wrong kind`);
    elements of different kinds then panic `kind mismatch` in the comparison (G15)."""
    need_array(xs)
    for x in xs:
        need_ordered(x)
    return sorted(xs, key=functools.cmp_to_key(compare_values))


def array_sort_by(interp, frame, xs, key):
    """`key` runs once per element, in order; sorting the indices by the
    precomputed keys keeps the sort stable (G35)."""
    need_array(xs)
    need_func(key)
    keys = [need_ordered(interp.call(key, [x], (), frame)) for x in xs]
    order = sorted(range(len(xs)), key=functools.cmp_to_key(lambda i, j: compare_values(keys[i], keys[j])))
    return [xs[i] for i in order]


def array_sort_with(interp, frame, xs, cmp):
    need_array(xs)
    need_func(cmp)

    def compare(a, b):
        return need_int(interp.call(cmp, [a, b], (), frame))

    return sorted(xs, key=functools.cmp_to_key(compare))


def array_reverse(xs):
    return need_array(xs)[::-1]


def array_entries(xs):
    need_array(xs)
    return [Record(INDEXED, {"index": i, "value": v}) for i, v in enumerate(xs)]


def array_range(start, end):
    need_int(start)
    need_int(end)
    return list(range(start, end))        # empty when to <= from


def array_filled(n, v):
    need_int(n)
    if n < 0:
        panic("negative count")
    return [v] * n                        # the same v in every slot, never a copy


def array_flatten(xs):
    """One level: every element must itself be an array."""
    need_array(xs)
    for x in xs:
        need_array(x)
    return [y for x in xs for y in x]


def array_sum(xs):
    """`null` when empty: with types erased there is no zero of the right
    kind. The fold is the `+` operator, so an overflow panics `integer
    overflow` and elements of different kinds panic `kind mismatch`."""
    need_array(xs)
    if not xs:
        return None
    for x in xs:
        need_number(x)
    return functools.reduce(functools.partial(arith.binary, "+"), xs)


def array_same(xs, ys):
    return need_array(xs) is need_array(ys)


ARRAY = {
    "length": (["xs"], lambda xs: len(need_array(xs))),
    "isEmpty": (["xs"], lambda xs: not need_array(xs)),
    "get": (["xs", "i"], array_get),
    "push": (["xs", "v"], array_push),
    "pop": (["xs"], array_pop),
    "slice": (["xs", "from", "to"], array_slice),
    "concat": (["xs", "ys"], array_concat),
    "map": (["xs", "f"], array_map, True),
    "filter": (["xs", "f"], array_filter, True),
    "reduce": (["xs", "init", "f"], array_reduce, True),
    "find": (["xs", "f"], array_find, True),
    "some": (["xs", "f"], array_some, True),
    "every": (["xs", "f"], array_every, True),
    "contains": (["xs", "v"], array_contains),
    "indexOf": (["xs", "v"], array_index_of),
    "sort": (["xs"], array_sort),
    "sortBy": (["xs", "key"], array_sort_by, True),
    "sortWith": (["xs", "cmp"], array_sort_with, True),
    "reverse": (["xs"], array_reverse),
    "entries": (["xs"], array_entries),
    "range": (["from", "to"], array_range),
    "filled": (["n", "v"], array_filled),
    "flatten": (["xs"], array_flatten),
    "sum": (["xs"], array_sum),
    "same": (["xs", "ys"], array_same),
}


# ----------------------------------------------------------------------------- Map

def map_get(m, k):
    return need_map(m).get(k)


def map_has(m, k):
    return need_map(m).has(k)


def map_set(m, k, v):
    need_map(m).set(k, v)
    return VOID


def map_size(m):
    return need_map(m).size


def map_is_empty(m):
    return need_map(m).size == 0


def map_remove(m, k):
    v = need_map(m).remove(k)
    return None if v is MISSING else v


def entry(k, v):
    return Record(ENTRY, {"key": k, "value": v})


def map_keys(m):
    return [k for k, _ in need_map(m).items()]


def map_values(m):
    return [v for _, v in need_map(m).items()]


def map_entries(m):
    return [entry(k, v) for k, v in need_map(m).items()]


def map_from_entries(es):
    need_array(es)
    out = AntMap()
    for e in es:
        need_record(e)
        out.set(record_field(e, "key"), record_field(e, "value"))   # later entries win
    return out


def map_map(interp, frame, m, f):
    """Every key kept, each value replaced by f(Entry); a new map."""
    need_map(m)
    need_func(f)
    out = AntMap()
    for k, v in m.items():                # items() is a snapshot, so f may mutate m
        out.set(k, interp.call(f, [entry(k, v)], (), frame))
    return out


def map_filter(interp, frame, m, f):
    need_map(m)
    need_func(f)
    out = AntMap()
    for k, v in m.items():
        if need_bool_result(interp.call(f, [entry(k, v)], (), frame)):
            out.set(k, v)
    return out


def map_merge(m, n):
    need_map(m)
    need_map(n)
    out = AntMap()
    for k, v in m.items():
        out.set(k, v)
    for k, v in n.items():
        out.set(k, v)                     # entries of n win
    return out


def map_same(m, n):
    return need_map(m) is need_map(n)


MAP = {
    "size": (["m"], map_size),
    "isEmpty": (["m"], map_is_empty),
    "has": (["m", "k"], map_has),
    "get": (["m", "k"], map_get),
    "set": (["m", "k", "v"], map_set),
    "remove": (["m", "k"], map_remove),
    "keys": (["m"], map_keys),
    "values": (["m"], map_values),
    "entries": (["m"], map_entries),
    "fromEntries": (["entries"], map_from_entries),
    "map": (["m", "f"], map_map, True),
    "filter": (["m", "f"], map_filter, True),
    "merge": (["m", "n"], map_merge),
    "same": (["m", "n"], map_same),
}


# ----------------------------------------------------------------------------- String

def string_length(s):
    return len(need_string(s))


def string_is_empty(s):
    return need_string(s) == ""


def case_map(s, fn):
    """Maps each code point with `fn` when the result is one code point and
    leaves it alone otherwise (G36): `"ß"` stays `"ß"`, `"é"` becomes `"É"`."""
    out = []
    for c in need_string(s):
        mapped = fn(c)
        out.append(mapped if len(mapped) == 1 else c)
    return "".join(out)


def string_to_upper(s):
    return case_map(s, str.upper)


def string_to_lower(s):
    return case_map(s, str.lower)


def string_from_bytes(bs):
    """Strict UTF-8: any invalid sequence gives null, never a replacement."""
    data = need_bytes(bs)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


INT_LITERAL = re.compile(r"[+-]?[0-9]+")     # ASCII digits only; Python's int() takes more


def string_to_int(s):
    """An optional sign and decimal digits, within the `int` range; null
    otherwise (G36). Whitespace, `_`, a radix prefix, and Unicode digits are
    all rejected by the pattern, so int() is only ever called on a match."""
    need_string(s)
    if INT_LITERAL.fullmatch(s) is None:
        return None
    n = int(s)
    lo, hi = INT_RANGE["int64"]
    return n if lo <= n <= hi else None


def string_chars(s):
    return list(need_string(s))


def string_join(parts, sep):
    need_array(parts)
    need_string(sep)
    for p in parts:
        need_string(p)
    return sep.join(parts)


def string_to_bytes(s):
    return byte_array(need_string(s).encode("utf-8"))


def string_slice(s, start, end):
    need_string(s)
    check_slice(len(s), start, end)       # code point indices: Python str indexes by code point
    return s[start:end]


def string_concat(parts):
    return string_join(parts, "")


def string_split(s, sep):
    need_string(s)
    need_string(sep)
    if sep == "":
        panic("empty separator")
    return s.split(sep)                   # [""] for "", ["", ""] for a lone separator (G36)


def string_repeat(s, n):
    need_string(s)
    need_int(n)
    if n < 0:
        panic("negative count")
    return s * n


# The code points with the Unicode White_Space property (section 11, `trim`).
# Not str.strip(): Python also strips U+001C..U+001F, which lack the property.
WHITE_SPACE = (
    "\t\n\v\f\r \x85\xa0\u1680"
    "\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a"
    "\u2028\u2029\u202f\u205f\u3000"
)


def string_trim(s):
    return need_string(s).strip(WHITE_SPACE)


def string_contains(s, t):
    need_string(s)
    return need_string(t) in s


def string_starts_with(s, t):
    need_string(s)
    return s.startswith(need_string(t))


def string_ends_with(s, t):
    need_string(s)
    return s.endswith(need_string(t))


def string_index_of(s, t):
    need_string(s)
    i = s.find(need_string(t))            # find("") is 0 (G36)
    return None if i < 0 else i


def string_replace(s, old, new):
    need_string(s)
    need_string(old)
    need_string(new)
    if old == "":
        panic("empty pattern")
    return s.replace(old, new)            # every occurrence, left to right, never rescanned


# An optional sign, digits, an optional fraction, an optional exponent (G36).
# Python's float() also takes " 1.0", "inf", "nan", "1_0", and ".5", which are all null here.
FLOAT_LITERAL = re.compile(r"[+-]?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?")


def string_to_float(s):
    need_string(s)
    if FLOAT_LITERAL.fullmatch(s) is None:
        return None
    v = float(s)
    return v if math.isfinite(v) else None   # null on overflow, like toInt


STRING = {
    "length": (["s"], string_length),
    "isEmpty": (["s"], string_is_empty),
    "slice": (["s", "from", "to"], string_slice),
    "concat": (["parts"], string_concat),
    "split": (["s", "sep"], string_split),
    "join": (["parts", "sep"], string_join),
    "repeat": (["s", "n"], string_repeat),
    "trim": (["s"], string_trim),
    "contains": (["s", "t"], string_contains),
    "startsWith": (["s", "t"], string_starts_with),
    "endsWith": (["s", "t"], string_ends_with),
    "indexOf": (["s", "t"], string_index_of),
    "replace": (["s", "old", "new"], string_replace),
    "toUpper": (["s"], string_to_upper),
    "toLower": (["s"], string_to_lower),
    "chars": (["s"], string_chars),
    "toBytes": (["s"], string_to_bytes),
    "fromBytes": (["bytes"], string_from_bytes),
    "toInt": (["s"], string_to_int),
    "toFloat": (["s"], string_to_float),
    "from": (["value"], render),
}


# ----------------------------------------------------------------------------- Iter

# A sequence is a function of `yield` (section 5). Each Iter function checks
# its arguments at the call and returns a stage that runs its source only when
# it is itself run, with a fresh yield per run, so a sequence can be run again.
# A stage's body returns what the downstream yield returned, so a `false` from
# a `break` in the loop travels back through every stage to the source.

def stage(name, run):
    return Native("Iter." + name, ["yield"], run, ctx=True)


def drive(interp, frame, source, body):
    """Runs `source` with a fresh yield whose `body(v)` returns True to
    continue. The yield is done once the source returns (section 12, `for`),
    so a late call panics `yield after done`."""
    y = YieldFn(body)
    try:
        interp.call(source, [y], (), frame)
    finally:
        y.done = True
    return VOID


def iter_range(start, end):
    need_int(start)
    need_int(end)

    def run(interp, frame, y):
        need_func(y)
        for i in range(start, end):
            if not need_bool_result(interp.call(y, [i], (), frame)):
                return VOID
        return VOID

    return stage("range", run)


def iter_from_array(xs):
    need_array(xs)

    def run(interp, frame, y):
        need_func(y)
        i = 0
        while i < len(xs):                # the length is re-read, like `for`: a push before the run is seen
            try:
                v = xs[i]
            except IndexError:            # popped by another goroutine: not visited
                return VOID
            i += 1
            if not need_bool_result(interp.call(y, [v], (), frame)):
                return VOID
        return VOID

    return stage("fromArray", run)


def iter_map(s, f):
    need_func(s)
    need_func(f)

    def run(interp, frame, y):
        need_func(y)

        def body(v):
            return need_bool_result(interp.call(y, [interp.call(f, [v], (), frame)], (), frame))

        return drive(interp, frame, s, body)

    return stage("map", run)


def iter_filter(s, f):
    need_func(s)
    need_func(f)

    def run(interp, frame, y):
        need_func(y)

        def body(v):
            if need_bool_result(interp.call(f, [v], (), frame)):
                return need_bool_result(interp.call(y, [v], (), frame))
            return True

        return drive(interp, frame, s, body)

    return stage("filter", run)


def iter_take(s, n):
    """`n < 0` panics at the call (G65); take(s, 0) never runs the source
    (G37); otherwise the source is stopped right after the nth element."""
    need_func(s)
    need_int(n)
    if n < 0:
        panic("negative count")

    def run(interp, frame, y):
        need_func(y)
        if n == 0:
            return VOID
        left = n

        def body(v):
            nonlocal left
            left -= 1
            more = need_bool_result(interp.call(y, [v], (), frame))
            return more and left > 0

        return drive(interp, frame, s, body)

    return stage("take", run)


def iter_to_array(interp, frame, s):
    need_func(s)
    out = []

    def collect(v):
        out.append(v)
        return True

    drive(interp, frame, s, collect)
    return out


ITER = {
    "range": (["from", "to"], iter_range),
    "fromArray": (["xs"], iter_from_array),
    "map": (["s", "f"], iter_map),
    "filter": (["s", "f"], iter_filter),
    "take": (["s", "n"], iter_take),
    "toArray": (["s"], iter_to_array, True),
}


# ----------------------------------------------------------------------------- Math

def math_abs(x):
    """Integers keep their kind and panic `integer overflow` on the most
    negative value (through the operator's negation); floats clear the sign
    bit, so abs(-0.0) is 0.0 (G56)."""
    need_number(x)
    if KIND[type(x)] in INT_KINDS:
        return x if x >= 0 else arith.neg(x)
    return type(x)(math.fabs(x))


def math_min(a, b):
    """The first argument on a tie or with a NaN; two different kinds panic
    `kind mismatch` through the operator, like `Array.sort`."""
    need_ordered(a)
    need_ordered(b)
    return b if arith.binary("<", b, a) else a


def math_max(a, b):
    need_ordered(a)
    need_ordered(b)
    return b if arith.binary("<", a, b) else a


def is_odd_integer(y):
    return abs(math.fmod(y, 2.0)) == 1.0


def math_pow(x, y):
    """C's pow: Python raises where IEEE gives an infinity or a NaN."""
    need_float64(x)
    need_float64(y)
    try:
        return math.pow(x, y)
    except OverflowError:                 # a finite result beyond the range
        return -math.inf if x < 0 and is_odd_integer(y) else math.inf
    except ValueError:
        if x == 0.0:                      # a zero to a negative power: the pole
            return -math.inf if math.copysign(1.0, x) < 0 and is_odd_integer(y) else math.inf
        return math.nan                   # a negative base with a fractional exponent


def math_sqrt(x):
    need_float64(x)
    try:
        return math.sqrt(x)
    except ValueError:                    # a negative argument
        return math.nan


def rounded(x, fn):
    """Applies the integer rounding `fn` and returns a float64: inf and NaN
    pass through, and a zero result keeps the sign of x (ceil(-0.5) is -0.0)."""
    if not math.isfinite(x):
        return x
    r = float(fn(x))
    return math.copysign(r, x) if r == 0.0 else r


def round_half_away(x):
    """Half away from zero (section 11). Python's round() is half to even, and
    floor(x + 0.5) is wrong at 0.49999999999999994, where the sum is 1.0."""
    ax = abs(x)
    r = math.floor(ax)
    if ax - r >= 0.5:                     # exact: the fraction of a double subtracts exactly
        r += 1
    return math.copysign(r, x)


def math_floor(x):
    return rounded(need_float64(x), math.floor)


def math_ceil(x):
    return rounded(need_float64(x), math.ceil)


def math_round(x):
    return rounded(need_float64(x), round_half_away)


MATH = {
    "abs": (["x"], math_abs),
    "min": (["a", "b"], math_min),
    "max": (["a", "b"], math_max),
    "pow": (["x", "y"], math_pow),
    "sqrt": (["x"], math_sqrt),
    "floor": (["x"], math_floor),
    "ceil": (["x"], math_ceil),
    "round": (["x"], math_round),
    "PI": math.pi,
    "E": math.e,
}
