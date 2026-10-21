/* Function values, boxes, calls with named arguments and defaults, yield
 * objects, `for`, and `try` (sections 5, 6, 12). */
#include <string.h>
#include "ant.h"

/* A closure: generated code creates every function value with this, and
 * the runtime's natives are closures over no boxes with `data`. */
AntValue ant_closure(AntNative code, const AntFuncInfo *info, int nfree, AntBox **boxes) {
  AntFunc *f = ant_alloc(sizeof(AntFunc) + sizeof(AntBox *) * (size_t)nfree);
  f->code = code;
  f->info = info;
  f->ret = NULL;
  f->data = NULL;
  f->nfree = nfree;
  if (nfree) memcpy(f->free, boxes, sizeof(AntBox *) * (size_t)nfree);
  return ant_ptr(K_FUNC, f);
}

/* A `for` body closure, which carries the enclosing function's frame as the target of `return`. */
AntValue ant_closure_ret(AntNative code, const AntFuncInfo *info, AntFrame *ret, int nfree, AntBox **boxes) {
  AntValue f = ant_closure(code, info, nfree, boxes);
  ((AntFunc *)f.as.p)->ret = ret;
  return f;
}

/* A native over fixed metadata: the runtime's per-call closures (yield
 * objects, Iter stages) and the namespace members. */
AntValue ant_closure_data(AntNative code, const AntFuncInfo *info, void *data) {
  AntValue f = ant_closure(code, info, 0, NULL);
  ((AntFunc *)f.as.p)->data = data;
  return f;
}

/* The one `not a function` check: the function behind f, or NULL after the panic. */
static AntFunc *callee(AntValue f) {
  if (f.kind != K_FUNC) {
    ant_panic("not a function");
    return NULL;
  }
  return f.as.p;
}

/* The callee check of a call, made before the arguments evaluate. */
AntValue ant_callee(AntValue f) {
  callee(f);
  return f;
}

/* A native function with the parameter names `params` ("xs,f"), so that
 * named arguments resolve against it like against a declared function. */
AntValue ant_native(const char *name, const char *params, AntNative code, void *data) {
  AntFuncInfo *info = ant_alloc(sizeof(AntFuncInfo));
  int n = 0;
  if (*params) {
    n = 1;
    for (const char *p = params; *p; p++) if (*p == ',') n++;
  }
  Sym *syms = ant_alloc_atomic(sizeof(Sym) * (size_t)(n ? n : 1));
  const char *p = params;
  for (int i = 0; i < n; i++) {
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char buf[64];
    memcpy(buf, p, len);
    buf[len] = 0;
    syms[i] = ant_sym(buf);
    p += len + 1;
  }
  info->name = name;
  info->file = "<runtime>";
  info->line = 0;
  info->nparams = n;
  info->params = syms;
  info->hasDefault = NULL;
  info->isLambda = false;
  return ant_closure_data(code, info, data);
}

AntBox *ant_box_new(AntValue v) {
  AntBox *b = ant_alloc(sizeof(AntBox));
  b->v = v;
  return b;
}

AntFunc *ant_need_func(AntValue v) {
  return ant_need_kind(v, K_FUNC, "a function") ? v.as.p : NULL;
}

/* ---------------------------------------------------------------- calls */

/* Every parameter still unfilled must have a default. */
static bool check_missing(const AntFuncInfo *info, AntValue *slots) {
  for (int i = 0; i < info->nparams; i++) {
    if (slots[i].kind == K_ABSENT && !(info->hasDefault && info->hasDefault[i])) {
      ant_panicf("missing argument: %s", ant_sym_name(info->params[i]));
      return false;
    }
  }
  return true;
}

AntValue ant_call(AntValue f, int argc, AntValue *argv) {
  AntFunc *fn = callee(f);
  if (!fn) return ANT_VOID;
  const AntFuncInfo *info = fn->info;
  if (info->isLambda) {
    if (argc > 1) { ant_panic("too many arguments"); return ANT_VOID; }
    AntValue x = argc == 0 ? ANT_VOID : argv[0];
    return fn->code(fn, 1, &x);
  }
  int n = info->nparams;
  if (argc == n) return fn->code(fn, n, argv);
  if (argc > n) { ant_panic("too many arguments"); return ANT_VOID; }
  AntValue slots[n > 0 ? n : 1];
  for (int i = 0; i < n; i++) slots[i] = i < argc ? argv[i] : ANT_ABSENT;
  if (!check_missing(info, slots)) return ANT_VOID;
  return fn->code(fn, n, slots);
}

