"""The prelude scope (section 11): tags, conversions, print, panic, and the
library namespaces. Each namespace is a record of Native functions, so
`Array.map` is an ordinary field read (section 10).

A native is declared with its parameter names (named arguments resolve against
them). One marked ctx=True receives (interp, frame, *args) and may call back
into Ant with interp.call."""
import functools
import re
import sys

from antpy.values import (
    VOID, MISSING, Scope, Record, Tag, Native, YieldFn, Mutex, WaitGroup, UInt8,
    CONVERSION_KINDS, KIND, NUMBER_KINDS, INT_RANGE, panic,
)
from antpy.equal import AntMap, eq
from antpy import arith
from antpy.render import render

OK = Tag("Ok")
ERR = Tag("Err")
ERROR = Tag("Error")
ENTRY = Tag("Entry")
INDEXED = Tag("Indexed")


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


def need_result(v):
    if type(v) is not Record or (v.tag is not OK and v.tag is not ERR):
        panic("wrong kind: expected a Result")
    return v


# ----------------------------------------------------------------------------- Array

def array_get(xs, i):
    need_array(xs)
    need_int(i)
    return xs[i] if 0 <= i < len(xs) else None


def array_push(xs, v):
    need_array(xs).append(v)
    return VOID


def array_pop(xs):
    need_array(xs)
    return xs.pop() if xs else None


def array_slice(xs, start, end):
    need_array(xs)
    need_int(start)
    need_int(end)
    if not 0 <= start <= end <= len(xs):
        panic("index out of range")
    return xs[start:end]


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
    need_array(xs)
    for x in xs:
        if eq(x, v):
            return True
    return False


def compare_values(a, b):
    if arith.binary("<", a, b):
        return -1
    if arith.binary("<", b, a):
        return 1
    return 0


def array_sort(xs):
    need_array(xs)
    return sorted(xs, key=functools.cmp_to_key(compare_values))   # stable


def array_sort_with(interp, frame, xs, cmp):
    need_array(xs)
    need_func(cmp)

    def compare(a, b):
        return need_int(interp.call(cmp, [a, b], (), frame))

    return sorted(xs, key=functools.cmp_to_key(compare))


def array_entries(xs):
    need_array(xs)
    return [Record(INDEXED, {"index": i, "value": v}) for i, v in enumerate(xs)]


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


# ----------------------------------------------------------------------------- Map, String, Record, Result

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


def map_same(m, n):
    return need_map(m) is need_map(n)


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
    need_array(bs)
    for b in bs:
        need(b, UInt8, "a byte")
    try:
        return bytes(bs).decode("utf-8")
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
    return [UInt8(b) for b in need_string(s).encode("utf-8")]


def record_same(a, b):
    return need_record(a) is need_record(b)


def result_ok(v):
    return Record(OK, {"value": v})


def result_err(e):
    return Record(ERR, {"error": e})


def result_is_ok(r):
    return need_result(r).tag is OK


def result_payload(r, name):
    """The `value` of an Ok or the `error` of an Err; an unchecked program
    can build an `Ok{}` without one, which is `no field` like any read."""
    v = r.fields.get(name, MISSING)
    if v is MISSING:
        panic("no field: " + name)
    return v


def result_unwrap(r):
    if need_result(r).tag is ERR:
        panic("unwrap on Err")
    return result_payload(r, "value")


def result_unwrap_or(r, fallback):
    if need_result(r).tag is ERR:
        return fallback
    return result_payload(r, "value")


def result_map(interp, frame, r, f):
    """A fresh Result either way (section 11: a non-mutating function returns
    a new value); `f` runs only on an Ok."""
    need_result(r)
    need_func(f)
    if r.tag is ERR:
        return result_err(result_payload(r, "error"))
    return result_ok(interp.call(f, [result_payload(r, "value")], (), frame))


def result_map_err(interp, frame, r, f):
    need_result(r)
    need_func(f)
    if r.tag is OK:
        return result_ok(result_payload(r, "value"))
    return result_err(interp.call(f, [result_payload(r, "error")], (), frame))


# ----------------------------------------------------------------------------- Iter

def iter_range(start, end):
    need_int(start)
    need_int(end)

    def run(interp, frame, y):
        need_func(y)
        for i in range(start, end):
            if not need_bool_result(interp.call(y, [i], (), frame)):
                return VOID
        return VOID

    return Native("range", ["yield"], run, ctx=True)


def iter_to_array(interp, frame, s):
    need_func(s)
    out = []

    def collect(v):
        out.append(v)
        return True

    y = YieldFn(collect)
    try:
        interp.call(s, [y], (), frame)
    finally:
        y.done = True
    return out


# ----------------------------------------------------------------------------- sync

def mutex_with_lock(interp, frame, m, f):
    """Acquires, calls `f` with no argument, and releases even when `f`
    panics; the panic itself propagates (section 11, the `with` forms)."""
    need(m, Mutex, "a Mutex")
    need_func(f)
    m.lock.acquire()
    try:
        return interp.call(f, [], (), frame)
    finally:
        m.lock.release()


def wait_group_add(wg, n):
    need(wg, WaitGroup, "a WaitGroup")
    need_int(n)
    with wg.cond:
        if wg.count + n < 0:
            panic("negative counter")
        wg.count += n
        if wg.count == 0:
            wg.cond.notify_all()
    return VOID


