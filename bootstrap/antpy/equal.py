"""Structural equality (section 12, "Equality"), the structural hash that
agrees with it, and the map keyed by them (section 4, "Arrays and maps")."""
from antpy.values import (
    KIND, NUMBER_KINDS, FLOAT_KINDS, MISSING, Record, panic,
)


class AntMap:
    """A hash map keyed by `==`: buckets from the structural hash of a key to
    [key, value] pairs. The hash is taken when a key is inserted (section 4)."""
    __slots__ = ("buckets", "size")

    def __init__(self):
        self.buckets = {}
        self.size = 0

    def find(self, key):
        bucket = self.buckets.get(hash_value(key))
        if bucket is not None:
            for pair in bucket:
                if eq(pair[0], key):
                    return pair
        return None

    def get(self, key, default=None):
        pair = self.find(key)
        return default if pair is None else pair[1]

    def has(self, key):
        return self.find(key) is not None

    def set(self, key, value):
        h = hash_value(key)
        bucket = self.buckets.get(h)
        if bucket is None:
            self.buckets[h] = [[key, value]]
        else:
            for pair in bucket:
                if eq(pair[0], key):
                    pair[1] = value
                    return
            bucket.append([key, value])
        self.size += 1

    def remove(self, key):
        """Removes the entry and returns its value, or MISSING."""
        h = hash_value(key)
        bucket = self.buckets.get(h)
        if bucket is not None:
            for i, pair in enumerate(bucket):
                if eq(pair[0], key):
                    try:
                        del bucket[i]
                        if not bucket:
                            del self.buckets[h]
                    except (IndexError, KeyError):    # removed by another goroutine meanwhile
                        return MISSING
                    self.size -= 1
                    return pair[1]
        return MISSING

    def items(self):
        """A snapshot list of (key, value) tuples. The bucket list is copied
        first: an insert from another goroutine must not raise "dictionary
        changed size during iteration" (section 12: a data race cannot crash
        the interpreter)."""
        return [(pair[0], pair[1]) for bucket in list(self.buckets.values()) for pair in bucket]


KIND[AntMap] = "map"

# Kinds whose values Python's == compares the way section 12 asks: numbers by
# value with IEEE rules, strings by code point, bool/null/void by value.
PRIMITIVE_KINDS = NUMBER_KINDS | frozenset(("bool", "string", "null", "void"))
REFERENCE_KINDS = frozenset(("func", "tag", "sync"))


def eq(a, b, pairs=None):
    """Structural equality. Never panics. `pairs` holds the ids of the
    container pairs under comparison; a repeat counts as equal (section 4)."""
    ta, tb = type(a), type(b)
    ka, kb = KIND[ta], KIND[tb]
    if ka != kb:
        return False
    if ka in PRIMITIVE_KINDS:
        return a == b
    if ka in REFERENCE_KINDS:
        return a is b
    key = (id(a), id(b))
    if pairs is None:
        pairs = set()
    elif key in pairs:
        return True
    pairs.add(key)
    try:
        if ta is list:
            if len(a) != len(b):
                return False
            for x, y in zip(a, b):
                if not eq(x, y, pairs):
                    return False
            return True
        if ta is Record:
            if a.tag is not b.tag or a.fields.keys() != b.fields.keys():
                return False
            for name, x in a.fields.items():
                if not eq(x, b.fields[name], pairs):
                    return False
            return True
        # map
        if a.size != b.size:
            return False
        for k, v in a.items():
            pair = b.find(k)
            if pair is None or not eq(v, pair[1], pairs):
                return False
        return True
    finally:
        pairs.discard(key)


MAX_HASH_DEPTH = 200


def hash_value(v, depth=0):
    """A hash consistent with eq: equal values hash alike."""
    if depth > MAX_HASH_DEPTH:
        panic("map key too deep (cyclic?)")
    t = type(v)
    k = KIND[t]
    if k in NUMBER_KINDS:
        if k in FLOAT_KINDS and v != v:
            return hash((k, "nan"))     # a NaN key is never found (G13), but hashes deterministically
        return hash((k, v))
    if k in PRIMITIVE_KINDS:
        return hash((k, v))
    if k in REFERENCE_KINDS:
        return hash((k, id(v)))
    depth += 1
    if t is list:
        return hash(("array", tuple(hash_value(x, depth) for x in v)))
    if t is Record:
        return hash(("record", id(v.tag), frozenset((n, hash_value(x, depth)) for n, x in v.fields.items())))
    return hash(("map", frozenset((hash_value(a, depth), hash_value(b, depth)) for a, b in v.items())))
