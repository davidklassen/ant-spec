# Report for step 0.6 "antpy-io": File, Json, Time

## What was built

All changes are under `bootstrap/antpy/`; no evaluator change was needed (the step is purely additive natives, as the plan predicted from the baseline: 19 checks, 14 failures, all `unbound name: File`/`Json` or `no field: now`; `work/0.6/baseline.log`).

| File | Change |
|---|---|
| `fileio.py` (new, 216 lines) | the `FileHandle` value (kind `"file"`), `need_file`, the `failed(e, path)` helper that turns an I/O exception into `Err{ error: Error{ message } }`, the thirteen `File` natives, registry `FILE` |
| `jsonio.py` (new, 136 lines) | `Json.parse` over `json.loads` with `parse_int=float`, a `parse_constant` that rejects `NaN`/`Infinity`, an `object_pairs_hook` building an `AntMap` (last duplicate wins), and a lone-surrogate check; a hand-written compact `Json.stringify` (`emit`/`emit_container`) reusing `render_float` and `render`; registry `JSON` |
| `system.py` | now "the Os and Time namespaces": `Time.now`, `Time.monotonic` (millisecond reads of `time_ns`/`monotonic_ns`), `Time.sleep` rewritten as a deadline loop on `monotonic_ns`; registry `TIME` |
| `sync.py` | the Time section and `import time` removed; docstring updated |
| `library.py` | a "Result constructors" section: `ok(v)`, `err(e)`, `error(message)`, the one definition of the `Ok`/`Err`/`Error` shapes, shared by the prelude and the I/O natives |
| `prelude.py` | `Result.ok`/`Result.err` are `library.ok`/`err` (the duplicate bodies deleted; `result_map`/`result_map_err` use them); imports `TIME` from `system`, `FILE`, `JSON`; `"File"` and `"Json"` in `NAMESPACES`; docstring |
| `equal.py` | `REFERENCE_KINDS` gains `"file"`: a handle compares and hashes by identity |
| `render.py` | a `"file"` branch: `String.from(f)` is `File` |
| `values.py`, `__init__.py` | comments and the module list |

Work method followed the plan's order: plumbing (errors/nullability subsets: 53 checks, 0 failures), Time (time test 2/2; concurrency subset 32 checks, 0 failures, `work/0.6/concurrency.log`), File, Json (io tier 19 checks, 0 failures), full acceptance, `--consistency`, `ruff check --select F401,F821,F841,F811 bootstrap/` clean, probes.

## Acceptance

Command: the verbatim acceptance command of the task, output in `work/0.6/test.log`. Last line:

```
432 checks, 0 failures, 0 warnings (implementation mode, 431 tests)
```

