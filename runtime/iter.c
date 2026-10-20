/* The Iter namespace: lazy sequences (section 11). Every function returns
 * a native sequence stage without running its source; a stage runs when it
 * is called with a yield, and a `false` from downstream travels back to the
 * source through every stage. */
#include "ant.h"

typedef struct { AntValue source, f, yield; int64_t a, b; } Stage;

static AntValue stage(const char *name, AntNative run, Stage *st) {
  return ant_native(name, "yield", run, st);
}

/* Runs a stage's source with `body` as the stage's own yield: the body
 * finds a copy of the stage, with the downstream `yield`, in self->data.
 * A `false` from downstream (ant_call_truth) travels back to the source. */
static AntValue run_stage(const Stage *st, AntValue yield, const char *name, AntNative body) {
  Stage *run = ant_alloc(sizeof(Stage));
  *run = *st;
  run->yield = yield;
  ant_for_each(st->source, ant_native(name, "x", body, run));
  return ANT_VOID;
}

static AntValue range_run(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  FUNC(argv[0]);
  for (int64_t i = st->a; i < st->b; i++) if (!ant_call_truth(argv[0], ant_int(i))) break;
  return ANT_VOID;
}

static AntValue i_range(AntFunc *self, int argc, AntValue *argv) {
  INT(argv[0], from);
  INT(argv[1], to);
  Stage *st = ant_alloc(sizeof(Stage));
  st->a = from;
  st->b = to;
  return stage("Iter.range", range_run, st);
}

static AntValue from_array_run(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  FUNC(argv[0]);
  AntArray *xs = st->source.as.p;
  for (int64_t i = 0; i < xs->len; i++) if (!ant_call_truth(argv[0], xs->data[i])) break;
  return ANT_VOID;
}

static AntValue i_from_array(AntFunc *self, int argc, AntValue *argv) {
  if (!ant_need_array(argv[0])) return ANT_VOID;
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  return stage("Iter.fromArray", from_array_run, st);
}

/* map: body(v) pushes f(v) downstream and answers what the yield answered. */
static AntValue map_body(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  AntValue r = ant_call(st->f, 1, argv);
  UNWOUND();
  return ant_bool(ant_call_truth(st->yield, r));
}

static AntValue map_run(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  return run_stage(self->data, argv[0], "Iter.map", map_body);
}

static AntValue i_map(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  FUNC(argv[1]);
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->f = argv[1];
  return stage("Iter.map", map_run, st);
}

static AntValue filter_body(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  bool keep = ant_call_truth(st->f, argv[0]);
  UNWOUND();
  if (!keep) return ANT_TRUE;
  return ant_bool(ant_call_truth(st->yield, argv[0]));
}

static AntValue filter_run(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  return run_stage(self->data, argv[0], "Iter.filter", filter_body);
}

static AntValue i_filter(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  FUNC(argv[1]);
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->f = argv[1];
  return stage("Iter.filter", filter_run, st);
}

/* take: the source is stopped right after the nth element; take(s, 0) never runs it. */
static AntValue take_body(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  st->a--;
  bool more = ant_call_truth(st->yield, argv[0]);
  UNWOUND();
  return ant_bool(more && st->a > 0);
}

static AntValue take_run(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  FUNC(argv[0]);
  if (st->a == 0) return ANT_VOID;
  return run_stage(st, argv[0], "Iter.take", take_body);
}

static AntValue i_take(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  INT(argv[1], n);
  if (n < 0) { ant_panic("negative count"); return ANT_VOID; }
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->a = n;
  return stage("Iter.take", take_run, st);
}

static AntValue collect_body(AntFunc *self, int argc, AntValue *argv) {
  ant_array_push(*(AntValue *)self->data, argv[0]);
  return ANT_TRUE;
}

static AntValue i_to_array(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  AntValue *out = ant_alloc(sizeof(AntValue));
  *out = ant_array_new(4);
  ant_for_each(argv[0], ant_native("Iter.toArray", "x", collect_body, out));
  UNWOUND();
  return *out;
}

AntValue ant_ns_iter(void) {
  static const AntMember members[] = {
    { "range", "from,to", i_range },
    { "fromArray", "xs", i_from_array },
    { "map", "s,f", i_map },
    { "filter", "s,f", i_filter },
    { "take", "s,n", i_take },
    { "toArray", "s", i_to_array },
  };
  return ANT_NAMESPACE("Iter", members);
}
