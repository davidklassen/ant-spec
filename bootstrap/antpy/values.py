"""Run-time values (ant-lang.md section 12, "Values").

Every Ant value is a Python object whose exact type names its kind, so that
`KIND[type(v)]` is one dictionary lookup. Python's `bool` is a subclass of
`int` and the sized numeric kinds below subclass `int` and `float`, so kind
tests always use `type(v) is ...`, never `isinstance`.
"""
import math
import struct


class AntPanic(BaseException):
    """A panic unwinding the current goroutine. `trace` is filled at the
    innermost statement runner and is a list of "name (path:line)" strings."""

    def __init__(self, message, trace=None):
        super().__init__(message)
        self.message = message
        self.trace = trace


def panic(message):
    raise AntPanic(message)


# ----------------------------------------------------------------------------- scalars

class Void:
    __slots__ = ()

    def __repr__(self):
        return "void"


VOID = Void()


class Sentinel:
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name


UNINIT = Sentinel("UNINIT")     # a top-level var whose initializer has not run
MISSING = Sentinel("MISSING")   # internal "no such key"; never stored in a scope


class Int8(int):
    __slots__ = ()


class Int16(int):
    __slots__ = ()


class Int32(int):
    __slots__ = ()


class UInt8(int):
    __slots__ = ()


class UInt16(int):
    __slots__ = ()


class UInt32(int):
    __slots__ = ()


class UInt64(int):
    __slots__ = ()


def round_f32(x):
    """Round a float to float32 precision. Raises OverflowError when a finite
    value rounds to infinity (the conversion panics; arithmetic gives inf)."""
    return struct.unpack("f", struct.pack("f", x))[0]


class Float32(float):
    __slots__ = ()

    def __new__(cls, x):
        try:
            r = round_f32(x)
        except OverflowError:
            r = math.copysign(math.inf, x)
        return float.__new__(cls, r)


# ----------------------------------------------------------------------------- references

class Record:
    __slots__ = ("tag", "fields")

    def __init__(self, tag, fields):
        self.tag = tag
        self.fields = fields      # dict in creation order


class Tag:
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name


MODULE_TAG = Tag("Module")

# The prelude's record tags (section 11), shared by the library and the evaluator.
OK = Tag("Ok")
ERR = Tag("Err")
ERROR = Tag("Error")
ENTRY = Tag("Entry")
INDEXED = Tag("Indexed")


class Closure:
    """A declared function, an anonymous function, or a lambda."""
    __slots__ = ("name", "params", "names", "body", "scope", "is_lambda", "module")

    def __init__(self, name, params, names, body, scope, is_lambda, module):
        self.name = name              # for traces: the declared name, "func", or "lambda"
        self.params = params          # [(name, default_node_or_None)]
        self.names = names            # the parameter names, in order, computed once by the lowering pass
        self.body = body              # a lowered Block node
        self.scope = scope            # the captured scope
        self.is_lambda = is_lambda
        self.module = module          # path, for traces


class Native:
    """A library function. `params` names the parameters so that named
    arguments resolve; with `ctx` the function also receives the interpreter
    and the caller's frame (needed to call back into Ant)."""
    __slots__ = ("name", "params", "fn", "ctx")

    def __init__(self, name, params, fn, ctx=False):
        self.name = name
        self.params = params
        self.fn = fn
        self.ctx = ctx


class YieldFn:
    """The `yield` function a `for` loop hands to a sequence. `body(v)` runs
    one iteration and returns True to continue or False after a break."""
    __slots__ = ("body", "done")

    def __init__(self, body):
        self.body = body
        self.done = False


# ----------------------------------------------------------------------------- scopes and frames

class Scope:
    """A lexical scope. `vars` is the binding dict (shared with the module
    record for module scopes); `frame` the activation it belongs to; `defers`
    the block's defer list or None; `imports` the live name imports of a
    module scope (local name -> (exporter vars, exported name)) or None."""
    __slots__ = ("vars", "parent", "frame", "defers", "imports")

    def __init__(self, vars, parent, frame):
        self.vars = vars
        self.parent = parent
        self.frame = frame
        self.defers = None
        self.imports = None


class Frame:
    """A function activation, for stack traces: the chain through `parent`
    is the trace. `line` is the statement being evaluated."""
    __slots__ = ("name", "path", "line", "parent")

    def __init__(self, name, path, line, parent):
        self.name = name
        self.path = path
        self.line = line
        self.parent = parent


