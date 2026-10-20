# Plan for step 0.6 "antpy-io": File, Json, Time

Deliverable (ROADMAP.md row 0.6): the `File`, `Json`, and `Time` namespaces of `stdlib.md`, written only under `bootstrap/` (plus `work/0.6/`). Acceptance is the 0.5 command plus `--tier io --filter io/`.

## 1. Baseline

`python3 -I src/tests/run.py --run ... --tier io --filter io/ -v` (log it to `work/0.6/baseline.log`): 19 checks, 14 failures, all `unbound name: File`, `unbound name: Json`, or `no field: now` (Time). The four `os-*` tests already pass (0.5 built `Os`). No test outside `src/tests/io/` in the acceptance selection uses `File`, `Json`, `Time.now`, or `Time.monotonic` (verified by grep; the only other user is `check/stdlib/json-narrow-before-index.ant`, a check-tier test outside this step). So the step is purely additive: new natives, no evaluator change.

## 2. Files

| File | Change |
|---|---|
| `bootstrap/antpy/fileio.py` | **new**: the `FileHandle` value class (kind `"file"`), the thirteen `File` natives, registry `FILE` |
| `bootstrap/antpy/jsonio.py` | **new**: `Json.parse`, `Json.stringify`, registry `JSON` |
| `bootstrap/antpy/system.py` | add `Time.now`, `Time.monotonic`, and `Time.sleep` (moved here from `sync.py`, rewritten as a monotonic-deadline loop); registry `TIME`; docstring: "the Os and Time namespaces" |
| `bootstrap/antpy/sync.py` | delete the `Time` section (`time_sleep`, `TIME`) and the `import time` if nothing else uses it; docstring: drop "and `Time.sleep`" |
| `bootstrap/antpy/library.py` | add the Result constructors `ok(v)`, `err(e)`, `error(message)` next to `entry` (import `OK`, `ERR`, `ERROR` from `values`) |
| `bootstrap/antpy/prelude.py` | import `TIME` from `system`, `FILE` from `fileio`, `JSON` from `jsonio`; add `"File": FILE`, `"Json": JSON` to `NAMESPACES`; `result_ok`/`result_err` become `library.ok`/`library.err` (one definition of the `Ok`/`Err` shape); docstring |
| `bootstrap/antpy/equal.py` | `REFERENCE_KINDS` gains `"file"` (identity equality and hashing, like sync objects) |
| `bootstrap/antpy/render.py` | after the `"sync"` branch: `if k == "file": return "File"` |
| `bootstrap/antpy/values.py` | only the comment near `KIND` listing where other kinds are registered (`fileio.py` registers `"file"`) |
| `bootstrap/antpy/__init__.py` | module list: `sync` without Time, `system` = Os and Time, add `fileio`, `jsonio` |
| `work/0.6/plan.md`, `report.md`, `test.log`, `probes/*.ant` | this plan, the report, the acceptance log, hand probes |

Import graph stays acyclic: `values <- equal <- arith, render <- library <- sync, system, fileio, jsonio <- prelude <- interp`. `fileio.py` and `jsonio.py` import only `values`, `equal`, `render`, `library`.

Conventions to keep (from 0.3 to 0.5): a registry maps a field name to `(param names, fn)` or `(param names, fn, True)` for a ctx native; parameter names are the ones `stdlib.md` prints so named arguments resolve; every native checks argument kinds first with the `need_*` helpers (`wrong kind` panics), then does the work; `type(v) is ...`, never `isinstance`; no Python exception may escape a native (it would be reported as `internal interpreter error` and skip deferred blocks).

## 3. Design

### 3.1 The file-handle kind

```python
class FileHandle:
    __slots__ = ("f", "path")       # f: the Python binary file object; path: for error messages
KIND[FileHandle] = "file"           # at module level in fileio.py, as sync.py does for "sync"
def need_file(v): return need(v, FileHandle, "a File")
```

A handle is a reference value: `==` and map hashing by identity (`equal.REFERENCE_KINDS`), `String.from` renders `File`, `Json.stringify` renders `null`. `f is File` is `IsTag` on the namespace record and panics `not a tag` (untested, leave it). Nothing else in the bootstrap enumerates kinds exhaustively (`arith` answers `not a number`, `library.need_*` answer `wrong kind`, `lower.KIND_TESTS` is a fixed table), but probe `for x in f` and `f.x` to be sure they panic `not iterable` / `not a record` rather than crash.

