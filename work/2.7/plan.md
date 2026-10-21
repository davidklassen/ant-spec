# Plan for step 2.7 "rt-io": File, Json, Time

Row: `File, Json, Time`; directories `runtime/`, `src/antc/`, `Makefile`, `bootstrap/antpy/system.py` (Os.exec only).
Acceptance: `make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier io`.

## 0. Baseline and scope

Baseline against the committed `bin/antc` (18 io tests): `19 checks, 10 failures`. Passing already: `file-list`,
`file-read-missing`, `file-roundtrip`, `os-args`, `os-env`, `os-exit-defers-goroutine`, `os-exit-status`,
`time-now-monotonic-sleep`. Failing: `file-bytes-roundtrip` (`no field: writeBytes`), `file-lines`
(`no field: create`), `file-pipe-copy` (`no field: open`), and all seven `json-*` (`unbound name: Json`).

So `Time` and `Os` are complete (step 2.6 did `Time.sleep`); do not touch them. The work is: `File.readBytes`,
`File.writeBytes`, the five handle natives plus `File.close`, and the whole `Json` namespace. This is a
runtime-only step: the emitter resolves every prelude global dynamically (`init_prelude_refs` does
`G_Json = ant_prelude_get("Json")`, `ant_global` panics `unbound name` on `K_UNBOUND`), so defining `Json`
in `prelude.c` is enough. Do not change `src/antc/`, `Makefile` (`RT_SRC` is a wildcard, so a new `runtime/json.c`
is picked up) or `bootstrap/`. The C `antc` emits is then byte-identical and the fixpoint holds by construction.

Fast iteration: test programs link `runtime/libantrt.a` when `antc build` runs, so during development
`make runtime` is enough to see a change through the existing `bin/antc`; `make fixpoint` (rebuilds stage1
under antpy, ~1 min, then stage2/3) is needed only for the final acceptance run.

## 1. Files

| File | Change |
|---|---|
| `runtime/ant.h` | declare `ant_ns_json`, `ant_ns_http`, `ant_render_float`, `ant_bytes_array`, `ant_bytes_of`, `ant_sync_need` |
| `runtime/string.c` | the two byte helpers; `s_to_bytes` / `s_from_bytes` rewritten over them |
| `runtime/render.c` | `render_float` becomes the exported `ant_render_float` (pure rename; `String.from` output unchanged) |
| `runtime/sync.c` | the static `need(v, name)` becomes the exported `ant_sync_need` (same body, same phrase) |
| `runtime/io.c` | `File.readBytes`, `File.writeBytes`, the handle object, `open`, `create`, `readLine`, `writeLine`, `pipe`, `close`; the `Http` stub; header comment updated |
| `runtime/json.c` | new: `Json.parse`, `Json.stringify`, `ant_ns_json` |
| `runtime/prelude.c` | `ant_prelude_define("Json", ant_ns_json())` and `("Http", ant_ns_http())` after `File` |
| `work/2.7/report.md`, `work/2.7/test.log` | report; acceptance output |

## 2. Shared helpers (do first)

- `string.c`: `AntValue ant_bytes_array(const char *p, int64_t n)` builds an array of `K_UINT8` values
  (`ant_mk(K_UINT8)`, `as.u = byte`, exactly what `s_to_bytes` does now). `char *ant_bytes_of(AntValue v,
  int64_t *n)` checks `ARR` (`wrong kind`), then every element is `K_UINT8` else `ant_panic("wrong kind:
  expected a byte")` and returns NULL, else returns a `GC_MALLOC_ATOMIC` buffer of the bytes (NUL-terminated,
  as now). `s_to_bytes` and `s_from_bytes` call them; behaviour identical.
- `render.c`: rename `static void render_float` to `void ant_render_float(AntSb *sb, double x, bool f32)`.
- `sync.c`: rename `need` to `void *ant_sync_need(AntValue v, const char *name)`; the call sites inside
  `sync.c` follow. The phrase stays `wrong kind: expected a <name>` / `an Atomic`; for the file handle it
  yields `wrong kind: expected a File`. Identity is the `name` pointer, so `io.c` passes its own static array.

## 3. File (io.c)

