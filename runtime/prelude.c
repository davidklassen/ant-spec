/* The prelude (section 11): tags, conversions, print, panic, and the
 * namespace records, all reachable by name through ant_prelude_get. */
#include <string.h>
#include "ant.h"

AntValue ant_tag_ok, ant_tag_err, ant_tag_error, ant_tag_entry, ant_tag_indexed;
Sym S_value, S_error, S_key, S_x, S_yield;
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

/* ---------------------------------------------------------------- globals */

typedef struct { const char *name; AntValue value; } Global;
static Global *globals;
static int nglobals, capglobals;

void ant_prelude_define(const char *name, AntValue v) {
  ANT_PUSH(globals, nglobals, capglobals, 64, ((Global){ name, v }));
}

AntValue ant_prelude_get(const char *name) {
  for (int i = 0; i < nglobals; i++) {
    if (strcmp(globals[i].name, name) == 0) return globals[i].value;
  }
  return ANT_UNBOUND;
}

/* A namespace: a record tagged with its name whose fields are its members,
 * defined as the prelude global of that name. */
AntValue ant_namespace(const char *name, int n, const AntMember *members) {
  Sym *syms = ant_alloc_atomic(sizeof(Sym) * (size_t)(n ? n : 1));
  AntValue *values = ant_alloc(sizeof(AntValue) * (size_t)(n ? n : 1));
  for (int i = 0; i < n; i++) {
    const AntMember *m = &members[i];
    syms[i] = ant_sym(m->member);
    values[i] = m->code ? ant_native(ant_fmt("%s.%s", name, m->member), m->params, m->code, NULL) : m->value;
  }
  AntValue tag = ant_tag_new(name);
  AntValue ns = ant_record_make(tag.as.p, ant_shape_intern(n, syms), values);
  ant_prelude_define(name, ns);
  return ns;
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
  S_yield = ant_sym("yield");
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

  /* the conversion functions: one per numeric kind name, the aliases included */
  int nkinds;
  const AntKindName *kinds = ant_kind_table(&nkinds);
  for (int i = 0; i < nkinds; i++) {
    if (ant_is_number(kinds[i].kind)) ant_prelude_define(kinds[i].name, ant_conversion(kinds[i].name, kinds[i].kind));
  }

  ant_prelude_define("print", ant_native("print", "value", ant_native_print, NULL));
  ant_prelude_define("panic", ant_native("panic", "message", ant_native_panic, NULL));

  static void (*const namespaces[])(void) = {
    ant_ns_array, ant_ns_map, ant_ns_string, ant_ns_iter, ant_ns_math, ant_ns_bits, ant_ns_result, ant_ns_record,
    ant_ns_mutex, ant_ns_rwmutex, ant_ns_waitgroup, ant_ns_once, ant_ns_cond, ant_ns_atomic,
    ant_ns_os, ant_ns_time, ant_ns_file, ant_ns_http, ant_ns_json,
  };
  for (int i = 0; i < ANT_LEN(namespaces); i++) namespaces[i]();
}