# ----------------------------------------------------------------------------- kinds

KIND = {
    int: "int64", Int8: "int8", Int16: "int16", Int32: "int32",
    UInt8: "uint8", UInt16: "uint16", UInt32: "uint32", UInt64: "uint64",
    float: "float64", Float32: "float32",
    bool: "bool", str: "string", type(None): "null", Void: "void",
    list: "array", Record: "record", Tag: "tag",
    Closure: "func", Native: "func", YieldFn: "func",
}
# equal.py adds its AntMap class under "map" (the map's lookup needs
# structural equality and hashing), sync.py its six classes under "sync", and
# fileio.py its File under "file".

INT_WIDTH = {
    "int8": 8, "int16": 16, "int32": 32, "int64": 64,
    "uint8": 8, "uint16": 16, "uint32": 32, "uint64": 64,
}
SIGNED_KINDS = frozenset(("int8", "int16", "int32", "int64"))
INT_RANGE = {
    kind: (-(1 << (width - 1)), (1 << (width - 1)) - 1) if kind in SIGNED_KINDS else (0, (1 << width) - 1)
    for kind, width in INT_WIDTH.items()
}
INT_CLASS = {kind: cls for cls, kind in KIND.items() if kind in INT_WIDTH}
INT_KINDS = frozenset(INT_WIDTH)
FLOAT_KINDS = frozenset(("float32", "float64"))
NUMBER_KINDS = INT_KINDS | FLOAT_KINDS
ORDERED_KINDS = NUMBER_KINDS | frozenset(("string",))

# The conversion functions, by name. `int` and `byte` are aliases (section 4).
CONVERSION_KINDS = {
    "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
    "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
    "float32": "float32", "float64": "float64", "int": "int64", "byte": "uint8",
}

FUNC_TYPES = (Closure, Native, YieldFn)


# ----------------------------------------------------------------------------- argument checks

# The kind checks of the library functions (section 12: an argument of a kind
# the function does not take panics `wrong kind`). Only `type(v) is ...`:
# bool subclasses int and Float32 subclasses float. The map check lives in
# equal.py, where the map class is.

def need(v, t, what):
    if type(v) is not t:
        panic("wrong kind: expected " + what)
    return v


def need_array(v):
    return need(v, list, "an array")


def need_string(v):
    return need(v, str, "a string")


def need_int(v):
    return need(v, int, "an int")


def need_record(v):
    return need(v, Record, "a record")


def need_float64(v):
    return need(v, float, "a float64")     # Float32 is its own type, so a float32 is `wrong kind`


def need_number(v):
    if KIND[type(v)] not in NUMBER_KINDS:
        panic("wrong kind: expected a number")
    return v


def need_ordered(v):
    if KIND[type(v)] not in ORDERED_KINDS:
        panic("wrong kind: expected a number or a string")
    return v


def need_func(v):
    if type(v) not in FUNC_TYPES:
        panic("wrong kind: expected a function")
    return v


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


# ----------------------------------------------------------------------------- value-level primitives

# The prelude's record shapes (sections 8 and 11), shared by the evaluator
# and the library: `Ok{ value }`, `Err{ error }`, the `Error{ message }` the
# I/O namespaces fail with, and the `Entry{ key, value }` of a map.

def ok(v):
    return Record(OK, {"value": v})


def err(e):
    return Record(ERR, {"error": e})


def error(message):
    return Record(ERROR, {"message": message})


def entry(k, v):
    return Record(ENTRY, {"key": k, "value": v})


def record_field(r, name):
    """A field read that panics `no field` when the field is absent (section
    12, `a.b`): an unchecked program can build an `Ok{}` without its `value`."""
    v = r.fields.get(name, MISSING)
    if v is MISSING:
        panic("no field: " + name)
    return v


def live_elements(xs):
    """The elements of an array iterated by index (section 12, `for`): the
    length is re-read at each step, so an element appended during the loop is
    visited, and one popped by another goroutine ends the loop."""
    i = 0
    while i < len(xs):
        try:
            v = xs[i]
        except IndexError:
            return
        i += 1
        yield v


def drive(interp, frame, source, body):
    """Runs the sequence `source` with a fresh yield whose `body(v)` returns
    True to continue. The yield is done once the source returns (section 12,
    `for`), so a late call panics `yield after done`."""
    y = YieldFn(body)
    try:
        interp.call(source, [y], (), frame)
    finally:
        y.done = True
    return VOID
