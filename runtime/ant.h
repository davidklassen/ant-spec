/* The Ant runtime (docs/runtime-design.md). Everything the code `antc build`
 * emits calls is declared here; the runtime's own C files include it too.
 * This header compiles with nothing but the C standard library: gc.h is
 * included only by alloc.c. */
#ifndef ANT_H
#define ANT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- values */

typedef enum {
  K_INT8, K_INT16, K_INT32, K_INT64, K_UINT8, K_UINT16, K_UINT32, K_UINT64,
  K_FLOAT32, K_FLOAT64, K_BOOL, K_STRING, K_NULL, K_VOID, K_ARRAY, K_MAP,
  K_RECORD, K_FUNC, K_TAG, K_SYNC,
  K_UNBOUND,   /* a hoisted local before its `var` ran */
  K_UNINIT,    /* a module var before its initializer ran */
  K_ABSENT     /* an unfilled parameter slot */
} AntKind;

typedef struct {
  uint32_t kind;
  union { int64_t i; uint64_t u; double f; float f32; bool b; void *p; } as;
} AntValue;

typedef uint32_t Sym;
typedef struct { int n; const Sym *names; } AntShape;
typedef struct { int64_t nbytes, ncps; uint32_t hash; bool ascii; char data[]; } AntString;
typedef struct { const char *name; } AntTag;
typedef struct { AntTag *tag; const AntShape *shape; AntValue fields[]; } AntRecord;
typedef struct { int64_t len, cap; AntValue *data; } AntArray;
typedef struct AntMap AntMap;
typedef struct { AntValue v; } AntBox;
typedef struct { const char *name; } AntSync;   /* the first field of every sync object */

typedef struct AntFuncInfo AntFuncInfo;
typedef struct AntFrame { const AntFuncInfo *info; int line; struct AntFrame *parent; } AntFrame;
typedef struct AntFunc AntFunc;
typedef AntValue (*AntNative)(AntFunc *self, int argc, AntValue *argv);
struct AntFuncInfo {
  const char *name, *file;
  int line, nparams;
  const Sym *params;
  const bool *hasDefault;    /* NULL: no parameter has a default */
  bool isLambda;
};
struct AntFunc {
  AntNative code;
  const AntFuncInfo *info;
  AntFrame *ret;             /* `for` bodies: the enclosing function's frame */
  void *data;                /* native state */
  int nfree;
  AntBox *free[];
};

typedef struct { int n, cap; AntValue *items; } AntDefers;
typedef struct { const AntShape *shape; int idx; } AntFieldCache;

/* ---------------------------------------------------------------- unwinding */

enum { U_NONE = 0, U_RETURN = 1, U_PANIC = 2 };
typedef struct { int n; char **lines; } AntTrace;
typedef struct { int mode; AntValue value; AntFrame *target; AntTrace *trace; } AntUnwind;
typedef struct { void *a, *b; } AntPair;
typedef struct {
  AntUnwind unwind;
  AntFrame *top;
  AntPair *pairs; int npairs, cappairs;   /* pairs under structural comparison */
} AntGoroutine;

extern AntGoroutine *ant_g;

static inline void ant_frame_push(AntFrame *fr, const AntFuncInfo *info) {
  fr->info = info;
  fr->line = info->line;
  fr->parent = ant_g->top;
  ant_g->top = fr;
}

static inline void ant_frame_pop(AntFrame *fr) {
  ant_g->top = fr->parent;
}

/* The function epilogue: consumes a `return` aimed at this frame. */
static inline AntValue ant_finish(AntFrame *fr, AntValue result) {
  AntUnwind *u = &ant_g->unwind;
  if (u->mode == U_RETURN && u->target == fr) {
    u->mode = U_NONE;
    return u->value;
  }
  return result;
}

#define CHK(e, L) ({ AntValue _v = (e); if (ant_g->unwind.mode) goto L; _v; })
#define CHKB(e, L) ({ bool _b = (e); if (ant_g->unwind.mode) goto L; _b; })
#define ANT_SAFEPOINT() ((void)0)

