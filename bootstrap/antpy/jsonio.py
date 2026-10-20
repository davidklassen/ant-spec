"""The Json namespace of stdlib.md: `Json.parse` over Python's json module
and a compact `Json.stringify` written over Ant values (GAPS G39, G45).

Parsing: a number is always a float64 (`parse_int=float`), an object an
AntMap keyed by its strings with the last duplicate winning, an array a
list; null, bools, and strings are already Ant values, so no conversion walk
is needed. Python's parser also accepts `NaN`, `Infinity`, and `-Infinity`,
which JSON does not, and turns a lone `\\uD800` escape into a string that no
UTF-8 encoder accepts; both are an Err. A malformed text is an Err with the
parser's message.

Stringify: no whitespace; numbers as String.from renders them (`1.0` stays
`1.0`, a sized int is bare digits); NaN and infinities as `null`; record
fields in order without the tag; map keys through String.from; `null` for
anything JSON has no form for (functions, tags, void, sync objects, file
handles, a module), and for a container that contains itself.
"""
import json
import math
import re

from antpy.values import KIND, INT_KINDS, FLOAT_KINDS, MODULE_TAG, Record, need_string, ok, err, error
from antpy.equal import AntMap
from antpy.render import render, render_float


# ----------------------------------------------------------------------------- parse

LONE_SURROGATE = re.compile("[\ud800-\udfff]")


def reject_constant(name):
    raise ValueError("not a JSON value: " + name)


def to_map(pairs):
    m = AntMap()
    m.update(pairs)
    return m


def has_lone_surrogate(v):
    """Whether a string anywhere in a parsed value holds a code point in
    U+D800..U+DFFF; only a lone \\u escape in the text produces one, a pair
    of escapes decodes to one code point. The walk keeps its own stack: the
    value is nested as deeply as the parser allowed, and a RecursionError
    here would escape the Result."""
    stack = [v]
    while stack:
        v = stack.pop()
        t = type(v)
        if t is str:
            if LONE_SURROGATE.search(v) is not None:
                return True
        elif t is list:
            stack.extend(v)
        elif t is AntMap:
            for pair in v.items():
                stack.extend(pair)
    return False


def json_parse(text):
    need_string(text)
    try:
        v = json.loads(text, parse_int=float, parse_constant=reject_constant, object_pairs_hook=to_map)
    except (ValueError, RecursionError) as e:
        return err(error(str(e) or "invalid JSON"))
    if "\\u" in text and has_lone_surrogate(v):
        return err(error("lone surrogate escape"))
    return ok(v)


# ----------------------------------------------------------------------------- stringify

def quote(s):
    """A JSON string literal: `"`, `\\`, and control characters escaped,
    everything else literal."""
    return json.dumps(s, ensure_ascii=False)


def json_stringify(value):
    out = []
    emit(value, out, set())
    return "".join(out)


def emit(v, out, active):
    """Appends the JSON text of `v` to the pieces in `out`. `active` holds the
    ids of the containers being emitted (they stay alive while they are in
    it), so that a cycle emits `null` instead of recursing forever."""
    t = type(v)
    k = KIND[t]
    if k == "string":
        out.append(quote(v))
    elif k in INT_KINDS or k == "bool":
        out.append(render(v))
    elif k in FLOAT_KINDS:
        out.append(render_float(v, k == "float32") if math.isfinite(v) else "null")
    elif t is list or t is AntMap or (t is Record and v.tag is not MODULE_TAG):
        emit_container(v, out, active)
    else:
        out.append("null")


def emit_container(v, out, active):
    if id(v) in active:
        out.append("null")
        return
    active.add(id(v))
    t = type(v)
    if t is list:
        out.append("[")
        for i, x in enumerate(v):
            if i:
                out.append(",")
            emit(x, out, active)
        out.append("]")
    else:
        if t is AntMap:
            entries = [(render(key), x) for key, x in v.items()]     # the key's top-level String.from text
        else:
            entries = v.fields.items()
        out.append("{")
        for i, (name, x) in enumerate(entries):
            if i:
                out.append(",")
            out.append(quote(name))
            out.append(":")
            emit(x, out, active)
        out.append("}")
    active.discard(id(v))


JSON = {
    "parse": (["text"], json_parse),
    "stringify": (["value"], json_stringify),
}
