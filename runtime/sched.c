/* Goroutines. This step runs a `go` block synchronously: step 2.6 brings
 * the GIL scheduler of the design document. */
#include "ant.h"

/* Runs the closure to completion; a panic in it is reported and dropped,
 * so the caller continues as it would with a real goroutine. */
AntValue ant_go(AntValue closure) {
  ant_call(closure, 0, NULL);
  if (ant_g->unwind.mode == U_PANIC) ant_report_panic();
  ant_g->unwind.mode = U_NONE;
  return ANT_VOID;
}