/* ---------------------------------------------------------------- constructors */

static inline AntValue ant_mk(AntKind k) { AntValue v; v.kind = k; v.as.i = 0; return v; }
static inline AntValue ant_int(int64_t i) { AntValue v; v.kind = K_INT64; v.as.i = i; return v; }
static inline AntValue ant_float(double f) { AntValue v; v.kind = K_FLOAT64; v.as.f = f; return v; }
static inline AntValue ant_bool(bool b) { AntValue v; v.kind = K_BOOL; v.as.i = 0; v.as.b = b; return v; }
static inline AntValue ant_ptr(AntKind k, void *p) { AntValue v; v.kind = k; v.as.p = p; return v; }
#define ANT_VOID ant_mk(K_VOID)
#define ANT_NULL ant_mk(K_NULL)
#define ANT_TRUE ant_bool(true)
#define ANT_FALSE ant_bool(false)
#define ANT_UNBOUND ant_mk(K_UNBOUND)
#define ANT_UNINIT ant_mk(K_UNINIT)
#define ANT_ABSENT ant_mk(K_ABSENT)

/* alloc.c */
void *ant_alloc(size_t n);
void *ant_alloc_atomic(size_t n);
void *ant_realloc(void *p, size_t n);
void ant_gc_init(void);
void ant_run_on_big_stack(void *(*fn)(void *), void *arg);

/* value.c */
const char *ant_kind_name(AntKind k);
bool ant_is_number(AntKind k);
bool ant_is_int(AntKind k);
bool ant_truth(AntValue v);                 /* `not a bool` sets a panic and answers false */
AntValue ant_check_bool(AntValue v);        /* the value, or `not a bool` */
AntValue ant_is_kind(AntValue v, AntKind k);
AntValue ant_global(AntValue g, const char *name);   /* `unbound name` for K_UNBOUND */

/* symbol.c */
void ant_symbols_init(int n, const char *const *names);
Sym ant_sym(const char *name);
const char *ant_sym_name(Sym s);
const AntShape *ant_shape_intern(int n, const Sym *names);

/* string.c */
typedef struct { char *data; int64_t len, cap; } AntSb;
AntValue ant_str_lit(const char *bytes, int64_t n);
AntValue ant_str_new(const char *bytes, int64_t n);
AntValue ant_str_cstr(const char *s);
AntString *ant_str(AntValue v);             /* the string, or `wrong kind` (NULL) */
AntValue ant_str_concat(AntValue a, AntValue b);
int ant_str_cmp(AntString *a, AntString *b);
bool ant_str_eq(AntString *a, AntString *b);
uint32_t ant_str_hash(AntString *s);
int ant_utf8_decode(const char *p, int64_t n, int32_t *cp);   /* bytes consumed */
bool ant_utf8_valid(const char *p, int64_t n);
AntValue ant_str_slice_cps(AntString *s, int64_t from, int64_t to);
void ant_sb_init(AntSb *sb);
void ant_sb_append(AntSb *sb, const char *p, int64_t n);
void ant_sb_append_cstr(AntSb *sb, const char *p);
void ant_sb_append_str(AntSb *sb, AntValue s);
void ant_sb_append_cp(AntSb *sb, int32_t cp);      /* as UTF-8 */
AntValue ant_sb_finish(AntSb *sb);
void ant_template_add(AntSb *sb, AntValue v);       /* appends ant_render(v) */

/* unicase.c (generated): simple one-to-one case mappings, the input on a miss */
int32_t ant_cp_upper(int32_t cp);
int32_t ant_cp_lower(int32_t cp);

