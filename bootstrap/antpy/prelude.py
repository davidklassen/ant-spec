"""The prelude scope (section 11): tags, conversions, print, panic, and the
library namespaces. Each namespace is a record of Native functions, so
`Array.map` is an ordinary field read (section 10).

A native is declared with its parameter names (named arguments resolve against
them). One marked ctx=True receives (interp, frame, *args) and may call back
into Ant with interp.call."""
import functools
import sys

from antpy.values import (
    VOID, Scope, Record, Tag, Native, YieldFn, Mutex, WaitGroup, UInt8, CONVERSION_KINDS, KIND, panic,
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


def string_length(s):
    return len(need_string(s))


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


def result_unwrap(r):
    if need_result(r).tag is ERR:
        panic("unwrap on Err")
    return r.fields["value"]


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
        "same": (["xs", "ys"], array_same),
    },
    "Map": {
        "get": (["m", "k"], map_get),
        "has": (["m", "k"], map_has),
        "set": (["m", "k", "v"], map_set),
        "size": (["m"], map_size),
    },
    "String": {
        "length": (["s"], string_length),
        "chars": (["s"], string_chars),
        "join": (["parts", "sep"], string_join),
        "toBytes": (["s"], string_to_bytes),
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
        "unwrap": (["r"], result_unwrap),
    },
    "Record": {
        "same": (["a", "b"], record_same),
    },
    "Mutex": {
        "new": ([], Mutex),
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
