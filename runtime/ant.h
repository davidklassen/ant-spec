/* The Ant runtime (docs/runtime-design.md). Everything the code `antc build`
 * emits calls is declared here; the runtime's own C files include it too.
 * This header compiles with nothing but the C standard library: gc.h is
 * included only by alloc.c. */
#ifndef ANT_H
#define ANT_H

#include <stdarg.h>
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
/* sched.c: generated code polls at function entry and loop back-edges; the
 * goroutine at the head of the GIL queue raises the request after waiting
 * 5 ms. Waiters write it under the scheduler's mutex and the holder reads
 * it without, so every access is a relaxed atomic (a plain load or store
 * on arm64; <stdatomic.h> is avoided because its atomic_load/atomic_store
 * macros would capture the Atomic natives' names): ant_gil_yield takes
 * the mutex before acting on what it read. */
extern int ant_switch_requested;
void ant_gil_yield(void);
#define ANT_SAFEPOINT() do { if (__atomic_load_n(&ant_switch_requested, __ATOMIC_RELAXED)) ant_gil_yield(); } while (0)

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
void *ant_alloc_big(size_t n);           /* a table only its owner's header points to */
void *ant_alloc_atomic_big(size_t n);
void *ant_realloc(void *p, size_t n);
void ant_gc_init(void);
void ant_run_on_big_stack(void *(*fn)(void *), void *arg);
void *ant_alloc_uncollectable(size_t n);   /* zeroed; scanned by the collector, never collected; freed with ant_free */
void ant_free(void *p);
int ant_thread_spawn(void *(*fn)(void *), void *arg, size_t stack);   /* detached; 0 or an errno */

/* value.c */
typedef struct { const char *name; AntKind kind; } AntKindName;
const AntKindName *ant_kind_table(int *n);  /* every kind by name, then the aliases `int` and `byte` (G48) */
const char *ant_kind_name(AntKind k);
bool ant_is_number(AntKind k);
bool ant_is_int(AntKind k);
bool ant_truth(AntValue v);                 /* `not a bool` sets a panic and answers false */
AntValue ant_check_bool(AntValue v);        /* the value, or `not a bool` */
AntValue ant_is_kind(AntValue v, AntKind k);
AntValue ant_bound(AntValue v, const char *name);    /* the value, or `unbound name` for K_UNBOUND */
/* The library `wrong kind` rule (design section 6), in one place: the
 * argument has kind k (or satisfies the caller's predicate), or the panic
 * `wrong kind: expected <what>` is set and the answer is false. */
bool ant_wrong_kind(const char *what);
bool ant_need_kind(AntValue v, AntKind k, const char *what);
int64_t ant_need_int(AntValue v);           /* an int, or `wrong kind` (0) */

/* symbol.c */
void ant_symbols_init(int n, const char *const *names);
Sym ant_sym(const char *name);
const char *ant_sym_name(Sym s);
const AntShape *ant_shape_intern(int n, const Sym *names);

/* string.c */
typedef struct { char *data; int64_t len, cap; } AntSb;   /* data: the bytes of the AntString under construction */
AntValue ant_str_new(const char *bytes, int64_t n);
AntValue ant_str_cstr(const char *s);
char *ant_fmt(const char *fmt, ...);        /* printf into fresh GC memory, however long */
char *ant_vfmt(const char *fmt, va_list ap);
AntString *ant_str(AntValue v);             /* the string, or `wrong kind` (NULL) */
AntValue ant_str_concat(AntValue a, AntValue b);
int ant_str_cmp(AntString *a, AntString *b);
bool ant_str_eq(AntString *a, AntString *b);
uint32_t ant_fnv1a(const char *p, int64_t n);   /* never zero */
uint32_t ant_str_hash(AntString *s);
int ant_utf8_decode(const char *p, int64_t n, int32_t *cp);   /* bytes consumed */
bool ant_utf8_valid(const char *p, int64_t n);
static inline bool ant_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }   /* a continuation byte */
AntValue ant_str_next_char(AntString *s, int64_t *i);   /* the code point at byte *i as a string; *i advances */
bool ant_check_slice(int64_t from, int64_t to, int64_t len);   /* `index out of range` unless 0 <= from <= to <= len */
void ant_sb_init(AntSb *sb);
void ant_sb_append(AntSb *sb, const char *p, int64_t n);
void ant_sb_append_cstr(AntSb *sb, const char *p);
void ant_sb_append_str(AntSb *sb, AntValue s);
void ant_sb_append_cp(AntSb *sb, int32_t cp);      /* as UTF-8 */
AntValue ant_sb_finish(AntSb *sb);
void ant_template_add(AntSb *sb, AntValue v);       /* appends ant_render(v) */
AntValue ant_bytes_array(const char *p, int64_t n);  /* the n bytes at p as an Array[byte] */
char *ant_bytes_of(AntValue v, int64_t *n);          /* the bytes of an Array[byte], NUL-terminated, or `wrong kind` (NULL) */

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
bool ant_less(AntValue a, AntValue b);      /* `<` as a C bool (sorting); the unwind flag reports panics */
AntValue ant_conversion(const char *name, AntKind k);   /* the native `int8` ... `float64`, `int`, `byte` */
bool ant_need_number(AntValue v);           /* a number, or `wrong kind` */
bool ant_need_ordered(AntValue v);          /* a number or a string, or `wrong kind` */

/* record.c. The literal constructors take their checks as preconditions:
 * generated code lands the head through ant_check_tag and every spread
 * operand through ant_record_snapshot first, in evaluation order. */