Handle: a `K_SYNC` object, as the design's "Sync renders as its namespace name; compares by identity" already
gives rendering `File` (render.c), identity `==` and hashing (equal.c) for free; the bootstrap did the same
(kind `"file"`, renders `File`).

```c
static const char FILE_NAME[] = "File";                 /* not FILE: that is libc's type */
typedef struct { AntSync base; FILE *fp; const char *path; } FileHandle;   /* fp NULL once closed */
```
`ant_alloc(sizeof(FileHandle))` (scanned: `path` is a GC string copy); `base.name = FILE_NAME`;
`ant_ptr(K_SYNC, h)`. `need_file(v)` is `ant_sync_need(v, FILE_NAME)`. A closed handle makes every
read/write return `Err("file is closed: <path>")`; never a panic. Errors keep the existing `io_error(errno,
path)` form `strerror: path`. All kind checks run in argument order before any I/O (`File.pipe(1, f)` panics
`wrong kind` without touching `f`; `File.writeBytes(p, [1])` panics before the file is created).

Natives (member table order: read, readBytes, write, writeBytes, exists, remove, list, open, create, readLine,
writeLine, pipe, close; params as in stdlib.md: `path`, `path,bytes`, `f`, `f,line`, `src,dst`):

- `readBytes(path)`: factor the fread loop of `file_read` into `static bool read_all(FILE*, AntSb*)`; result
  `Ok(ant_bytes_array(sb.data, sb.len))`; no UTF-8 check. `file_read` keeps its check (invalid UTF-8 → Err).
- `writeBytes(path, bytes)`: `STR` path, `ant_bytes_of` (panics before opening), then the `file_write` body
  (`fopen "wb"`, `fwrite`, `fclose`, short write or close failure → Err). Factor `write_all(path, p, n)` and
  use it from `file_write` too.
- `open(path)`: `fopen(path, "rb")` → `Ok(handle)` or `Err`. `create(path)`: `fopen(path, "wb")`.
- `readLine(f)`: handle check; closed → Err. Read with `fgetc` into an `AntSb` until `\n` or EOF (no
  `getline`/malloc, NUL bytes are fine). If nothing was read and EOF: `Ok(ANT_NULL)`. If `ferror(fp)`:
  Err (a handle opened `"wb"` gives EBADF here, which is the right answer). Drop exactly one trailing `\n`,
  keep a `\r` (G44); a last line without `\n` is still a line; an empty line is `Ok("")`. Validate
  `ant_utf8_valid` before `ant_sb_finish` (else `Err("invalid UTF-8: path")`; `ant_str_new` trusts its input).
- `writeLine(f, line)`: handle check, `STR` line, closed → Err; `fwrite` the bytes then `"\n"`, then
  `fflush`; any failure → Err. The flush is required: the process ends through `_exit` (`ant_exit`), which
  flushes nothing but stdout/stderr, so an unclosed handle would lose its lines at `Os.exit`/panic (the
  bootstrap flushes per line for the same reason).
- `pipe(src, dst)`: both handle checks first, then closed checks (Err names the closed side); loop `fread` 64 KB
  from `src->fp`, `fwrite` to `dst->fp`; `ferror(src)` → Err against `src->path`, short write → Err against
  `dst->path`; at EOF `fflush(dst)` (failure → Err dst) and `Ok(ANT_VOID)`. Reading from a `"wb"` handle fails
  with EBADF → Err, which `file-pipe-copy` does not need but the spec's "nothing here panics" does.
- `close(f)`: handle check; if `fp`: `fclose`, `fp = NULL`; always `ANT_VOID`; closing twice is a no-op.

Hold the GIL during every file operation (as `file_read` does today). Releasing it around `fread`/`fwrite`
would need a guard against a concurrent `File.close` on the same handle; regular files do not block
meaningfully and no test reads a FIFO or a tty. Record it as a known limitation with that sketch.

`Http` (design section 5: "stub it to return Err until needed"): `Http.get(url)` and `Http.post(url, body,
headers)` do `STR`/`MAP` checks and return `Err(Error{ message: "Http is not supported" })`. Define
`ant_ns_http` in `io.c`. Do it last; it is not on the acceptance path.