(The plan estimated 433 checks; the actual count is 432 over 431 tests: 0.5's 413 tests plus the 18 io tests.)

The same selection with `--consistency` (a regression check for 0.G; the io tier is not part of consistency) is in `work/0.6/consistency.log`, last line:

```
432 checks, 0 failures, 0 warnings (implementation mode, 431 tests)
```

## Test changes

None. No test contradicted the spec.

## Decisions and deviations from the plan

1. **`failed(e, path)` takes the path** (plan: `failed(e)` with Python's message). Every `Err` message ends in the path the operation was on: for a handle, the path it was opened with. An `io.UnsupportedOperation` (reading a `create`d handle, writing an `open`ed one) reads `cannot read: <path>` / `cannot write: <path>` instead of Python's bare `read` / `write`; a closed handle gives Python's `readline of closed file: <path>`; a missing file `No such file or directory: <path>`; bad text `invalid UTF-8: <path>`. Messages are only asserted non-empty, but a reader of a trace should see which file failed.
2. **`Json.stringify` of a module record is `null`** (plan: an object of its fields). stdlib.md says "records as objects without their tag, as `String.from` would", and `String.from` renders a module as `Module`, not its fields; the fields dict is the live module scope and can hold the `UNINIT` sentinel (which has no kind and would crash the emitter) besides functions and tags. Every other record, including the prelude's `Ok`/`Err`/`Entry` and the namespace records (`Json.stringify(Math)` is an object of `null`s and the two constants), is emitted as an object.
3. **`File.pipe` is a plain chunk loop** (64 KiB reads until empty) instead of `shutil.copyfileobj`: the same behaviour, no extra import, and the loop is what the later Ant rewrite will look like.
4. **`File.writeBytes` snapshots the array** (`list(bs)`) before checking each element is a `uint8`, so another goroutine pushing a non-byte between the check and `bytes(items)` cannot raise a `TypeError` out of the native (section 12: a data race cannot crash the interpreter).
5. **`Time.sleep` naps at most an hour per loop iteration** (`MAX_NAP_NS`). `time.sleep` raises `OverflowError` for about 2**63 ns and more, so `Time.sleep(10**13)` would otherwise escape as an internal interpreter error; the deadline loop re-checks after each nap, so the cap changes nothing else. The deadline loop itself is the plan's: two `Time.monotonic()` readings around `Time.sleep(50)` differ by at least 50 in integer arithmetic (the deadline is on the same nanosecond clock, and floor division by 10**6 commutes with adding 50 * 10**6), where a single `time.sleep(0.05)` can wake microseconds early as that clock measures it. The time test passed 10 runs out of 10.
6. **The Result constructors** sit in their own "Result constructors" section of `library.py` after the argument checks (the plan said "next to `entry`", which is in the Map section); `prelude.py` imports `ok`, `err`, and `fileio.py`/`jsonio.py` import all three.
7. `NAMESPACES` lists `File`, `Time`, `Os`, `Json` in stdlib.md's order.

Everything else is as planned: `read`/`readBytes`/`write`/`writeBytes` use `with` and close before returning; `writeLine` and `pipe` flush after writing because the process ends through `os._exit`, which skips Python's buffered flushing (probe `file-noclose.ant`: a line written through a handle that is never closed, and a `pipe` into another, are both on disk after the run); `exists` is a bare `bool` and `close` a bare `void`; `readLine` strips one `\n` and gives `null` at end of file; `readBytes` elements are `UInt8`; JSON numbers are always `float64`; `stringify` numbers come from `render_float`, map keys from top-level `render`, record fields in `fields` order without the tag, no whitespace, `null` for NaN/infinities/void/func/tag/sync/file and for a container that contains itself.

## Probe results (`work/0.6/probes/*.ant`, run with a fresh temporary directory as the argument)

Handles (`file-handle-*.ant`): `print(f)` is `File`, `[f, g]` is `[File, File]`, `f == f` true, `f == g` false, a map keyed by two handles has size 2 and `m[f]` reads back; `Json.stringify(f)` is `null`; `for x in f` panics `not iterable`, `f.x` `not a record`, `f[0]` `not indexable`, `f is File` `not a tag` (File is a namespace record, as planned).

Kind checks: `File.write(1, "x")` panics `wrong kind: expected a string`; `File.writeBytes(p, [1, 2])` `wrong kind: expected a byte`; `Json.parse(1)` `wrong kind: expected a string`; `Time.sleep(1.0)` `wrong kind: expected an int`; `Time.now(1)` `too many arguments`.

File edge cases (`file-edge.ant`): `File.exists(dir)` true; `File.remove(dir)` `Err Operation not permitted: <dir>`; `File.list(file)` `Err Not a directory: <file>`; `readLine` on a `create`d handle `Err cannot read: <path>`; `writeLine` on an `open`ed one `Err cannot write: <path>`; a `"x\r\ny\nz"` file reads as `"x\r"`, `"y"`, `"z"`, `null`; `close` twice is a no-op; any operation on a closed handle is an `Err ... of closed file: <path>`; `pipe` copies from the source's current position (after one `readLine`, the rest) and appends after a `writeLine` on the destination, a second `pipe` copies nothing and is `Ok`; `File.read` of a missing file is `Err No such file or directory: <path>`, of a `[255]` file `Err invalid UTF-8: <path>`; `File.remove`/`File.list` of a missing path are `Err`; `File.exists("")` false, `File.write("", "x")` `Err`; an empty file reads as `""` and its first `readLine` is `null`.

JSON (`json-edge.ant`): `parse("1e400")` is `Ok(Infinity)`; `"\ud800"` alone, or nested in an array or object value, is `Err lone surrogate escape`, while a pair in a key decodes to one code point; `NaN`/`Infinity`/`-Infinity` are `Err not a JSON value: ...`; `" 1 "` is `Ok(1.0)`; `""`, `"  "`, `01`, `1.`, `.5`, `[1,]`, `"\x41"`, a raw tab in a string are all `Err` with Python's message; duplicate keys keep the last; `-0` is `-0.0`; a 30-digit integer rounds to a float; 50,000 nested arrays parse and stringify back (100,000 characters). `stringify`: `void`, a module, a tag, a `Mutex` are `null`; a cyclic array is `[1.0,null]` and a map holding itself `{"a":1,"self":null}` (two references to the same non-cyclic array are both emitted in full); `float32(1.5)` is `1.5`, `float32(0.1)` is `0.1`; a record key renders `{"Point{ x: 1.0 }":2}`, a float key `{"1.5":true}`, an array key `{"[1, 2]":null}`; strings escape `"`, `\`, `\n`, `\t`, `\u0001` and keep `é` and emoji literal; `1e21`, `1e-7`, `-0.0`, the largest `uint64`, `Math.PI` as `3.141592653589793`; `Ok{ value: 1 }` is `{"value":1}`, `Entry` `{"key":"k","value":"v"}`, `[[], {}, Map{}]` is `[[],null,{}]` (`{}` is an empty block, so `void`).

Time (`time-edge.ant`): `Time.sleep(0)` and `Time.sleep(-5)` return at once, `Time.sleep(1)` returns `void`, `Time.now()` and `Time.monotonic()` satisfy `is int`, `Time.now()` advances by at least 15 ms across `Time.sleep(20)`.

## Known limitations

- An Ant string holding a lone surrogate (reachable only through a `\u{D800}` escape in source, a pre-existing gap) cannot be printed or written by Python; `File.write` of one returns `Err` through the `UnicodeError` branch (reported as `invalid UTF-8`), and `Json.stringify` of one produces a text that `print` cannot encode.
- `Json.parse` error messages are Python's (`Expecting ',' delimiter: line 1 column 6 (char 5)`); the spec fixes nothing about them.
- `Json.stringify` of a map follows the map's own iteration order, which is unspecified (tests compare after a parse or use one entry).
- The lone-surrogate walk after a parse is linear in the value and runs only when the text contains `\u`.
- Handles are not closed at exit (the process ends through `os._exit`); every write through a handle is flushed, so nothing is lost, and the operating system reclaims the descriptors.
- `File.list` order is the file system's (G43: unspecified).

## Refactor

A single-pass `simplify` review (reuse, simplification, efficiency, altitude) over the step's working tree, then the three reviewer suggestions, each followed by a subset run; everything under `bootstrap/antpy/`. Behaviour is unchanged except where suggestion 1 asked for a change: the 14 probes in `work/0.6/probes/` produce byte-identical stdout, stderr, and exit status under the pre-refactor and the refactored interpreter (A/B run against a copy of the pre-refactor tree).

Simplify pass:

- `fileio.py`: the eleven identical `try/except (OSError, ValueError): return failed(e, path)` blocks are one `io_result` decorator (the native returns the Ok value, an I/O exception becomes the Err against the first argument, a path or a handle; kind-check panics pass through since `AntPanic` is a `BaseException`). `FileHandle` is now `File`, so `render.py` renders it by the same rule as the sync objects (class name = namespace name) instead of a `"file"` special case. 216 lines to 209 with the pipe rewrite below included.
- `library.py`: `need_bytes(v)` (array of `uint8` to Python bytes, snapshotting the array first) and `byte_array(data)` (the inverse) in the argument-checks section; `String.fromBytes`, `String.toBytes`, `File.readBytes`, and `File.writeBytes` all use them, so the byte-array check and conversion exist once. `String.fromBytes` gains the snapshot that `File.writeBytes` already had (section 12: a data race cannot crash the interpreter).
- `interp.py`: the `try` expression's `Err` re-wrap uses `library.err` instead of a hand-built `Record(ERR, ...)`, as the "one definition" comment in `library.py` promises.
- `values.py`: comment follows the rename.

Reviewer suggestions:

1. `File.pipe` reports a failure against the side it happened on: the read is in its own `try` (Err names `src.path`), the write and the final flush in another (Err names `dst.path`). Verified with a probe piping into a handle from `File.open` (`cannot write: <dst path>`, was the source path) and into a closed handle (`write to closed file: <dst path>`); a read failure still names the source and the success path is unchanged. `pipe` therefore does not use `io_result`.
2. `Json.parse`'s lone-surrogate walk is iterative with an explicit stack, so no `RecursionError` can escape the Result; a lone escape nested 100,000 arrays deep, or in an object key, is still `Err lone surrogate escape`.
3. `Json.stringify`'s cycle check and `String.from`'s (`render_in`) keep a set of the `id()`s of the containers being emitted instead of scanning a list: O(1) per container. Stringifying a 100,000-deep array went from 31.3 s to 0.14 s (the whole probe, parse included).

Acceptance after the refactor (`work/0.6/test.log`): `432 checks, 0 failures, 0 warnings (implementation mode, 431 tests)`, exit 0. `ruff check --select F401,F821,F841,F811 bootstrap/` clean.
