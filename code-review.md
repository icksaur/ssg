# Code review — HEAD~2..HEAD

Scope: `89343bc` (baseline repair) and `68c3239` (M9-W self-pipe signal wakeup).
Standard: `code-quality.md` (correctness first). Built and ran the affected
targets: `test_ssg_app` (249), `test_runtime_editing` (153), `test_runtime_files`
(77) all pass; `ssg` links.

---

## MUST

### 1. Escape-timeout and edge-scroll waits omit the signal pipe — SIGTERM/SIGHUP can be deferred indefinitely
**File:** apps/ssg_main.cpp:104-110 (`input_ready`), used at :471 (edge-scroll) and :529 (escape-timeout)
**Why it matters:** The M9-W spec is explicit:

> "The escape-timeout and edge-scroll `select()` calls also add the pipe fd so a
> resize during a drag is not missed."

`input_ready()` selects on `STDIN_FILENO` only; the pipe read end is never added.
The edge-scroll branch (`if (drag_edge && !input_ready(kEdgeScrollIntervalMs))`)
loops purely on stdin readiness and never drains the signal pipe. `edge_scroll`
(apps/pointer_routing.cpp:115-121) returns a direction whenever the pointer row
is past the content edge — it does **not** stop when the view can no longer
scroll — so while a user holds a drag past the top/bottom edge, the loop cycles
every 40 ms forever and a `SIGTERM`/`SIGHUP` written to the pipe is **never
observed** until the user releases (i.e. until fresh stdin arrives). The process
is unresponsive to termination signals for the whole duration of the held drag.
This defeats one of M9's stated headline goals ("the terminal is restored on ...
a terminating signal (`SIGTERM`/`SIGHUP`)"), and the terminate path is live in
*this* commit (`drain_signals()` → `quit`), not deferred to M9-X.

Note the resize case is masked (and thus easy to miss in testing) because
`refresh()` re-queries `terminal_size()` every iteration, so a `SIGWINCH` during
a drag is still reflected within ~40 ms; only the terminate signals are the
observable casualty.

**Suggested fix:** Have the escape-timeout and edge-scroll waits select on both
stdin and the pipe fd (and treat a pipe-only wake as a signal to drain), as the
spec prescribes — e.g. pass the pipe fd into `input_ready` and drain/return on a
pipe-side wake.

---

## Verified clean (points that were scrutinized and found sound)

- **Async-signal-safety (handler + fd pattern).** `signal_tag_handler`
  (apps/ssg_main.cpp:128-134) only reads the `volatile std::sig_atomic_t`
  fd and calls `write()` — both async-signal-safe; `EAGAIN` correctly ignored.
  Ordering is race-free: the pipe is created and set non-blocking, then
  `g_signal_pipe_write` is assigned (:206), and only then are handlers installed
  (:207-209), so no handler can fire with a stale/invalid fd. A handler that
  somehow fires before assignment sees `-1` and no-ops (:130).
- **Main `select()` loop (apps/ssg_main.cpp:499-519).** `max_fd` is correct;
  `EINTR` retries (:506); when both fds are ready the resize path falls through
  to the stdin `read()` (:515-517), so buffered/pending keyboard input is not
  dropped; a resize-only wake `continue`s and re-snapshots. Left-over bytes in
  `buffer` are only ever *incomplete* sequences (the inner loop drains complete
  events every pass), so a resize `continue` cannot strand a decodable event.
- **Self-pipe drain (apps/ssg_main.cpp:214-225).** Non-blocking read-until-empty
  is correct (`n <= 0` covers both `EAGAIN` and EOF); coalescing via
  `classify_signal_tags` is safe. Event loss on a genuinely full 64 KB pipe is
  not reachable in practice (the loop drains promptly).
- **Terminate path.** A `SIGTERM` while blocked in the main `select()` wakes it
  (pipe readable, or `EINTR`+re-select), `drain_signals()` returns false, `quit`
  is set, the loop exits, and the RAII `TerminalMode` destructor restores the
  terminal on the same wake — not on a later keypress. Correct for the idle case.
- **`classify_signal_tags` (apps/ssg_terminal.cpp:15-26).** Total; unknown bytes
  ignored; tag round-trips (SIGHUP/TERM/WINCH all < 256); last-terminate-wins is
  fine for the current "quit cleanly" behavior. Matches its unit tests.