### 3.2 Result helpers (`library.py`)

```python
def ok(v):            return Record(OK, {"value": v})
def err(e):           return Record(ERR, {"error": e})
def error(message):   return Record(ERROR, {"message": message})
```

The `Err` shape is exactly `Err{ error: Error{ message } }`: the tests read `r.error.message`, `try` reads `error`, and `is Ok`/`is Err` test the `OK`/`ERR` tags from `values.py`. In `prelude.py` bind `"ok": (["value"], ok)`, `"err": (["error"], err)` and make `result_map`/`result_map_err` use `ok`/`err`; delete the duplicate bodies.

### 3.3 `fileio.py`

All paths are `need_string`; every I/O failure is `err(error(message))`, never a panic (stdlib.md: "every operation that can fail returns a `Result` with `Error` as the error type, and nothing here panics on bad input"); kind errors stay panics (section 12, library-function row). One helper turns an exception into the `Err`:

```python
def failed(e):
    if isinstance(e, UnicodeDecodeError): message = "invalid UTF-8"
    elif isinstance(e, OSError) and e.strerror: message = e.strerror + (": " + str(e.filename) if e.filename else "")
    else: message = str(e) or type(e).__name__
    return err(error(message))
```

Wrap each body in `try: ... except (OSError, ValueError) as e: return failed(e)`. `ValueError` covers "I/O operation on closed file", `io.UnsupportedOperation` (reading a `create`d handle, writing an `open`ed one) and `UnicodeDecodeError`; the message is always non-empty (`file-read-missing` asserts that).