AntValue ant_tag_new(const char *name);
AntValue ant_check_tag(AntValue v);         /* the value, or `not a tag` */
AntValue ant_record_snapshot(AntValue v);   /* a shallow copy, or `not a record` */
AntValue ant_record_new(AntValue tag, const AntShape *shape, AntValue *values);   /* tag: K_TAG */
AntValue ant_record_build(AntValue tag, int n, const Sym *names, AntValue *values, const bool *isSpread);   /* spreads: K_RECORD */
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
AntValue ant_array_from(const AntValue *p, int64_t n);   /* a fresh array holding these n values */
void ant_array_push(AntValue a, AntValue v);
AntValue ant_array_extend(AntValue a, AntValue v);
AntValue ant_index_get(AntValue a, AntValue i);
AntValue ant_index_set(AntValue a, AntValue i, AntValue v);
AntValue ant_index_check(AntValue a, AntValue i);
AntArray *ant_need_array(AntValue v);

/* map.c */
AntValue ant_map_new(void);
bool ant_map_lookup(AntValue m, AntValue k, AntValue *out);
AntValue ant_map_get(AntValue m, AntValue k);              /* null when absent */
void ant_map_set(AntValue m, AntValue k, AntValue v);
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
void ant_render_float(AntSb *sb, double x, bool f32);   /* shortest round-trip, as String.from */
/* The containers a walk is inside, so that a cycle is noticed (render, Json.stringify). */
typedef struct { void **items; int n, cap; } AntActive;
bool ant_active_has(AntActive *a, void *p);
void ant_active_push(AntActive *a, void *p);

/* func.c */
AntValue ant_closure(AntNative code, const AntFuncInfo *info, int nfree, AntBox **boxes);
AntValue ant_closure_ret(AntNative code, const AntFuncInfo *info, AntFrame *ret, int nfree, AntBox **boxes);
AntValue ant_closure_data(AntNative code, const AntFuncInfo *info, void *data);   /* a native over fixed metadata */
AntValue ant_callee(AntValue f);            /* the value, or `not a function` */
AntValue ant_native(const char *name, const char *params, AntNative code, void *data);
AntBox *ant_box_new(AntValue v);
AntValue ant_call(AntValue f, int argc, AntValue *argv);
AntValue ant_call_named(AntValue f, int npos, AntValue *pos, int nnamed, const Sym *syms, AntValue *vals);
bool ant_call_truth(AntValue f, AntValue x);   /* f(x) as a bool; false after a panic (`not a bool` included) */
AntValue ant_for_each(AntValue v, AntValue body);
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
/* Cleanup with the pending unwind set aside (G59): `aside` saves and clears
 * it, `merge` keeps a panic the cleanup raised and restores the saved
 * state otherwise. The one rule for deferred blocks and withLock. */
void ant_unwind_aside(AntUnwind *saved);
void ant_unwind_merge(const AntUnwind *saved);
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
extern Sym S_value, S_error, S_key, S_x, S_yield;

/* sync.c */
void *ant_sync_need(AntValue v, const char *name);   /* the sync object named `name` (by pointer), or `wrong kind` (NULL) */

/* namespaces, one per file: each defines its prelude global */
void ant_ns_array(void);
void ant_ns_map(void);
void ant_ns_string(void);
void ant_ns_iter(void);
void ant_ns_math(void);
void ant_ns_bits(void);
void ant_ns_result(void);
void ant_ns_record(void);
void ant_ns_os(void);
void ant_ns_time(void);
void ant_ns_file(void);
void ant_ns_http(void);
void ant_ns_json(void);
void ant_ns_mutex(void);
void ant_ns_rwmutex(void);
void ant_ns_waitgroup(void);
void ant_ns_once(void);
void ant_ns_cond(void);
void ant_ns_atomic(void);

/* sched.c */
AntValue ant_go(AntValue closure);
void ant_gil_acquire(void);
void ant_gil_release(void);          /* only around a syscall that touches no AntValue */
void ant_sched_enter_main(void);     /* the main goroutine takes the GIL */

AntValue ant_native_print(AntFunc *self, int argc, AntValue *argv);
AntValue ant_native_panic(AntFunc *self, int argc, AntValue *argv);
extern AntValue ant_args;

/* The argument checks of natives: bind the argument, or leave with the
 * `wrong kind` panic of the ant_need_* function set; UNWOUND() leaves after
 * a callback or an operation that unwound. One spelling per check, so that
 * the audit of design section 6 (every callback is followed by an unwind
 * test) is one grep. */
#define ARR(v, name) AntArray *name = ant_need_array(v); if (!name) return ANT_VOID
#define MAP(v, name) AntMap *name = ant_need_map(v); if (!name) return ANT_VOID
#define STR(v, name) AntString *name = ant_str(v); if (!name) return ANT_VOID
#define INT(v, name) int64_t name = ant_need_int(v); if (ant_g->unwind.mode) return ANT_VOID
#define COUNT(v, name) INT(v, name); if (name < 0) { ant_panic("negative count"); return ANT_VOID; }
#define FUNC(v) if (!ant_need_func(v)) return ANT_VOID
#define NUM(v) if (!ant_need_number(v)) return ANT_VOID
#define ORD(v) if (!ant_need_ordered(v)) return ANT_VOID
#define SYNC(v, T, NAME, name) T *name = ant_sync_need(v, NAME); if (!name) return ANT_VOID
#define UNWOUND() if (ant_g->unwind.mode) return ANT_VOID

/* The grow-by-doubling push of the runtime's own lists: `items` of `n` used
 * and `cap` allocated, `first` the initial capacity. */
#define ANT_PUSH(items, n, cap, first, item) do { \
    if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : (first); (items) = ant_realloc((items), sizeof *(items) * (size_t)(cap)); } \
    (items)[(n)++] = (item); \
  } while (0)

#endif