- **Baseline-repair paste-reveal test (tests/runtime/test_runtime_editing.cpp:788-800).**
  Correct: caret collapsed to byte 0, paste of `"a\n"` at offset 0 leaves the
  caret at row 1; revealing upward from row 40 places the caret's row at the
  viewport top, so `first_row() == 1U`. Test passes. The `.value()` and lost
  `TEST()` header fixes are correct compile repairs.

## NIT / non-issues (not actionable)

- **Self-pipe fds never closed; handlers never uninstalled before exit**
  (apps/ssg_main.cpp:197-209). Immaterial: the fds and dispositions live for the
  whole process and are reclaimed on teardown; there is no leak across a
  boundary and no second run in-process. No change needed.
- **Startup window before handler install.** Between entering raw mode (:188)
  and installing handlers (:207-209) a `SIGTERM` would kill without restoring the
  terminal. This is an inherent, microsecond-scale startup window and no worse
  than the pre-existing state (no handlers at all); not worth hardening here.

---

# M9-X

## M9-X

Scope: `1d4d2b9` (M9-X: restore the terminal and re-raise on a terminating
signal). Standard: `code-quality.md` (correctness first). Files:
`apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp`.
(The build tree could not be regenerated due to a pre-existing, unrelated CMake
error — `test_color` has no sources in `cmake/components/color-depth.cmake` —
so findings below are by inspection; the byte-pinning was checked against the
removed inline literals directly.)

### MUST

None.

### SHOULD

None.

### NIT / non-issues (scrutinized and found sound)

- **Terminate path terminates deterministically.** `drain_signals` runs only in
  normal context (after `select()` wakes on the self-pipe), where SIGTERM/SIGHUP
  are unblocked (the handlers were installed with `sigemptyset` masks that only
  apply during handler execution). `mode.restore()` → `::signal(signo, SIG_DFL)`
  → `::raise(signo)` therefore delivers synchronously and does not return, so the
  loop is never re-entered and the exit status is `WTERMSIG == signo`. Using
  `::signal` (not `sigaction`) for the one-shot reset-to-default is acceptable
  here. No window leaves the terminal unrestored: restore precedes raise, and a
  redundant SIGTERM in the tiny gap would terminate with the terminal already
  restored. (ssg_main.cpp:241-246)

- **All three callers behave correctly with a non-returning drain.** Edge-scroll
  (`drain_signals(); continue;`, :504-505), main blocking wait
  (`drain_signals(); if (!wait.input) continue;`, :541-542), and Escape-timeout
  (`if (ready.signal) drain_signals();`, :559) each treat the resize case as
  "returns, then re-snapshot/continue" and the terminate case as "does not
  return." No path sets `quit` incorrectly or drops the pending `buffer`
  (terminate kills the process; resize preserves buffered bytes and the drag).

- **`active_` is race-free.** It is read/written only by `restore()`, which is
  invoked solely from normal context (destructor, drain, catch). The async-signal
  handler touches only `g_signal_pipe_write` and `::write`, so there is no
  concurrent access. (ssg_main.cpp:79-84, 144-150)

- **Idempotent restore across catch + destructor is safe.** `restore()` is
  `noexcept`, gated on `active_`, and clears the flag before doing work; the
  catch blocks call it and then `return 1`, after which the destructor's second
  call is a no-op. On the terminate path the process dies at `raise`, so the
  destructor never runs. Both `catch (std::exception const&)` and `catch (...)`
  cover every propagating type. (ssg_main.cpp:75-84, 710-718)

- **Sequence extraction is drift-free and pinned.** `terminal_setup_sequence()` /
  `terminal_restore_sequence()` return byte-for-byte the literals previously
  inlined in `write_all` (`\x1b[?1049h\x1b[5 q\x1b[?1000h\x1b[?1002h\x1b[?1006h`
  and `\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[0 q\x1b[?25h\x1b[?1049l`), and
  `terminal_sequences_are_inverse_control_strings` asserts both exact strings.
  (ssg_terminal.cpp:28-37, test_ssg_app.cpp:90-105)

Conclusion: no material correctness, safety, lifetime, or invariant issues found.

## M9-C