| Native | Body |
|---|---|
| `read(path)` | `open(path, "rb")`, read all, `.decode("utf-8")` strict; `ok(text)` |
| `readBytes(path)` | same read; `ok([UInt8(b) for b in data])`. Elements must be `UInt8`, not `int`: `file-bytes-roundtrip` compares with `[byte(255), ...]` by `==`, which is kind-sensitive |
| `write(path, text)` | `need_string(text)`; `open(path, "wb")` inside `with`, write `text.encode("utf-8")`; `ok(VOID)` |
| `writeBytes(path, bytes)` | `need_array`, then `need(b, UInt8, "a byte")` for every element (as `String.fromBytes` does); write `bytes(bs)`; `ok(VOID)` |
| `exists(path)` | `os.path.exists(path)`, a plain `bool` (no Result) |
| `remove(path)` | `os.remove`; `ok(VOID)`; a missing path or a directory is an `Err` |
| `list(dir)` | `ok(os.listdir(dir))`: bare names, no `.`/`..`, unspecified order (G43); a missing or non-directory path is an `Err` |
| `open(path)` | `ok(FileHandle(open(path, "rb"), path))` |
| `create(path)` | `ok(FileHandle(open(path, "wb"), path))` (truncates) |
| `readLine(f)` | `need_file`; `line = f.f.readline()`; `b""` means end of file: `ok(None)`; else strip one trailing `b"\n"`, decode strict, `ok(text)`. A last line without a terminator is still returned; an empty file gives `null` at once; `"a\n"` gives `"a"` then `null` (G44) |
| `writeLine(f, line)` | `need_file`, `need_string(line)`; write `line.encode("utf-8") + b"\n"`; **`f.f.flush()`**; `ok(VOID)` |
| `pipe(src, dst)` | `need_file` both; `shutil.copyfileobj(src.f, dst.f)`; **`dst.f.flush()`**; `ok(VOID)`. Copies from the source's current position; a closed or write-only source is an `Err` through `ValueError` |
| `close(f)` | `need_file`; `f.f.close()` inside `try/except OSError: pass`; returns `VOID`. Closing twice is a no-op (Python's `close` is idempotent) |

Why the flushes: the process ends through `os._exit` (main return, `Os.exit`, panic), which skips Python's buffered-file flushing, so a handle that is never closed would silently lose everything written through it. Flushing after every handle write keeps the data on disk whatever exit path follows. `read`/`write`/`readBytes`/`writeBytes` use `with` and need nothing.

Registry `FILE`, in stdlib.md order, with its parameter names: `read(path)`, `readBytes(path)`, `write(path, text)`, `writeBytes(path, bytes)` (Python parameter `bs`), `exists(path)`, `remove(path)`, `list(dir)`, `open(path)`, `create(path)`, `readLine(f)`, `writeLine(f, line)`, `pipe(src, dst)`, `close(f)`. None is a ctx native.

### 3.4 `jsonio.py`

**`Json.parse(text)`**: `need_string(text)`, then Python's `json.loads` with three hooks, all verified against this machine's Python 3.15:

```python
v = json.loads(text, parse_int=float, parse_constant=reject_constant, object_pairs_hook=to_map)
```

- `parse_int=float`: every number is a Python `float` (the `float64` kind): `"3"` gives `3.0`, `"[1, 2]"` gives `[1.0, 2.0]`, `"-0"` gives `-0.0`, a huge integer rounds, `"1e400"` is `inf` (a `float64`; allowed, untested). Never a Python `int` (that would be kind `int64` and break `j is float64`) and never `Float32`.
- `reject_constant(name)`: `raise ValueError("not a JSON value: " + name)`. Python accepts `NaN`, `Infinity`, `-Infinity`; JSON does not.
- `to_map(pairs)`: `m = AntMap(); for k, v in pairs: m.set(k, v); return m`. Keys are `str`; later duplicates overwrite, so the last one wins (G39). Arrays come back as Python `list`, which is Ant's array type; `null`/`true`/`false`/strings map to `None`/`bool`/`str` directly. No conversion walk is needed.
- `except (ValueError, RecursionError) as e: return err(error(str(e) or "invalid JSON"))`. `JSONDecodeError` is a `ValueError`; its messages ("Expecting ',' delimiter: line 1 column 6 (char 5)") are non-empty, which `json-parse-error` asserts. Empty or whitespace-only text, trailing data (`01`, `1.`), `.5`, a trailing comma, a BOM, a raw control character in a string, and `\x` escapes are all rejected by Python already.
- Surrogates: Python combines `\ud83d\ude00` into one code point (G39: `json-parse-unicode` wants `String.length` 1), but turns a lone `\ud83d` into a lone-surrogate `str` that no UTF-8 encoder accepts, so `print` would crash the interpreter. After a successful parse, if `"\\u" in text`, walk the value (strings, list elements, map keys and values via `items()`) and return `err(error("lone surrogate escape"))` when any string holds a code point in `U+D800..U+DFFF`. Skip the walk when the text has no `\u` escape.

**`Json.stringify(value)`**: a hand-written emitter over Ant values into a list of string pieces (not `json.dumps` of the whole value: it cannot see `AntMap`/`Record`, prints NaN as `NaN`, and lays floats out as Python does, `1e+21` instead of G33's `1e21`). `emit(v, out, path)` by `KIND[type(v)]`:

| Kind | Text |
|---|---|
| string | `json.dumps(v, ensure_ascii=False)`: escapes `"`, `\`, control characters (`\n`, `\t`, `\r`, `\b`, `\f`, `\u00XX`), keeps non-ASCII literal |
| int kinds (incl. sized) | `str(int(v))` (`int8(5)` is `5`, G39) |
| float kinds | `null` when not `math.isfinite(v)` (NaN, ±Infinity, G39); else `render_float(v, k == "float32")`, the same text as `String.from` (G45: `1.0`, `2.0`, `1.5`, `1e21`, `-0.0`, all valid JSON) |
| bool, null | `true`/`false`, `null` |
| void, func, tag, sync, file | `null` (G39 for func/tag/sync; void and file by analogy) |
| array | `[` elements joined by `,` `]` |
| map | `{` entries in `m.items()` order, each `json.dumps(render(k), ensure_ascii=False) + ":" + value` `}`: the key goes through top-level `String.from` (a string key renders bare, so `"k"` becomes `"k"`, not `"\"k\""`; `Map{ 1: "one" }` becomes `{"1":"one"}`, G39). Map order is the map's own, unspecified; tests use single-entry maps or compare after a parse |
| record (any tag, including `Module`) | `{` `"name":value` per field in `fields.items()` order (literal/declaration order, G45) `}`, no tag anywhere (`json-stringify-record-no-tag` asserts the text has no `Point`) |

No whitespace anywhere (G45). Cycles: keep a `path` list of containers being emitted as `render_in` does; a back-reference emits `null` (JSON has no `...` token, `stringify` has no `Result` to fail through, and `null` keeps the output valid JSON; untested, record it).

Registry `JSON = {"parse": (["text"], json_parse), "stringify": (["value"], json_stringify)}`.

### 3.5 `Time` (`system.py`)

```python
def time_now():        return time.time_ns() // 1_000_000        # ms since the Unix epoch, an int
def time_monotonic():  return time.monotonic_ns() // 1_000_000   # ms from an arbitrary origin
def time_sleep(ms):
    need_int(ms)
    deadline = time.monotonic_ns() + ms * 1_000_000
    while True:
        remaining = deadline - time.monotonic_ns()
        if remaining <= 0: return VOID
        time.sleep(remaining / 1e9)                              # releases the GIL: only the caller blocks
```

Why a deadline loop on the same integer clock: `time-now-monotonic-sleep` asserts `c - a >= 50` around `Time.sleep(50)` with truncating millisecond reads. With `a = a_ns // 10**6` and `c_ns >= a_ns + 50 * 10**6`, `c >= a + 50` holds exactly in integer arithmetic; a plain `time.sleep(0.05)` can return a few microseconds short as measured by `monotonic_ns` (different clock path, float rounding), which would read as 49. Zero or negative `ms` returns at once (the first check). Registry `TIME = {"now": ([], time_now), "sleep": (["ms"], time_sleep), "monotonic": ([], time_monotonic)}`. The results are plain Python `int` (kind `int64`), so `Time.now() > 1577836800000` compares like kinds.

## 4. Order of work

1. Plumbing: `library.ok/err/error`, `prelude` rebinding of `Result.ok`/`Result.err`, the `"file"` kind lines in `equal.py` and `render.py`. Check: `--tier run --tier panic --tier unchecked --filter /errors/ --filter /nullability/` is still 0 failures (these tiers exercise `Result` and `try`).
2. `Time`: `system.py` gains the three functions, `sync.py` loses its Time section, `prelude` imports `TIME` from `system`. Check: `--tier io --filter time-` and `--tier run --tier panic --tier programs --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer` (the `Time.sleep` users).
3. `File`: `fileio.py`, wired into `prelude`. Check: `--tier io --filter file-` (six tests).
4. `Json`: `jsonio.py`, wired in. Check: `--tier io --filter json-` (seven tests).
5. The full acceptance command (verbatim from the task) with output to `work/0.6/test.log`; it should end `433 checks, 0 failures, 0 warnings (implementation mode, 431 tests)` or thereabouts (0.5 had 414 checks over 413 tests; this step adds 18 tests and 19 checks). Then the same command with `--consistency` (0.G needs it; the io tier is not part of consistency, so this is a regression check only), and `ruff check --select F401,F821,F841,F811 bootstrap/`.
6. Probes under `work/0.6/probes/` for the untested choices in section 6; results into `report.md`.

## 5. Spec rules that are easy to get wrong

- **Kind errors panic, I/O errors return.** `File.write(1, "x")` is `panic: wrong kind`; `File.write("/nonexistent/dir/x", "x")` is `Err`. Do the `need_*` checks before the `try`.
- **`File.exists` is a bare `bool` and `File.close` a bare `void`**, not Results. `File.readLine` at end of file is `Ok(null)`, not `Err`.
- **`File.readBytes` elements are `UInt8`**, and `File.writeBytes` requires `UInt8` elements (a plain `int` is `wrong kind`), exactly like `String.fromBytes`/`toBytes`. `Expect.eq(back, bytes)` is kind-sensitive structural `==`.
- **`File.read` of invalid UTF-8 is an `Err`** (strict decode); the string type holds Unicode text. `readLine` likewise.
- **`Ok(void)`**: `write`, `writeBytes`, `remove`, `writeLine`, `pipe` return `ok(VOID)`; a `try File.writeLine(...)` statement then yields `void`.
- **JSON numbers are always `float64`**: `parse_int=float`. `Json.parse("3")` is `3.0`, and `int(number("3.0"))` is `3`, `int(number("1.5"))` is `1` through the existing truncating conversion (G38; already implemented in 0.2).
- **`stringify` floats use `String.from`'s text**: `{"x":1.0,"y":2.0}`, never `{"x":1,"y":2}` or `1e+21`; ints are bare digits; NaN and infinities are `null`.
- **Map keys through top-level `String.from`, then JSON-quoted**: `render(k)` (bare for strings), not `render_in(k, False, ...)`.
- **Record fields keep `fields` order and drop the tag.** A record built with a spread keeps the earlier position of a replaced field (as `String.from` does in 0.2); stringify must agree with `String.from` and `==` (section 4 "Equality").
- **Reject `NaN`/`Infinity` on parse** (Python accepts them); **keep the last duplicate key**; **one code point per surrogate pair**; **lone surrogate is an `Err`**.
- **`Time.sleep` keeps `need_int`** (`Time.sleep(1.0)` is `wrong kind`, as 0.4 probed) and the `ms` parameter name; `Time.now`/`monotonic` take no parameters (`Time.now(1)` is `too many arguments` through the native call path).
- **Never let a Python exception escape**: `OSError`, `ValueError` (closed file, unsupported operation, bad UTF-8, JSON errors), `RecursionError` (JSON nesting) are all caught and turned into `Err`.
- **No special-casing of test paths or names**; the temporary directory arrives as `Os.args()[0]` and needs nothing from the interpreter.

## 6. Untested choices to record in the report (and probe)

- `File.exists` on a directory: `true` (`os.path.exists`). `File.remove` on a directory: `Err`. `File.list` on a file: `Err`.
- `File.readLine` on a `create`d handle, `File.writeLine` on an `open`ed one, any operation on a closed handle: `Err` with Python's message; `File.close` twice: no-op.
- `File.readLine` strips only `"\n"`; a `"\r\n"` line keeps its `"\r"`.
- `File.pipe` copies from the source's current position (after two `readLine`s it copies the rest) and appends at the destination's position.
- `Json.parse("1e400")` is `Ok(Infinity)`; `Json.parse("\"\\ud800\"")` is `Err`; `Json.parse("NaN")` is `Err`; `Json.parse(" 1 ")` is `Ok(1.0)`; deep nesting (`"[" * 50000 + "]" * 50000`) parses (the recursion limit is 5,000,000 and the worker stack 512 MB) and stringifies back.
- `Json.stringify` of `void`, a `File` handle, a module record, a cyclic array (`null` for the back-reference), a `float32` (`render_float(v, True)`), a map with a record key (`"Point{ x: 1.0 }"` as the key text).
- `Time.sleep(0)`, `Time.sleep(-5)` return at once; `Time.now()` and `Time.monotonic()` are `int` (`is int` true).

## 7. Risks

- **Lost writes at exit**: `os._exit` skips Python's buffered flushing. Mitigated by flushing after every handle write and using `with` for the whole-file functions. Probe: `File.create` + `writeLine` with no `close`, then read the file after the run.
- **Timing flake in `time-now-monotonic-sleep`**: the deadline loop on `monotonic_ns` removes the only source of a 49 ms reading; run the test ten times in a row after the change.
- **Regression from rebinding `Result.ok`/`Result.err`** to `library.ok`/`err`: same records, same tags, but run the `/errors/` and `/nullability/` subsets right after the change, before anything else.
- **Moving `Time.sleep`** touches the concurrency tier's only timing primitive: rerun the 31-test concurrency subset after step 2 of section 4.
- **A `FileHandle` reaching code that falls through to the record branch** (render, eq, hash) would crash; the two one-line edits cover the three places, and the probes `print(f)`, `f == f`, `Map{ f: 1 }`, `for x in f`, `f.x`, `f[0]` confirm.
- **`json.loads` accepting more than JSON**: only `NaN`/`Infinity`/`-Infinity` (handled by `parse_constant`); Python already rejects comments, single quotes, trailing commas, leading zeros, and control characters (verified).
- **Lone-surrogate walk cost** on large JSON with `\u` escapes: linear, acceptable for the bootstrap.
- `ruff` must stay clean: drop `import time` from `sync.py` if `time_sleep` was its only user; no unused imports in the new modules.
- No test should need changing; if one does, it must contradict the spec, be fixed minimally with the section cited, and be logged in `work/TEST-CHANGES.md`.
