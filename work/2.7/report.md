# Report for step 2.7 "rt-io": File, Json, Time

Acceptance: `make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier io`

Last line of the acceptance output (`work/2.7/test.log`):

```
19 checks, 0 failures, 0 warnings (implementation mode, 18 tests)
```

Baseline before the step: `19 checks, 10 failures` (the three File-handle tests and all seven `json-*` tests; `Time` and `Os` already passed).

## What was built

A runtime-only step, as the plan foresaw: `src/antc/`, `Makefile` and `bootstrap/` are untouched. The emitter resolves prelude globals by name at startup (`ant_prelude_get`), so defining `Json` and `Http` in `prelude.c` is the whole hook, and the C that `antc` emits for itself is byte-identical (the fixpoint `cmp` holds by construction; `make fixpoint` rebuilt the stages only because the archive changed).

| File | Change |
|---|---|
| `runtime/ant.h` | declares `ant_bytes_array`, `ant_bytes_of`, `ant_render_float`, `AntActive` + `ant_active_has/push`, `ant_sync_need`, `ant_ns_json`, `ant_ns_http` |
| `runtime/string.c` | `ant_bytes_array` (bytes to `Array[byte]`) and `ant_bytes_of` (`Array[byte]` to a NUL-terminated buffer, `wrong kind: expected a byte` otherwise), factored out of `String.toBytes` / `String.fromBytes`, which now call them |
| `runtime/render.c` | `render_float` exported as `ant_render_float`; the cycle-detection stack exported as `AntActive` (pure renames, `String.from` unchanged) |
| `runtime/sync.c` | the static `need` exported as `ant_sync_need` (same body and phrase) |
| `runtime/io.c` | `File.readBytes`, `File.writeBytes`, the handle object and `open`, `create`, `readLine`, `writeLine`, `pipe`, `close`; `read_file` / `write_file` shared by the whole-file natives; `path_error` for `<reason>: <path>` messages; the `Http` stub |
| `runtime/json.c` | new: `Json.stringify` and `Json.parse` |
| `runtime/prelude.c` | `Http` and `Json` defined after `File` |

### File

- A handle is a `K_SYNC` object (`FileHandle { AntSync base; FILE *fp; AntString *path; }`, `base.name` the static `"File"`), so rendering as `File`, identity `==`, and hashing come from the existing sync support; `ant_sync_need(v, FILE_NAME)` gives `wrong kind: expected a File`. `fp` is NULL once closed; every read or write on a closed handle is `Err("file is closed: <path>")`, never a panic; closing twice is a no-op.
- Kind checks run in argument order before any I/O: `File.pipe(1, f)` panics without touching `f`; `File.writeBytes(path, [1])` panics `wrong kind: expected a byte` before the file is created (verified by hand).
- `readLine`: `fgetc` into a builder until `\n` or EOF; `Ok(null)` only when nothing was read at EOF; exactly one trailing `\n` dropped, a `\r` kept, a last line without `\n` is a line, an empty line is `Ok("")`; invalid UTF-8 is an Err (G44). A read on a handle opened for writing is `Err("Bad file descriptor: <path>")` through the usual errno path.
- `writeLine`: the bytes, `\n`, then `fflush` on every call, because the process ends through `_exit` (`ant_exit`), which flushes nothing but stdout/stderr.
- `pipe`: 64 KB `fread`/`fwrite` loop; a read failure is reported against the source path, a write failure against the destination; `fflush(dst)` at the end.
- `readBytes` never validates UTF-8; `read` does. Every failure is `Err(Error{ message: "<strerror>: <path>" })`.
- `Http.get` / `Http.post`: argument kinds checked (`string`; `string, string, map`), then `Err("Http is not supported")` (design section 5: stub until needed).

### Json