Scope: `d165e71` (M9-C1: library color-depth mapping) and `f0e0480` (M9-C2:
terminal color-depth detection and depth-aware frame encoder). Standard:
`code-quality.md` (correctness first). Files: `include/ssg/color.h`,
`src/color.cpp`, `cmake/components/color-depth.cmake`, `tests/test_color.cpp`,
`tests/test_theme.cpp`, `apps/ssg_terminal.{h,cpp}`, `apps/ssg_main.cpp`,
`tests/test_ssg_app.cpp`. Built and ran `test_color` and `test_ssg_app` (both
pass) plus hand-verification of the palette math.

### MUST
None.

### SHOULD
None.

### Verified clean (points scrutinized and found sound)

- xterm-256 index<->RGB math (`src/color.cpp:47-70`). Cube: `cube_channel`
  yields {0,95,135,175,215,255} (level0=0, level>=1 => 55+40*level); index
  decode `r=offset/36, g=(offset/6)%6, b=offset%6` over 16..231 is correct.
  Gray ramp 232..255 => `8+10*step` gives 8..238. Base16 table matches the
  standard xterm defaults. Confirmed exhaustively: `xterm256_color(i)` equals
  the independent reference for all 256 indices (test passes).
- Nearest-swatch search (`nearest_index`, `src/color.cpp:35-49`). Squared
  Euclidean in sRGB with strict `<` so ties keep the lowest index; distance
  computed in `int` (max 3*255^2 = 195075, no overflow) then widened to
  `uint32_t`. Deterministic and matches the reference over the sampled cube.
- Excluding 0..15 from the indexed256 search is intentional and not visibly
  wrong for the edge inputs raised: pure white (255,255,255) hits cube index
  231 exactly (distance 0, beats the gray ramp whose max is 238); pure black
  hits cube 16 exactly; mid-gray (128,128,128) hits gray-ramp index 244
  exactly (8+10*12=128). The cube+ramp cover near-black/near-white/pure-gray
  without the configurable system colors, so the mapping stays deterministic.
- ANSI-16 SGR mapping (`apps/ssg_terminal.cpp:80-88`). index<8 => base+index
  (30-37 fg / 40-47 bg); index>=8 => bright+(index-8) (90-97 / 100-107).
  Verified: pure red -> base16 index 9 -> fg `\x1b[91m`; black -> index 0 ->
  bg `\x1b[40m` (test passes).
- fg/bg run-dedup (`apps/ssg_terminal.cpp:107-112`) keys on the palette
  indices `cell.foreground`/`cell.background`, not the resolved terminal code.
  Correct regardless of depth: two distinct palette indices that resolve to
  the same terminal code merely re-emit an identical (harmless) escape; the
  encoder never suppresses a needed color change. Per-row `\x1b[0m` reset with
  `foreground=background=-1` forces a fresh emission at each row start.
- `detect_color_depth` precedence and null handling (`apps/ssg_terminal.cpp:
  39-52`): COLORTERM `truecolor`/`24bit` wins; else TERM containing `256color`
  -> indexed256; else ansi16. `nullptr` inputs are skipped; empty COLORTERM
  compares unequal and falls through. Matches common terminals and the test
  table.
- Default arg `ColorDepth::truecolor` on `encode_ansi_frame`: the only
  non-test caller is `apps/ssg_main.cpp:484`, which passes the detected
  `color_depth` explicitly, so the default is never silently relied upon in a
  way that would skip reduction. Detection runs once at startup
  (`ssg_main.cpp:229-230`).
- I22 invariant preserved: `resolve_color` is applied only inside the encoder's
  local `color` lambda; `screen.palette` (SrgbColor) is read, never written,
  and no resolved swatch is fed back into the theme/snapshot/API. The
  `test_theme` allowlist for `include/ssg/color.h`, `src/color.cpp`,
  `tests/test_color.cpp` is justified — these define the terminal hardware
  palette, not editor theme colors.

### NIT / non-issues (not actionable)

- The test oracle in `tests/test_color.cpp` (`ref_xterm`/`ref_nearest`) is an
  independently-authored but structurally identical transcription of the
  production algorithm (same squared-Euclidean metric, same tie-break). It
  robustly pins the index/RGB *transcription* (constants, cube divisors, ramp
  formula) but by construction cannot detect a shared *design* choice (e.g.
  sRGB rather than a perceptual metric) — which is a documented, deliberate
  choice, not a defect. No action needed.