AntValue ant_call_named(AntValue f, int npos, AntValue *pos, int nnamed, const Sym *syms, AntValue *vals) {
  AntFunc *fn = callee(f);
  if (!fn) return ANT_VOID;
  const AntFuncInfo *info = fn->info;
  if (info->isLambda) { ant_panic("named argument to lambda"); return ANT_VOID; }
  int n = info->nparams;
  if (npos > n) { ant_panic("too many arguments"); return ANT_VOID; }
  AntValue slots[n > 0 ? n : 1];
  for (int i = 0; i < n; i++) slots[i] = i < npos ? pos[i] : ANT_ABSENT;
  for (int k = 0; k < nnamed; k++) {
    int i = 0;
    while (i < n && info->params[i] != syms[k]) i++;
    if (i == n) { ant_panicf("unknown parameter: %s", ant_sym_name(syms[k])); return ANT_VOID; }
    if (slots[i].kind != K_ABSENT) { ant_panicf("parameter already filled: %s", ant_sym_name(syms[k])); return ANT_VOID; }
    slots[i] = vals[k];
  }
  if (!check_missing(info, slots)) return ANT_VOID;
  return fn->code(fn, n, slots);
}

/* ---------------------------------------------------------------- yield objects */

typedef struct { AntValue body; bool done; } Yield;

static const AntFuncInfo yield_info = { "yield", "<runtime>", 0, 1, &S_x, NULL, false };

static AntValue yield_code(AntFunc *self, int argc, AntValue *argv) {
  ANT_SAFEPOINT();   /* a `for` body has no entry safepoint of its own */
  Yield *y = self->data;
  if (y->done) { ant_panic("yield after done"); return ANT_VOID; }
  AntValue r = ant_call(y->body, 1, argv);
  UNWOUND();
  bool more = r.kind == K_BOOL && r.as.b;
  if (!more) y->done = true;
  return ant_bool(more);
}

/* The `yield` a sequence receives: runs body(x) and answers its bool. */
static AntValue yield_new(AntValue body) {
  Yield *y = ant_alloc(sizeof(Yield));
  y->body = body;
  y->done = false;
  return ant_closure_data(yield_code, &yield_info, y);
}

/* f(x) as the bool it must answer: false after a panic in f or `not a
 * bool`, which the unwind flag tells apart from a false answer. */
bool ant_call_truth(AntValue f, AntValue x) {
  AntValue r = ant_call(f, 1, &x);
  if (ant_g->unwind.mode) return false;
  return ant_truth(r);
}

/* One iteration: false after a break, or when the body unwound. */
static bool iterate(AntValue body, AntValue x) {
  ANT_SAFEPOINT();   /* a `for` body has no entry safepoint of its own */
  AntValue r = ant_call(body, 1, &x);
  if (ant_g->unwind.mode) return false;
  return !(r.kind == K_BOOL && !r.as.b);
}

AntValue ant_for_each(AntValue v, AntValue body) {
  switch (v.kind) {
    case K_ARRAY: {
      AntArray *a = v.as.p;
      for (int64_t i = 0; i < a->len; i++) if (!iterate(body, a->data[i])) break;
      return ANT_VOID;
    }
    case K_MAP: {
      for (int64_t i = 0; i < ant_map_slots(v); i++) {
        AntValue k, val;
        if (!ant_map_entry(v, i, &k, &val)) continue;
        if (!iterate(body, ant_entry(k, val))) break;
      }
      return ANT_VOID;
    }
    case K_STRING: {
      AntString *s = v.as.p;
      int64_t i = 0;
      while (i < s->nbytes) if (!iterate(body, ant_str_next_char(s, &i))) break;
      return ANT_VOID;
    }
    case K_FUNC: {
      AntValue y = yield_new(body);
      ant_call(v, 1, &y);
      ((Yield *)((AntFunc *)y.as.p)->data)->done = true;
      return ANT_VOID;
    }
    default:
      ant_panic("not iterable");
      return ANT_VOID;
  }
}

/* `try e`: null and Err leave the enclosing function (target), Ok unwraps,
 * anything else passes through (G28). */
AntValue ant_try(AntValue v, AntFrame *target) {
  if (v.kind == K_NULL) {
    ant_set_return(target, ANT_NULL);
    return ANT_VOID;
  }
  if (v.kind == K_RECORD) {
    AntRecord *r = v.as.p;
    if (r->tag == ant_tag_err.as.p) {
      AntValue e = ant_field_get(v, S_error, NULL);
      UNWOUND();
      ant_set_return(target, ant_err(e));
      return ANT_VOID;
    }
    if (r->tag == ant_tag_ok.as.p) return ant_field_get(v, S_value, NULL);
  }
  return v;
}