- `stringify` (G39, G45): compact, no whitespace. Ints as digits, finite floats through `ant_render_float` (so `1.0` stays `1.0`, `1e21` as `String.from`), NaN/Infinity, functions, tags, sync objects, void, modules and a container already being emitted (a cycle) as `null`; strings with `"` `\` and the control characters escaped (`\n \r \t \b \f`, else `\u00XX`), everything else literal; records as objects in shape order without their tag; map keys through the top-level `String.from` rendering, then quoted (`Map{ 1: "one" }` is `{"1":"one"}`). Never panics.
- `parse`: a hand-written RFC 8259 recursive-descent parser over the string's bytes (valid UTF-8 by construction, so non-escape bytes are copied through). Whitespace is space/tab/LF/CR; literals exact; numbers validated against the JSON grammar and then `strtod`, always `float64` (G38; `1e999` parses as Infinity); strings reject raw control characters and unknown escapes, decode `\uXXXX`, require a high surrogate to be followed by a `\u` low surrogate and combine them into one code point, reject lone surrogates (G39); objects keep the last duplicate key; a depth cap of 10000 makes hostile nesting an `Err("nesting too deep ...")` instead of a stack overflow; trailing content is an error. Every error is `Err(Error{ message: "<what> at offset <n>" })`; only a non-string argument panics (`wrong kind: expected a string`).

## Decisions and deviations from the plan

- The plan said to copy render.c's 15-line `Active` helper into json.c; instead it is exported as `AntActive` / `ant_active_has` / `ant_active_push` so the two walkers share one implementation. Reason: no duplicated code for reviewers to keep in sync.
- The handle keeps the caller's `AntString *` (immutable, kept alive by the scanned handle) instead of copying the path bytes as the plan sketched; same behaviour, less code.
- `io_error` is now a one-liner over `path_error(what, path)`, which the closed-handle and invalid-UTF-8 messages also use.
- The GIL is held during every file operation, as the plan chose (design section 3 would release it around pure syscalls; that needs a guard against a concurrent `File.close` of the same handle first). Regular files do not block meaningfully and no test reads a FIFO or a tty.

## Verification

1. After the shared helpers: `--tier run --tier panic --filter /stdlib/`: `70 checks, 0 failures`.
2. `--tier io` with the new runtime through the committed `bin/antc`: `19 checks, 0 failures`.
3. Hand probes (scratch programs, not tests): readLine/writeLine/pipe on handles opened the other way, closed handles, close twice, `\r\n`, a last line without `\n`, an empty line, invalid UTF-8 through `read` and `readLine`, empty `readBytes`/`writeBytes`; the panic order of `File.pipe(1, f)`, `File.writeBytes(p, [1])`, `File.readLine(Mutex.new())`, `Json.parse(1)`, `Http.post("u", "b", 1)`; for Json: `{bad`, `[1, 2`, `01`, `[1,]`, `"\ud800"`, `"\ude00"`, `"\ud83dA"`, `"a\u0000b"`, `1e999`, empty and blank input, `{"a":1}x`, duplicate keys (one entry, value 2), `NaN`, `Infinity`, `nul`, `True`, `'s'`, `1.`, `.5`, `+1`, `-0`, `0x10`, raw tab in a string, `\x41`, a short `\u`, a 90-digit number, 20000 nested `[` (Err) and 9000 balanced (Ok); stringify of nested records, a self-containing array (`[1.0,null]`), escapes, `float32`, `uint64` max, void/tag/sync, non-string map keys, `Result.ok(1)` (`{"value":1}`).
4. Regression of the 2.5 subset (`--tier run --tier panic --tier unchecked --tier programs --exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer`), because `string.c` and `render.c` changed: `396 checks, 0 failures` (`work/2.7/regression.log`).
5. The acceptance command (`work/2.7/test.log`), then `--consistency --tier io`: `19 checks, 0 failures, 0 warnings (implementation mode, 18 tests)` (`work/2.7/consistency.log`).
6. `make runtime` builds without warnings under `-Wall -Wextra`.

## Test changes

None.

## Known limitations

- File I/O runs under the GIL: a goroutine blocked in a slow `fread` (a FIFO, a tty) would stall the others. Releasing the GIL around the syscall needs a per-handle "in use" guard so that `File.close` from another goroutine cannot free the stream underneath it.
- A handle that is never closed leaks its descriptor and libc buffer until the process ends (no finalizer); its lines are not lost, since `writeLine` flushes.
- A stream's error flag is sticky: after one failed read on a handle, later `readLine` calls on it keep returning the Err (correct for a write-only handle; for a transient error, reopen).
- `strerror` text is platform-specific; tests only require a non-empty message.
- `Json.stringify` recurses as deep as the value is nested, like `String.from`; `Json.parse` caps nesting at 10000.

## Refactor

A single-pass `simplify` review (reuse, simplification, efficiency, altitude; done inline, without the four-agent fan-out) over the step's diff, then the reviewer's two suggestions, one at a time, each followed by the matching `--filter` subset of the acceptance command and the full command at the end. Behaviour is unchanged; each change was probed by hand as well.

| File | Change |
|---|---|
| `runtime/json.c` | `parse_array` / `parse_object`: the copy-pasted exit `ps->p++; ps->depth--; return out;` (four sites) became `leave(ps, out)`, the pair of `enter(ps)`. |
| `runtime/json.c` | `emit()`: the eight integer kinds, `K_BOOL` and `K_NULL` go through `ant_render_into(sb, v)` instead of a second `%lld`/`%llu`/`true`/`false`/`null` formatting (their `String.from` text is their JSON text); only the float case stays separate for the non-finite -> `null` rule. The local `char buf[32]` is gone. (Reviewer suggestion 1.) |
| `runtime/io.c` | `path_error()`: the fixed `char buf[1024]` (which silently truncated a long path) became a GC buffer sized from `strlen(what) + 2 + strlen(path)`, so an Err message always carries the whole path. (Reviewer suggestion 2.) |

Skipped on purpose: a shared walker between `render_in` and `emit_container` (a visitor abstraction would cost more than the ~35 structurally similar lines; every node prints differently); merging `quote()` with `render_quoted` (different escape sets); the per-byte `fgetc` loop in `readLine` (not a hot path; chosen for NUL safety); the out-parameter shape of `read_file` (the alternatives are no shorter).

Verification: `--tier io --filter json` after each `json.c` change (`8 checks, 0 failures`), `--tier io --filter file` after the `io.c` change (`7 checks, 0 failures`); hand probes: `Json.stringify` of every scalar kind, `uint64` max, `-0.0`, `1e21`, `float32(0.1)`, void/func in containers, non-string map keys and `Result.ok(1)` (output identical to before); `File.read` of a 3013-byte path gives `File name too long: /nonexistent/xxx...` with the path whole. `make runtime` without warnings. The acceptance command (`work/2.7/test.log`): `19 checks, 0 failures, 0 warnings (implementation mode, 18 tests)`.