- The broad sample uses step 15 (`test_color.cpp:87-`), which includes 255 and
  the cube/ramp endpoints but can skip an exact cube-vs-gray equidistant
  boundary. This does not weaken the guarantee: production and reference run
  the identical algorithm, so they agree at *every* point; the sampled sweep
  plus the exhaustive `exact_swatches_map_to_themselves` and the explicit
  tie case are sufficient. The `ansi16` assertions check only `index` (not
  `rgb`), but `rgb` there is `xterm256_color(index)`, which is exhaustively
  pinned against the reference for all 256 indices — so no coverage gap.

Conclusion: no material correctness, precision, logic, or invariant issues
found in M9-C1/M9-C2.

## M9-T

Reviewed commit e038af4 ("M9-T: layout totality + too-small placeholder").
Files: apps/ssg_main.cpp, apps/ssg_terminal.{h,cpp}, tests/test_ssg_app.cpp,
tests/runtime/test_runtime_totality.cpp, cmake/components/editor-runtime.cmake.

### Production code

No material correctness or invariant issues.

- `encode_too_small_frame` (ssg_terminal.cpp:53-66) is safe at every size probed
  (0x0, 1x1, 1xN, Nx1, large). The message is truncated to `columns` via
  `substr(0, min(size, columns))` so `visible.size() <= columns`; consequently
  `column = max(1, (columns - visible)/2 + 1)` keeps the last written column at
  `column + visible - 1 = (columns + visible)/2 <= columns`, so the row never
  overflows and the cursor address is always in `[1, columns]`. `row = rows/2+1`
  is in `[1, rows]` for all `rows >= 1`. The `columns<=0 || rows<=0` early-out
  correctly emits only reset/clear/home with no positioned text. No negative
  substr length, no out-of-range address.
- The app guard (ssg_main.cpp:483) `shell.viewport.columns <= 0 || rows <= 0`
  is the correct and complete too-small detector. It exactly mirrors render()'s
  own precondition (src/render.cpp:561) and the library's zeroing: both
  compute_shell_layout too-small paths (ui_layout.cpp:342 viewport_too_small and
  :477 prompt-reservation) return an error outcome, and snapshot.cpp:110
  (`if (!result.accepted()) return {};`) turns any error into a default-
  constructed ShellViewState whose `viewport == {0,0}`. A laid-out view only
  ever gets `view.viewport = request.viewport` (>= 20x4). There is no state
  where render() is reached with a zero/invalid viewport.
- Placeholder vs. normal frame transitions leave the terminal consistent: the
  placeholder issues `\x1b[2J` (full clear) so no normal-frame residue remains,
  and encode_ansi_frame repaints every row full-width so no placeholder residue
  remains. Cursor visibility is carried over rather than corrupted (see NIT
  below). Drawing the placeholder in the client (not the library) is consistent
  with library-first: render() declines the too-small state and the library
  exposes it as a typed zeroed viewport, exactly as doc/spec-terminal-
  robustness.md M9-T endorses.

### SHOULD — totality test never asserts that any size actually lays out