/* number.c */
AntValue ant_add(AntValue a, AntValue b);
AntValue ant_sub(AntValue a, AntValue b);
AntValue ant_mul(AntValue a, AntValue b);
AntValue ant_div(AntValue a, AntValue b);
AntValue ant_mod(AntValue a, AntValue b);
AntValue ant_lt(AntValue a, AntValue b);
AntValue ant_le(AntValue a, AntValue b);
AntValue ant_gt(AntValue a, AntValue b);
AntValue ant_ge(AntValue a, AntValue b);
AntValue ant_neg(AntValue a);
AntValue ant_not(AntValue a);
AntValue ant_convert(AntKind k, AntValue v);
bool ant_less(AntValue a, AntValue b);      /* `<` as a C bool (sorting); the unwind flag reports panics */
AntValue ant_conversion(const char *name, AntKind k);   /* the native `int8` ... `float64`, `int`, `byte` */
bool ant_need_number(AntValue v);           /* a number, or `wrong kind` */
bool ant_need_ordered(AntValue v);          /* a number or a string, or `wrong kind` */

/* record.c */
AntValue ant_tag_new(const char *name);
AntTag *ant_need_tag(AntValue v);
AntValue ant_check_tag(AntValue v);         /* the value, or `not a tag` */
AntValue ant_record_snapshot(AntValue v);   /* a shallow copy, or `not a record` */
AntValue ant_record_new(AntValue tag, const AntShape *shape, AntValue *values);
AntValue ant_record_build(AntValue tag, int n, const Sym *names, AntValue *values, const bool *isSpread);
AntValue ant_record_make(AntTag *tag, const AntShape *shape, AntValue *values);
AntValue ant_field_get(AntValue v, Sym s, AntFieldCache *c);
AntValue ant_field_set(AntValue v, Sym s, AntValue val, AntFieldCache *c);
AntValue ant_field_check(AntValue v, Sym s, AntFieldCache *c);
int ant_shape_find(const AntShape *shape, Sym s);
AntValue ant_is_tag(AntValue v, AntValue tag);
AntValue ant_module_new(const AntShape *shape);
AntValue ant_module_get(AntValue m, int idx);
void ant_module_set(AntValue m, int idx, AntValue v);
AntTag *ant_module_tag(void);

/* array.c */
AntValue ant_array_new(int64_t cap);
void ant_array_push(AntValue a, AntValue v);
AntValue ant_array_extend(AntValue a, AntValue v);
AntValue ant_index_get(AntValue a, AntValue i);
AntValue ant_index_set(AntValue a, AntValue i, AntValue v);
AntValue ant_index_check(AntValue a, AntValue i);
AntArray *ant_need_array(AntValue v);
int64_t ant_need_int(AntValue v);

/* map.c */
AntValue ant_map_new(void);
bool ant_map_lookup(AntValue m, AntValue k, AntValue *out);
AntValue ant_map_get(AntValue m, AntValue k);              /* null when absent */
bool ant_map_has(AntValue m, AntValue k);
void ant_map_set(AntValue m, AntValue k, AntValue v);
bool ant_map_remove(AntValue m, AntValue k, AntValue *out);
int64_t ant_map_size(AntValue m);
int64_t ant_map_slots(AntValue m);                         /* entry slots to walk */
bool ant_map_entry(AntValue m, int64_t i, AntValue *k, AntValue *v);   /* false for a dead slot */
AntMap *ant_need_map(AntValue v);

/* equal.c */
bool ant_equal(AntValue a, AntValue b);
uint32_t ant_hash(AntValue v);

/* render.c */
AntValue ant_render(AntValue v);
void ant_render_into(AntSb *sb, AntValue v);