## 4. Json (json.c, new)

### stringify (do before parse: it is the smaller half and two tests need only it)

`static void emit(AntSb *sb, AntValue v, Active *active)` with the same active-container stack as render.c
(copy the 15-line `Active` helper; it is static there). Compact, no whitespace (G45):

- `K_STRING`: JSON quoting: `"` → `\"`, `\` → `\\`, `\n \r \t \b \f` by name, other bytes < 0x20 as
  `\u00XX`; everything else (including `/`, DEL and non-ASCII UTF-8) literal. One `static void
  quote(AntSb*, const char*, int64_t)` shared with keys.
- the eight int kinds: digits (`%lld` / `%llu`); `K_BOOL`: `true`/`false`; `K_NULL`: `null`.
- `K_FLOAT64` / `K_FLOAT32`: finite → `ant_render_float(sb, x, f32)` (so `1.0` stays `1.0`, `1.5`, `-0.0`,
  `1e21` as `String.from` renders them, G45); NaN and ±Infinity → `null` (G39).
- `K_VOID`, `K_FUNC`, `K_TAG`, `K_SYNC`, a module record (`tag == ant_module_tag()`), anything else → `null`.
- `K_ARRAY`: `[` elements separated by `,` `]`.
- `K_MAP`: `{` then per live entry (`ant_map_slots`/`ant_map_entry`) `quote(ant_render(key))` `:` value `}`;
  the key text is the top-level `String.from` rendering (`ant_render`: a string bare, `1` → `1`, `1.0` → `1.0`),
  so `Map{ 1: "one" }` → `{"1":"one"}`.
- `K_RECORD`: `{` fields in shape order as `"name":value` `}`, no tag (`{"x":1.0,"y":2.0}`).
- A container already on the active stack → `null` (the bootstrap's choice; `...` is String.from's).

`Json.stringify(value)` = builder, `emit`, `ant_sb_finish`. Never panics (no kind check: any value is accepted).

### parse

Hand-written recursive descent over the string's bytes (the input is an Ant string, hence valid UTF-8; copy
non-escape bytes through verbatim).

```c
typedef struct { const char *p, *end; const char *msg; int64_t at; int depth; } P;
```
`Json.parse(text)`: `STR` check (`wrong kind` otherwise), parse one value, skip whitespace, anything left →
error `trailing characters`; on error return `Err(Error{ message: "<msg> at offset <n>" })` (only non-emptiness
is tested). Rules:

- Whitespace: space, `\t`, `\n`, `\r` only.
- Literals `null`, `true`, `false` exact (`nul`, `True`, `NaN`, `Infinity` are errors).
- Numbers: validate the RFC 8259 grammar by hand (`-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`; leading
  zeros, `+`, `.5`, `1.`, hex are errors), then `strtod` on that span (copy to a stack buffer with a NUL);
  always `ant_float` (`"3"` → `3.0`; never an int kind). Overflow to ±inf is accepted as parsed.
- Strings: bytes until `"`; a raw byte < 0x20 is an error; escapes `\" \\ \/ \b \f \n \r \t \uXXXX` (four hex
  digits, either case). `\u` in D800–DBFF must be followed immediately by `\u` in DC00–DFFF, combined to one
  code point (`"😀"` → U+1F600, `String.length` 1); a lone high or low surrogate is an error.
  Append escapes through `ant_sb_append_cp`; `\u0000` is allowed. Any other escape is an error.
- Arrays: `[` ws `]` or values separated by `,`; a trailing comma is an error. Objects: keys must be strings,
  `:`, values; `ant_map_set` per pair, so the last duplicate wins (position of the first; equality does not
  care). Empty text or only whitespace → error `unexpected end`.
- Depth: `++depth` on `[`/`{`, cap at 10000 → error `nesting too deep` (main runs on a 512 MB stack and
  goroutines on 64 MB, but the cap keeps a hostile input from being a crash instead of an Err).
- Nothing in `parse` can panic once the argument is a string; it returns `ant_ok(value)`.

`ant_ns_json`: `{ "parse", "text", j_parse }, { "stringify", "value", j_stringify }`; namespace `Json`.

## 5. Order of work and verification