**File:** tests/runtime/test_runtime_totality.cpp:126-147
The oracle gates rendering on the library's *own* output (`laid_out =
shell.viewport > 0`) and only asserts a negative for the too-small side
(`below_minimum => ASSERT_FALSE(laid_out)`). For every size at/above 20x4 it
takes the `if (!laid_out) { ...; continue; }` branch on faith: there is no
assertion that any above-minimum size in the sweep is actually laid out. A
regression that made `snapshot()` return the zeroed shell for *all* sizes (e.g.
an always-taken too-small path, or `compute_shell_layout` wrongly rejecting a
valid viewport) would pass this test green — the below-minimum asserts still
hold and every above-minimum case would silently `continue`. For a test whose
stated purpose is "a laid-out shell carries exactly the requested dims," it
should assert `laid_out == true` for the sizes/states known to be layoutable
(e.g. default_doc at 80x24 / 200x60). As written the self-referential gate is a
real weakness. (Mitigated in practice by test_render / test_runtime_snapshot
exercising real layout, but this test itself would not catch the regression.)

### NIT — second, unguarded render() call defeats ASSERT_NO_THROW

**File:** tests/runtime/test_runtime_totality.cpp:154-155
`ASSERT_NO_THROW(ssg::render(*snapshot))` is immediately followed by an
unguarded `auto grid = ssg::render(*snapshot);`. The ASSERT macros in
test_helpers.h only increment a counter (they do not return/abort), so if
render() ever did throw, the ASSERT would record the failure and then the
second call would throw out of the test uncaught and abort the whole binary —
defeating the point of the NO_THROW guard (graceful failure) and also
double-rendering on every iteration. Assign the grid inside the guarded call
(or render once into a variable and assert it does not throw around that).

### NIT — leaked scratch dir on the create-failure early return

**File:** tests/runtime/test_runtime_totality.cpp:105
`if (!created.accepted()) return;` returns before the closing
`fs::remove_all(root)` (:167), leaking `runtime_totality_<state>/` under the
build dir. Only reachable on a hard setup failure, and make_root() re-cleans on
the next run, so immaterial — noting only because temp-dir hygiene was asked
about. (The counter-only ASSERT macros do not early-return, so a failing
region/geometry assertion still reaches the cleanup at :167.)

### NIT — placeholder inherits a visible cursor (cosmetic)

**File:** apps/ssg_main.cpp:487 / apps/ssg_terminal.cpp:53
The normal path shows the cursor (`\x1b[?25h`) when the frame has a caret; the
placeholder emits no hide/show, so after shrinking from a caret-bearing frame a
blinking bar cursor is left at the end of "terminal too small". Purely cosmetic,
not a state-consistency bug (growing back re-hides via the normal path's leading
`\x1b[?25l`).

Conclusion: no bugs in shipped code; the material item is the test-oracle gap
(SHOULD) — the totality test cannot detect an "always too small" regression.

## M9-U

Reviewed: `65fe265` (M9-U: end-to-end Unicode lock) and `293b1dd` (M9-T review
fold). Both tests build and pass (test_ssg_app: 312 assertions;
test_runtime_totality: 1508). Verified the golden against the four regressions
it claims to catch by tracing the source paths (compute_cell_run cell indices ->
render cell placement / screen_cell_for -> encode_ansi_frame). No material bugs.

### Assessment of the M9-U golden (tests/test_ssg_app.cpp:62)

The exact-cell golden is genuinely strong and *does* catch the named
regressions, each as a real failure (not a coincidental pass):

- Wide glyph losing its continuation: caught by `cell(3).continuation` (:116),
  independent of the caret/encode checks.
- Combining mark split from its base: `cell(4).text == ecombining` (:117) — a
  split shifts every later cell, so this and `cell(5)`/`cell(6)` all fail.
- ZWJ sequence broken into multiple clusters: `cell(5).text == emoji` (:119)
  compares the full 11-byte cluster; a break leaves only the man emoji there.
- Continuation re-emitted by the encoder: `count(cjk)/count(emoji) == 1` (:150-151)
  goes to 2. Counting over the whole frame is safe here: every SGR escape is
  ASCII, the document is this single line, and each needle carries its
  non-ASCII lead bytes (0xCC 0x81 only comes from U+0301), so no accidental
  match — count==1 is robust for this fixture.

The dynamic row-location (scan for the "a","b" run, index relative to `startx`)
is the right call: it makes the cell golden robust to a line-number gutter or a
different pane origin, so §4's brittleness concern does not apply.

Caret advance (§2): byte offset 5 is correct ('e' after the 3-byte U+4E00:
a=0,b=1,CJK=2..4,e=5). `startx+4` is content.x+4, and screen_cell_for computes
column = content.x + (cell_index - start) with start=0 (no h-scroll), so the
assertion reduces to cell_index('e')==4 = a(1)+b(1)+wide(2). It genuinely locks
+2 and cannot pass at startx+4 "for the wrong reason" in isolation, because a
compensating width error (wide -1, combining +1) is independently rejected by
the cell golden. Resolving on the raw `line` (no trailing "\n") is fine: offset
5 is far before the newline, so the (byte,line,cell) triple matches the runtime
model built from `line + "\n"` and cursor.set_position is accepted (confirmed at
runtime).

### NIT — `before` is resolved but its column is never asserted

**File:** tests/test_ssg_app.cpp:126,128
The comment (:123-125) says "byte offset 2 (before the glyph) resolves to column
startx+2", but `before` is only checked for `has_value()`; its caret column is
never dispatched/rendered/asserted. The "+2 across the wide glyph" is instead
established by the cell golden plus the offset-5 caret. Harmless, but the comment
overstates what is verified, and `before` is effectively dead.

### NIT — caret assertion is largely redundant with the cell golden

**File:** tests/test_ssg_app.cpp:139
Both the caret column and `cell(4)` derive from the same library cell-index
machinery, so `caret->column == startx+4` re-derives what `cell(4).text ==
ecombining` already fixed. It does exercise the separate screen_cell_for /
cursor.set_position path end-to-end, which has some value, but it is not an
independent oracle for width. Not a defect.

### NIT — row scan takes the first "a","b" match anywhere in the grid

**File:** tests/test_ssg_app.cpp:97-103
The scan breaks on the first row/x with cell text "a" then "b", top-to-bottom.
For this fixture no chrome row (tab bar "u.txt", status/footer) contains an
adjacent a,b, so it locks the content row correctly (verified: cell(2)==cjk). If
future chrome ever rendered "ab" on an earlier row the test would lock the wrong
row and fail spuriously — a false-failure risk only, never a false pass. Minor
future-brittleness; immaterial today.

### M9-T fold (293b1dd) — correct and closes the prior SHOULD

**File:** tests/runtime/test_runtime_totality.cpp:143-146
The positive assertion (>=80x24 must lay out in every UI state) correctly closes
the self-referential-oracle gap flagged in the previous review: an "always too
small" regression previously satisfied only the negative branches (below_minimum
-> too-small; !laid_out -> continue) and passed green. All sweep sizes >=80x24
({80,24},{200,60},{132,43}) are far above the 20x4 minimum, so find_open (too
small only at small heights) and every other state lay out at 24 rows — verified
by the passing run. The `fs::remove_all(root)` added to the create-failure early
return (:105) also fixes the temp-dir leak noted previously. Both prior review
items are now resolved.

Conclusion: no bugs and no weak-oracle gaps of substance in either commit. The
M9-U golden is a real end-to-end lock; the three items above are NITs.


## M10-1 (startup measurement harness)

Reviewer: gpt-5.5 (code-review agent). 3 MUST, all folded and verified.

MUST zero-cost-instrumentation (apps/ssg_main.cpp) — the macro-off no-op inline
still emitted a startup_mark symbol + calls in the shipped ssg at O0. Fixed:
preprocessor-gated call sites (STARTUP_MARK macro -> ((void)0) when
SSG_STARTUP_TRACE_ENABLED is off). Verified: nm -C build/ssg shows no
startup_mark symbol and objdump shows 0 call refs.

MUST benchmark-deadline-bypass (benchmarks/startup_benchmark.cpp) — blocking
read(master) could stall past the deadline if the child hung before writing pty
bytes. Fixed: master fd set O_NONBLOCK; drain_nonblocking + poll with the
remaining deadline; child_exited(WNOHANG) bails early on a dead child.

MUST unsound-clean-oracle (benchmarks/startup_benchmark.cpp) — --verify-clean
ignored child exit status and had no positive control, so an execl failure would
false-pass. Fixed: run_trace_probe detects exec failure (exit 127); verify_clean
now requires the instrumented probe to WRITE a trace (positive control) and both
binaries to actually exec, before concluding the clean binary's missing trace
means "compiled out".



## M10-3/M10-4

Reviewer: gpt-5.5 (code-review agent). 1 MUST, folded.

MUST snapshot-revision-invariant (src/editor_runtime.cpp prime_deferred) —
prime_deferred() mutated snapshot-visible tree/syntax state without advancing the
session revision. derive_session_delta rejects a same-revision snapshot pair and
http_server skips enqueueing when the revision is unchanged, so a delta-based
(WebSocket) client would silently miss the primed enrichment (the in-process TUI
is unaffected because it takes full snapshots). Fixed: added
EditorSession::advance_revision() (mirrors dispatch's overflow guard) as the
runtime's seam for library-internal out-of-band authoritative changes;
prime_deferred() advances the revision when deferred work actually ran. Test
strengthened to assert the revision advances on prime and not on the idempotent
second call.

