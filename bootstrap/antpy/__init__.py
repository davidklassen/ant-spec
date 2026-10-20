"""antpy: the bootstrap interpreter for Ant (roadmap M0).

Modules:
  values   value classes, kinds, panics
  equal    structural equality, structural hashing, and the map keyed by them
  arith    arithmetic, comparison, unary operators, conversions, Bits
  render   String.from
  lower    the reference parser's AST to the evaluator's nodes
  interp   scopes, frames, evaluation, calls, loops, defers, modules, goroutines
  library  the data namespaces: Array, Map, String, Iter, Math, and the argument checks
  sync     the sync objects: Mutex, RwMutex, WaitGroup, Once, Cond, Atomic
  system   the Os and Time namespaces of stdlib.md
  fileio   the File namespace of stdlib.md
  jsonio   the Json namespace of stdlib.md
  prelude  the prelude scope: tags, conversions, print, panic, Result, Record, Bits
"""