/* func.c */
AntValue ant_closure(AntNative code, const AntFuncInfo *info, int nfree, AntBox **boxes);
AntValue ant_closure_ret(AntNative code, const AntFuncInfo *info, AntFrame *ret, int nfree, AntBox **boxes);
AntValue ant_callee(AntValue f);            /* the value, or `not a function` */
AntValue ant_native(const char *name, const char *params, AntNative code, void *data);
AntBox *ant_box_new(AntValue v);
AntValue ant_box_get(AntBox *b, const char *name);
AntValue ant_call(AntValue f, int argc, AntValue *argv);
AntValue ant_call_named(AntValue f, int npos, AntValue *pos, int nnamed, const Sym *syms, AntValue *vals);
bool ant_call_truth(AntValue f, AntValue x);   /* f(x) as a bool; false after a panic (`not a bool` included) */
AntValue ant_for_each(AntValue v, AntValue body);
AntValue ant_yield_new(AntValue body);
void ant_yield_done(AntValue y);
AntFunc *ant_need_func(AntValue v);         /* the function, or `wrong kind` (NULL) */
AntValue ant_try(AntValue v, AntFrame *target);

/* unwind.c */
void ant_finish_init(AntFrame *fr);         /* a module initializer's epilogue: a `return` aimed at it panics */
void ant_set_return(AntFrame *target, AntValue v);
void ant_panic(const char *msg);
void ant_panicf(const char *fmt, ...);
void ant_panic_value(AntValue msg);
void ant_defer_push(AntDefers *d, AntValue closure);
void ant_run_defers(AntDefers *d);
void ant_report_panic(void);
_Noreturn void ant_exit(int status);
int ant_run_main(void (*init)(void), AntValue *module, int main_idx);
void ant_runtime_init(int argc, char **argv);

/* prelude.c */
void ant_prelude_init(void);
AntValue ant_prelude_get(const char *name);
void ant_prelude_define(const char *name, AntValue v);
/* A member of a namespace: the native `code` with the parameter names
 * `params` ("xs,f"), named `Ns.member`; or, with no code, the constant `value`. */
typedef struct { const char *member, *params; AntNative code; AntValue value; } AntMember;
AntValue ant_namespace(const char *name, int n, const AntMember *members);
#define ANT_LEN(a) ((int)(sizeof (a) / sizeof *(a)))
#define ANT_NAMESPACE(name, members) ant_namespace(name, ANT_LEN(members), members)
extern AntValue ant_tag_ok, ant_tag_err, ant_tag_error, ant_tag_entry, ant_tag_indexed;
AntValue ant_ok(AntValue v);
AntValue ant_err(AntValue e);
AntValue ant_error(const char *message);
AntValue ant_entry(AntValue k, AntValue v);
AntValue ant_indexed(int64_t i, AntValue v);
extern Sym S_value, S_error, S_key, S_x;

/* namespaces, one per file */
AntValue ant_ns_array(void);
AntValue ant_ns_map(void);
AntValue ant_ns_string(void);
AntValue ant_ns_iter(void);
AntValue ant_ns_math(void);
AntValue ant_ns_bits(void);
AntValue ant_ns_result(void);
AntValue ant_ns_record(void);
AntValue ant_ns_os(void);
AntValue ant_ns_time(void);
AntValue ant_ns_file(void);
AntValue ant_ns_mutex(void);
AntValue ant_ns_rwmutex(void);
AntValue ant_ns_waitgroup(void);
AntValue ant_ns_once(void);
AntValue ant_ns_cond(void);
AntValue ant_ns_atomic(void);
AntValue ant_go(AntValue closure);
AntValue ant_native_print(AntFunc *self, int argc, AntValue *argv);
AntValue ant_native_panic(AntFunc *self, int argc, AntValue *argv);
extern AntValue ant_args;

/* The argument checks of natives: bind the argument, or leave with the
 * `wrong kind` panic of the ant_need_* function set. */
#define ARR(v, name) AntArray *name = ant_need_array(v); if (!name) return ANT_VOID
#define MAP(v, name) AntMap *name = ant_need_map(v); if (!name) return ANT_VOID
#define STR(v, name) AntString *name = ant_str(v); if (!name) return ANT_VOID
#define INT(v, name) int64_t name = ant_need_int(v); if (ant_g->unwind.mode) return ANT_VOID
#define FUNC(v) if (!ant_need_func(v)) return ANT_VOID
#define UNWOUND() if (ant_g->unwind.mode) return ANT_VOID

#endif
