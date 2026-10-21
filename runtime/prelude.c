/* The prelude (section 11): tags, conversions, print, panic, and the
 * namespace records, all reachable by name through ant_prelude_get. */
#include <string.h>
#include "ant.h"

AntValue ant_tag_ok, ant_tag_err, ant_tag_error, ant_tag_entry, ant_tag_indexed;
Sym S_value, S_error, S_key, S_x;
static const AntShape *shape_ok, *shape_err, *shape_error, *shape_entry, *shape_indexed;

AntValue ant_ok(AntValue v) { return ant_record_make(ant_tag_ok.as.p, shape_ok, &v); }
AntValue ant_err(AntValue e) { return ant_record_make(ant_tag_err.as.p, shape_err, &e); }

AntValue ant_error(const char *message) {
  AntValue m = ant_str_cstr(message);
  return ant_record_make(ant_tag_error.as.p, shape_error, &m);
}

AntValue ant_entry(AntValue k, AntValue v) {
  AntValue fields[2] = { k, v };
  return ant_record_make(ant_tag_entry.as.p, shape_entry, fields);
}

AntValue ant_indexed(int64_t i, AntValue v) {
  AntValue fields[2] = { ant_int(i), v };
  return ant_record_make(ant_tag_indexed.as.p, shape_indexed, fields);
}

/* A namespace: a record tagged with its name whose fields are its members. */
AntValue ant_namespace(const char *name, int n, const AntMember *members) {
  Sym *syms = ant_alloc_atomic(sizeof(Sym) * (size_t)(n ? n : 1));
  AntValue *values = ant_alloc(sizeof(AntValue) * (size_t)(n ? n : 1));
  size_t prefix = strlen(name);
  for (int i = 0; i < n; i++) {
    const AntMember *m = &members[i];
    syms[i] = ant_sym(m->member);
    if (!m->code) { values[i] = m->value; continue; }
    size_t len = strlen(m->member);
    char *full = ant_alloc_atomic(prefix + 1 + len + 1);
    memcpy(full, name, prefix);
    full[prefix] = '.';
    memcpy(full + prefix + 1, m->member, len + 1);
    values[i] = ant_native(full, m->params, m->code, NULL);
  }
  AntValue tag = ant_tag_new(name);
  return ant_record_make(tag.as.p, ant_shape_intern(n, syms), values);
}

/* ---------------------------------------------------------------- globals */

typedef struct { const char *name; AntValue value; } Global;
static Global *globals;
static int nglobals, capglobals;

void ant_prelude_define(const char *name, AntValue v) {
  if (nglobals == capglobals) {
    capglobals = capglobals ? capglobals * 2 : 64;
    globals = ant_realloc(globals, sizeof(Global) * (size_t)capglobals);
  }
  globals[nglobals].name = name;
  globals[nglobals].value = v;
  nglobals++;
}

AntValue ant_prelude_get(const char *name) {
  for (int i = 0; i < nglobals; i++) {
    if (strcmp(globals[i].name, name) == 0) return globals[i].value;
  }
  return ANT_UNBOUND;
}

static const AntShape *shape1(const char *a) {
  Sym s = ant_sym(a);
  return ant_shape_intern(1, &s);
}

static const AntShape *shape2(const char *a, const char *b) {
  Sym s[2] = { ant_sym(a), ant_sym(b) };
  return ant_shape_intern(2, s);
}

static void define_tag(const char *name, AntValue *slot) {
  AntValue t = ant_tag_new(name);
  if (slot) *slot = t;
  ant_prelude_define(name, t);
}

void ant_prelude_init(void) {
  S_value = ant_sym("value");
  S_error = ant_sym("error");
  S_key = ant_sym("key");
  S_x = ant_sym("x");
  shape_ok = shape1("value");
  shape_err = shape1("error");
  shape_error = shape1("message");
  shape_entry = shape2("key", "value");
  shape_indexed = shape2("index", "value");

  define_tag("Ok", &ant_tag_ok);
  define_tag("Err", &ant_tag_err);
  define_tag("Error", &ant_tag_error);
  define_tag("Entry", &ant_tag_entry);
  define_tag("Indexed", &ant_tag_indexed);
  /* type names bound to tags; `bool` and `string` so that calling them panics `not a function` (G8) */
  static const char *const tag_names[] = { "Seq", "Integer", "Number", "Ordered", "bool", "string", "never" };
  for (int i = 0; i < ANT_LEN(tag_names); i++) define_tag(tag_names[i], NULL);

  static const char *const conv_names[] = {
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32", "float64", "int", "byte",
  };
  static const AntKind conv_kinds[] = {
    K_INT8, K_INT16, K_INT32, K_INT64, K_UINT8, K_UINT16, K_UINT32, K_UINT64, K_FLOAT32, K_FLOAT64, K_INT64, K_UINT8,
  };
  for (int i = 0; i < ANT_LEN(conv_names); i++) ant_prelude_define(conv_names[i], ant_conversion(conv_names[i], conv_kinds[i]));

  ant_prelude_define("print", ant_native("print", "value", ant_native_print, NULL));
  ant_prelude_define("panic", ant_native("panic", "message", ant_native_panic, NULL));

  ant_prelude_define("Array", ant_ns_array());
  ant_prelude_define("Map", ant_ns_map());
  ant_prelude_define("String", ant_ns_string());
  ant_prelude_define("Iter", ant_ns_iter());
  ant_prelude_define("Math", ant_ns_math());
  ant_prelude_define("Bits", ant_ns_bits());
  ant_prelude_define("Result", ant_ns_result());
  ant_prelude_define("Record", ant_ns_record());
  ant_prelude_define("Mutex", ant_ns_mutex());
  ant_prelude_define("RwMutex", ant_ns_rwmutex());
  ant_prelude_define("WaitGroup", ant_ns_waitgroup());
  ant_prelude_define("Once", ant_ns_once());
  ant_prelude_define("Cond", ant_ns_cond());
  ant_prelude_define("Atomic", ant_ns_atomic());
  ant_prelude_define("Os", ant_ns_os());
  ant_prelude_define("Time", ant_ns_time());
  ant_prelude_define("File", ant_ns_file());
  ant_prelude_define("Http", ant_ns_http());
  ant_prelude_define("Json", ant_ns_json());
}
