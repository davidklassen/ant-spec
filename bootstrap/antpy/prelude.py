"""The prelude scope (section 11): tags, conversions, print, panic, and the
library namespaces. Each namespace is a record of Native functions (and
constants like `Math.PI`), so `Array.map` is an ordinary field read (section 10).

A native is declared with its parameter names (named arguments resolve against
them). One marked ctx=True receives (interp, frame, *args) and may call back
into Ant with interp.call. The data namespaces (Array, Map, String, Iter,
Math) live in library.py and the sync objects and Time in sync.py; this
module holds Result, Record, Bits, and the scope itself."""
import functools
import sys

from antpy.values import (
    VOID, Scope, Record, Tag, Native,
    OK, ERR, ERROR, ENTRY, INDEXED, CONVERSION_KINDS, panic,
)
from antpy import arith
from antpy.render import render
from antpy.library import (
    need_func, need_record, record_field,
    ARRAY, MAP, STRING, ITER, MATH,
)
from antpy.sync import MUTEX, RWMUTEX, WAITGROUP, ONCE, COND, ATOMIC, TIME


# ----------------------------------------------------------------------------- Result and Record

def need_result(v):
    if type(v) is not Record or (v.tag is not OK and v.tag is not ERR):
        panic("wrong kind: expected a Result")
    return v


def result_ok(v):
    return Record(OK, {"value": v})


def result_err(e):
    return Record(ERR, {"error": e})


def result_is_ok(r):
    return need_result(r).tag is OK


def result_unwrap(r):
    if need_result(r).tag is ERR:
        panic("unwrap on Err")
    return record_field(r, "value")


def result_unwrap_or(r, fallback):
    if need_result(r).tag is ERR:
        return fallback
    return record_field(r, "value")


def result_map(interp, frame, r, f):
    """A fresh Result either way (section 11: a non-mutating function returns
    a new value); `f` runs only on an Ok."""
    need_result(r)
    need_func(f)
    if r.tag is ERR:
        return result_err(record_field(r, "error"))
    return result_ok(interp.call(f, [record_field(r, "value")], (), frame))


def result_map_err(interp, frame, r, f):
    need_result(r)
    need_func(f)
    if r.tag is OK:
        return result_ok(record_field(r, "value"))
    return result_err(interp.call(f, [record_field(r, "error")], (), frame))


def record_same(a, b):
    return need_record(a) is need_record(b)


# ----------------------------------------------------------------------------- the prelude scope

def ant_print(v):
    sys.stdout.write(render(v) + "\n")
    return VOID


def ant_panic(message):
    if type(message) is not str:
        panic("wrong kind: panic needs a string")
    panic(message)


def namespace(name, members):
    """A record of the namespace's members. A (parameter names, python
    function) or (parameter names, function, True) spec becomes a native (True:
    one that needs the interpreter); any other value is a constant field."""
    fields = {}
    for fname, spec in members.items():
        if type(spec) is tuple:
            fields[fname] = Native(name + "." + fname, spec[0], spec[1], ctx=len(spec) > 2)
        else:
            fields[fname] = spec
    return Record(Tag(name), fields)


NAMESPACES = {
    "Array": ARRAY,
    "Map": MAP,
    "String": STRING,
    "Iter": ITER,
    "Math": MATH,
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
    "Mutex": MUTEX,
    "RwMutex": RWMUTEX,
    "WaitGroup": WAITGROUP,
    "Once": ONCE,
    "Cond": COND,
    "Atomic": ATOMIC,
    "Time": TIME,
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
    for name, members in NAMESPACES.items():
        vars[name] = namespace(name, members)
    return Scope(vars, None, None)
