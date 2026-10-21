/* The Iter namespace: lazy sequences (section 11). Every function returns
 * a native sequence stage without running its source; a stage runs when it
 * is called with a yield, and a `false` from downstream travels back to the
 * source through every stage. */
#include "ant.h"

typedef struct { AntValue source, f, yield; int64_t a, b; } Stage;

/* The metadata of every stage function, fixed: a stage takes `yield`, a
 * stage body (the yield a stage hands its source) takes `x`. */
#define STAGE_INFO(var, name, param) static const AntFuncInfo var = { name, "<runtime>", 0, 1, &param, NULL, false }
STAGE_INFO(range_info, "Iter.range", S_yield);
STAGE_INFO(from_array_info, "Iter.fromArray", S_yield);
STAGE_INFO(map_info, "Iter.map", S_yield);
STAGE_INFO(map_body_info, "Iter.map", S_x);
STAGE_INFO(filter_info, "Iter.filter", S_yield);
STAGE_INFO(filter_body_info, "Iter.filter", S_x);
STAGE_INFO(take_info, "Iter.take", S_yield);
STAGE_INFO(take_body_info, "Iter.take", S_x);
STAGE_INFO(collect_info, "Iter.toArray", S_x);

/* Runs a stage's source with `body` as the stage's own yield: the body
 * finds a copy of the stage, with the downstream `yield`, in self->data.
 * A `false` from downstream (ant_call_truth) travels back to the source. */
static AntValue run_stage(const Stage *st, AntValue yield, const AntFuncInfo *info, AntNative body) {
  Stage *run = ant_alloc(sizeof(Stage));
  *run = *st;
  run->yield = yield;
  ant_for_each(st->source, ant_closure_data(body, info, run));
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
  return ant_closure_data(range_run, &range_info, st);
}

static AntValue from_array_run(AntFunc *self, int argc, AntValue *argv) {
  Stage *st = self->data;
  FUNC(argv[0]);
  AntArray *xs = st->source.as.p;
  for (int64_t i = 0; i < xs->len; i++) if (!ant_call_truth(argv[0], xs->data[i])) break;
  return ANT_VOID;
}

static AntValue i_from_array(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  (void)xs;
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  return ant_closure_data(from_array_run, &from_array_info, st);
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
  return run_stage(self->data, argv[0], &map_body_info, map_body);
}

static AntValue i_map(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  FUNC(argv[1]);
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->f = argv[1];
  return ant_closure_data(map_run, &map_info, st);
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
  return run_stage(self->data, argv[0], &filter_body_info, filter_body);
}

static AntValue i_filter(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  FUNC(argv[1]);
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->f = argv[1];
  return ant_closure_data(filter_run, &filter_info, st);
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
  return run_stage(st, argv[0], &take_body_info, take_body);
}

static AntValue i_take(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  COUNT(argv[1], n);
  Stage *st = ant_alloc(sizeof(Stage));
  st->source = argv[0];
  st->a = n;
  return ant_closure_data(take_run, &take_info, st);
}

static AntValue collect_body(AntFunc *self, int argc, AntValue *argv) {
  ant_array_push(*(AntValue *)self->data, argv[0]);
  return ANT_TRUE;
}

static AntValue i_to_array(AntFunc *self, int argc, AntValue *argv) {
  FUNC(argv[0]);
  AntValue *out = ant_alloc(sizeof(AntValue));
  *out = ant_array_new(4);
  ant_for_each(argv[0], ant_closure_data(collect_body, &collect_info, out));
  UNWOUND();
  return *out;
}

void ant_ns_iter(void) {
  static const AntMember members[] = {
    { "range", "from,to", i_range },
    { "fromArray", "xs", i_from_array },
    { "map", "s,f", i_map },
    { "filter", "s,f", i_filter },
    { "take", "s,n", i_take },
    { "toArray", "s", i_to_array },
  };
  ANT_NAMESPACE("Iter", members);
}
