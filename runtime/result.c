/* The Result and Record namespaces (section 11). */
#include "ant.h"

static bool need_result(AntValue v) {
  if (v.kind != K_RECORD) { ant_panic("wrong kind: expected a Result"); return false; }
  AntTag *t = ((AntRecord *)v.as.p)->tag;
  if (t != ant_tag_ok.as.p && t != ant_tag_err.as.p) { ant_panic("wrong kind: expected a Result"); return false; }
  return true;
}

static bool is_err(AntValue v) {
  return ((AntRecord *)v.as.p)->tag == ant_tag_err.as.p;
}

static AntValue r_ok(AntFunc *self, int argc, AntValue *argv) { return ant_ok(argv[0]); }
static AntValue r_err(AntFunc *self, int argc, AntValue *argv) { return ant_err(argv[0]); }

static AntValue r_is_ok(AntFunc *self, int argc, AntValue *argv) {
  if (!need_result(argv[0])) return ANT_VOID;
  return ant_bool(!is_err(argv[0]));
}

static AntValue r_unwrap(AntFunc *self, int argc, AntValue *argv) {
  if (!need_result(argv[0])) return ANT_VOID;
  if (is_err(argv[0])) { ant_panic("unwrap on Err"); return ANT_VOID; }
  return ant_field_get(argv[0], S_value, NULL);
}

static AntValue r_unwrap_or(AntFunc *self, int argc, AntValue *argv) {
  if (!need_result(argv[0])) return ANT_VOID;
  if (is_err(argv[0])) return argv[1];
  return ant_field_get(argv[0], S_value, NULL);
}

/* A fresh Result either way; f runs only on the case it maps. */
static AntValue r_map(AntFunc *self, int argc, AntValue *argv) {
  if (!need_result(argv[0])) return ANT_VOID;
  FUNC(argv[1]);
  if (is_err(argv[0])) {
    AntValue e = ant_field_get(argv[0], S_error, NULL);
    if (ant_g->unwind.mode) return ANT_VOID;
    return ant_err(e);
  }
  AntValue v = ant_field_get(argv[0], S_value, NULL);
  if (ant_g->unwind.mode) return ANT_VOID;
  AntValue r = ant_call(argv[1], 1, &v);
  if (ant_g->unwind.mode) return ANT_VOID;
  return ant_ok(r);
}

static AntValue r_map_err(AntFunc *self, int argc, AntValue *argv) {
  if (!need_result(argv[0])) return ANT_VOID;
  FUNC(argv[1]);
  if (!is_err(argv[0])) {
    AntValue v = ant_field_get(argv[0], S_value, NULL);
    if (ant_g->unwind.mode) return ANT_VOID;
    return ant_ok(v);
  }
  AntValue e = ant_field_get(argv[0], S_error, NULL);
  if (ant_g->unwind.mode) return ANT_VOID;
  AntValue r = ant_call(argv[1], 1, &e);
  if (ant_g->unwind.mode) return ANT_VOID;
  return ant_err(r);
}

AntValue ant_ns_result(void) {
  static const AntMember members[] = {
    { "ok", "value", r_ok },
    { "err", "error", r_err },
    { "isOk", "r", r_is_ok },
    { "map", "r,f", r_map },
    { "mapErr", "r,f", r_map_err },
    { "unwrap", "r", r_unwrap },
    { "unwrapOr", "r,fallback", r_unwrap_or },
  };
  return ANT_NAMESPACE("Result", members);
}

static AntValue rec_same(AntFunc *self, int argc, AntValue *argv) {
  if (argv[0].kind != K_RECORD || argv[1].kind != K_RECORD) { ant_panic("wrong kind: expected a record"); return ANT_VOID; }
  return ant_bool(argv[0].as.p == argv[1].as.p);
}

AntValue ant_ns_record(void) {
  static const AntMember members[] = {
    { "same", "a,b", rec_same },
  };
  return ANT_NAMESPACE("Record", members);
}
