/* Kind names and the small checks generated code calls directly. */
#include "ant.h"

static const char *const KIND_NAMES[] = {
  "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
  "float32", "float64", "bool", "string", "null", "void", "array", "map",
  "record", "func", "tag", "sync", "unbound", "uninit", "absent",
};

const char *ant_kind_name(AntKind k) {
  return KIND_NAMES[k];
}

bool ant_is_number(AntKind k) {
  return k <= K_FLOAT64;
}

bool ant_is_int(AntKind k) {
  return k <= K_UINT64;
}

/* The operand of `!`, `&&`, `||`, `if`, `while`: a bool, or `not a bool`.
 * A panic answers false; the caller tests the unwind flag. */
bool ant_truth(AntValue v) {
  if (v.kind != K_BOOL) {
    ant_panic("not a bool");
    return false;
  }
  return v.as.b;
}

AntValue ant_check_bool(AntValue v) {
  if (v.kind != K_BOOL) ant_panic("not a bool");
  return v;
}

AntValue ant_is_kind(AntValue v, AntKind k) {
  return ant_bool(v.kind == k);
}

/* A prelude global read at a use site: the value, or `unbound name`. */
AntValue ant_global(AntValue g, const char *name) {
  if (g.kind == K_UNBOUND) ant_panicf("unbound name: %s", name);
  return g;
}