def wait_group_done(wg):
    return wait_group_add(wg, -1)


def wait_group_wait(wg):
    need(wg, WaitGroup, "a WaitGroup")
    with wg.cond:
        while wg.count > 0:
            wg.cond.wait()
    return VOID


# ----------------------------------------------------------------------------- the prelude scope

def ant_print(v):
    sys.stdout.write(render(v) + "\n")
    return VOID


def ant_panic(message):
    if type(message) is not str:
        panic("wrong kind: panic needs a string")
    panic(message)


def namespace(name, functions):
    """A record whose fields are natives. `functions` maps a name to
    (parameter names, python function) or (parameter names, function, True)
    for one that needs the interpreter."""
    fields = {}
    for fname, spec in functions.items():
        params, fn = spec[0], spec[1]
        fields[fname] = Native(name + "." + fname, params, fn, ctx=len(spec) > 2)
    return Record(Tag(name), fields)


NAMESPACES = {
    "Array": {
        "length": (["xs"], lambda xs: len(need_array(xs))),
        "isEmpty": (["xs"], lambda xs: not need_array(xs)),
        "get": (["xs", "i"], array_get),
        "push": (["xs", "v"], array_push),
        "pop": (["xs"], array_pop),
        "slice": (["xs", "from", "to"], array_slice),
        "map": (["xs", "f"], array_map, True),
        "filter": (["xs", "f"], array_filter, True),
        "reduce": (["xs", "init", "f"], array_reduce, True),
        "find": (["xs", "f"], array_find, True),
        "some": (["xs", "f"], array_some, True),
        "every": (["xs", "f"], array_every, True),
        "contains": (["xs", "v"], array_contains),
        "sort": (["xs"], array_sort),
        "sortWith": (["xs", "cmp"], array_sort_with, True),
        "entries": (["xs"], array_entries),
        "sum": (["xs"], array_sum),
        "same": (["xs", "ys"], array_same),
    },
    "Map": {
        "get": (["m", "k"], map_get),
        "has": (["m", "k"], map_has),
        "set": (["m", "k", "v"], map_set),
        "size": (["m"], map_size),
        "isEmpty": (["m"], map_is_empty),
        "remove": (["m", "k"], map_remove),
        "same": (["m", "n"], map_same),
    },
    "String": {
        "length": (["s"], string_length),
        "isEmpty": (["s"], string_is_empty),
        "chars": (["s"], string_chars),
        "join": (["parts", "sep"], string_join),
        "toUpper": (["s"], string_to_upper),
        "toLower": (["s"], string_to_lower),
        "toBytes": (["s"], string_to_bytes),
        "fromBytes": (["bytes"], string_from_bytes),
        "toInt": (["s"], string_to_int),
        "from": (["value"], render),
    },
    "Iter": {
        "range": (["from", "to"], iter_range),
        "toArray": (["s"], iter_to_array, True),
    },
    "Bits": {
        "and": (["a", "b"], arith.bits_and),
        "or": (["a", "b"], arith.bits_or),
        "xor": (["a", "b"], arith.bits_xor),
        "not": (["a"], arith.bits_not),
        "shiftLeft": (["a", "n"], arith.bits_shift_left),
        "shiftRight": (["a", "n"], arith.bits_shift_right),
        "popCount": (["a"], arith.bits_pop_count),
        "wrapAdd": (["a", "b"], arith.bits_wrap_add),
        "wrapSub": (["a", "b"], arith.bits_wrap_sub),
        "wrapMul": (["a", "b"], arith.bits_wrap_mul),
    },
    "Result": {
        "ok": (["value"], result_ok),
        "err": (["error"], result_err),
        "isOk": (["r"], result_is_ok),
        "map": (["r", "f"], result_map, True),
        "mapErr": (["r", "f"], result_map_err, True),
        "unwrap": (["r"], result_unwrap),
        "unwrapOr": (["r", "fallback"], result_unwrap_or),
    },
    "Record": {
        "same": (["a", "b"], record_same),
    },
    "Mutex": {
        "new": ([], Mutex),
        "withLock": (["m", "f"], mutex_with_lock, True),
    },
    "WaitGroup": {
        "new": ([], WaitGroup),
        "add": (["wg", "n"], wait_group_add),
        "done": (["wg"], wait_group_done),
        "wait": (["wg"], wait_group_wait),
    },
}

# Type names bound to tags. `bool` and `string` are tags so that calling them
# panics `not a function` (G8).
TAG_NAMES = ("Seq", "Integer", "Number", "Ordered", "Result", "bool", "string", "never")


def make_prelude():
    vars = {}
    for tag in (OK, ERR, ERROR, ENTRY, INDEXED):
        vars[tag.name] = tag
    for name in TAG_NAMES:
        vars[name] = Tag(name)
    for name, kind in CONVERSION_KINDS.items():
        vars[name] = Native(name, ["x"], functools.partial(arith.convert, kind))
    vars["print"] = Native("print", ["value"], ant_print)
    vars["panic"] = Native("panic", ["message"], ant_panic)
    for name, functions in NAMESPACES.items():
        vars[name] = namespace(name, functions)
    return Scope(vars, None, None)