1. Helpers (section 2); `make runtime`; `--filter /stdlib/ --filter /types/` subset of the run tier must stay
   green (`String.toBytes`/`fromBytes`, float rendering): run
   `python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier run --tier panic --filter /stdlib/`.
2. `readBytes`/`writeBytes` → `io/file-bytes-roundtrip`.
3. Handles → `io/file-lines`, `io/file-pipe-copy`. Probe by hand (not tests): readLine on a `"wb"` handle, close
   twice, writeLine after close, a last line without `\n`, an empty line, a file with `\r\n`.
4. `Json.stringify` + prelude entry → `io/json-stringify-edge`; `io/json-stringify-record-no-tag` also needs
   parse.
5. `Json.parse` → the remaining five json tests. Probe by hand: `{bad`, `[1, 2`, `01`, `[1,]`, `"\ud800"`,
   `"a\u0000b"`, `1e999`, `  `, `{"a":1}x`, `{"a":1,"a":2}` (one entry, value 2), deep nesting.
6. `Http` stub; `make runtime` without warnings (`-Wall -Wextra`).
7. Regression: the 2.5 acceptance subset (`--tier run --tier panic --tier unchecked --tier programs
   --exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer`),
   because `string.c` and `render.c` changed.
8. The acceptance command, output to `work/2.7/test.log`; last line `0 failures`. Then `--consistency` on
   `--tier io` once for the gate's sake.

## 6. Spec rules easy to get wrong

- stdlib.md's one rule: every failure is `Err(Error{ message })`, never a panic; only an argument of the wrong
  kind panics `wrong kind` (section 12's library rule). `File.exists` is a plain bool, never an Err.
- `File.readLine` returns `Ok(null)` at end of file, not an Err; strips exactly one `\n` (G44). `try File.readLine(f)
  ?? break` relies on that: `ant_try` unwraps the Ok to `null`, `??` breaks.
- `File.writeLine` appends `\n` (G44); `file-lines` checks the file is exactly `"first\nsecond\n"`.
- JSON numbers are always `float64` (`j is float64`, then `int(3.0)` converts, G38); never produce an int kind.
- `Json.stringify`: sized ints as bare digits, floats as `String.from` (`1.0` keeps its `.0`), NaN/Infinity →
  `null`, functions/tags/sync/void → `null`, records without tag in field order, map keys through `String.from`
  then quoted, no whitespace at all (G39, G45).
- `Json.parse`: surrogate pairs become one code point, lone surrogates fail, duplicates keep the last (G39);
  trailing content, trailing commas, leading zeros, `NaN`, single quotes, comments, raw control characters fail.
- `File.writeBytes` elements must be `uint8` (`byte(255)`), as `String.fromBytes` demands (`wrong kind: expected
  a byte`); `File.readBytes` never validates UTF-8, `File.read` does.
- Kind checks before I/O, in argument order; a `wrong kind` must not leave a half-written file behind.
- The phrase style of this runtime is `wrong kind: expected a File` (2.3 report, probes), not bare `wrong kind`.

## 7. Risks

- Unflushed `FILE*` streams at process exit: `ant_exit` is `fflush(stdout/stderr); _exit`. Mitigated by the
  per-call `fflush` in `writeLine` and the final flush in `pipe`; a handle that is never closed leaks its libc
  buffer and descriptor (no finalizer; note it).
- Deep JSON recursion and the C stack: the depth cap. Keep the parser's frames small (no large local buffers in
  the recursive function; the number buffer lives in the leaf).
- `ant_render_float` export must not change `String.from`: it is a rename; run the stdlib subset.
- `strerror` text is platform-specific; tests only require a non-empty message.
- `make fixpoint` rebuilds stage1 under antpy because the archive changed: slow but expected; the `cmp` holds
  because no `.ant` source changes. Run it once at the end, from the repository root.
- Name clashes in `io.c`: `FILE` is libc's type (use `FileHandle`/`FILE_NAME`); `json.c` must not define
  `quote`/`emit` with external linkage (keep them static).
- The GIL is held during file I/O (limitation; design section 3 would release it around pure syscalls, which
  needs a close-while-reading guard first).
