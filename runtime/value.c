/* Kind names and the small checks generated code and the natives call directly. */
#include "ant.h"

/* Every kind by name, in kind order, then the two aliases `int` and `byte`
 * (G48): the conversion functions are the numeric rows. */
static const AntKindName KINDS[] = {
  { "int8", K_INT8 }, { "int16", K_INT16 }, { "int32", K_INT32 }, { "int64", K_INT64 },
  { "uint8", K_UINT8 }, { "uint16", K_UINT16 }, { "uint32", K_UINT32 }, { "uint64", K_UINT64 },
  { "float32", K_FLOAT32 }, { "float64", K_FLOAT64 }, { "bool", K_BOOL }, { "string", K_STRING },
  { "null", K_NULL }, { "void", K_VOID }, { "array", K_ARRAY }, { "map", K_MAP },
  { "record", K_RECORD }, { "func", K_FUNC }, { "tag", K_TAG }, { "sync", K_SYNC },
  { "unbound", K_UNBOUND }, { "uninit", K_UNINIT }, { "absent", K_ABSENT },
  { "int", K_INT64 }, { "byte", K_UINT8 },
};

const AntKindName *ant_kind_table(int *n) {
  *n = ANT_LEN(KINDS);
  return KINDS;
}

const char *ant_kind_name(AntKind k) {
  return KINDS[k].name;
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
  ant_truth(v);
  return v;
}

AntValue ant_is_kind(AntValue v, AntKind k) {
  return ant_bool(v.kind == k);
}

/* A read that can see the K_UNBOUND sentinel (a boxed local, a prelude
 * global, a name nothing binds): the value, or `unbound name`. */
AntValue ant_bound(AntValue v, const char *name) {
  if (v.kind == K_UNBOUND) ant_panicf("unbound name: %s", name);
  return v;
}

bool ant_wrong_kind(const char *what) {
  ant_panicf("wrong kind: expected %s", what);
  return false;
}

bool ant_need_kind(AntValue v, AntKind k, const char *what) {
  return v.kind == k || ant_wrong_kind(what);
}

int64_t ant_need_int(AntValue v) {
  return ant_need_kind(v, K_INT64, "an int") ? v.as.i : 0;
}
