# spec-terminal-escape-discipline

Status: done

## Goals

Make it structurally impossible for SSG to enter a terminal mode and not leave
it, and impossible for unmeasured text to reach the terminal.

Today both are possible, and both have live instances. After this work every
mode is entered by constructing an object and left by destroying it; the exact
bytes that undo everything currently entered are available at any instant,
including from a signal handler; and text can only become a cell after it has
been classified as safe to emit.

## Current state (verified against the running binary)

SSG emits two kinds of escape sequence, and the distinction is the whole spec:

- **Modes** change how the terminal behaves until explicitly undone. Entering one
  incurs a debt.
- **Painting** — cursor positioning (`ESC [ r;c H`) and SGR colour
  (`ESC [ 38;2;...m`, `ESC [ 0m`) — takes effect where it lands and owes
  nothing. It is re-issued every frame and needs no pairing.

### The modes, and where each is paired

| Mode | Enter | Leave | Scope | Paired? |
|---|---|---|---|---|
| Alternate screen | `?1049h` | `?1049l` | process | yes |
| Mouse press/release | `?1000h` | `?1000l` | process | yes |
| Mouse motion | `?1002h` | `?1002l` | process | yes |
| SGR mouse coords | `?1006h` | `?1006l` | process | yes |
| Cursor style | `5 q` | `0 q` | process | yes |
| Cursor visibility | `?25l` | `?25h` | **frame** | **no — see below** |
| termios raw mode | `tcsetattr` | `tcsetattr` | process | yes |

The process-scoped modes live in two hand-written string literals,
`terminal_setup_sequence()` and `terminal_restore_sequence()`
(`apps/ssg_terminal.cpp:30,37`), which must mirror each other by inspection.
They currently do. That is a property of care, not of construction — and the
care is real: `?1002h`/`?1002l` were added in the same commit (`5fb57fd`).

### Live defect 1 — the cursor is hidden per frame and shown conditionally

The frame loop emits `ESC [ ?25l` unconditionally at the top of every frame, and
`ESC [ ?25h` **only if `grid.caret` has a value** (`apps/ssg_main.cpp`). A frame
with no caret therefore hides the cursor and never shows it again — the debt is
settled only by `terminal_restore_sequence()` at process exit, if that runs.

Reachable and measured: shrinking the window below the 20x4 minimum renders the
"too small" placeholder, which sets no caret. After that frame the last cursor
directive on the wire is `?25l`. The cursor stays hidden.

This is the exact shape of the concern: a mode entered in one scope and left in
another, where a branch decides whether the debt is paid.

### Live defect 2 — the mode debt is not paid on most exits

From `files/terminal-mode-findings.md` (measured): only SIGTERM/SIGHUP restore.
SIGINT, SIGTSTP and SIGSEGV each leave the alternate screen active and mouse
reporting on. Combined with defect 1, a crash can additionally leave the user's
cursor invisible.

### Live defect 3 — one input path can emit modes SSG never chose

From `files/render-escape-injection-findings.md` (measured): document text and
filenames are safe, because `GraphemeLayout` classifies C0/C1/DEL as `Control`
and the renderer substitutes a replacement glyph. **`style.define` glyphs bypass
that entirely** — `scrollbar_track = "\x1b(0"` reaches the terminal and switches
the character set. Multi-character and double-width glyphs are also accepted
silently, breaking the one-column cell contract.

### Why these are one problem

Nothing owns the answer to "which modes are currently set." The setup string,
the restore string, and the frame loop are three independent writers of terminal
mode, and the cell encoder is a fourth path by which mode-setting bytes can
reach the terminal. Each defect is a different way for those writers to
disagree.

## Design

**One owner: a mode stack that knows what is entered and can undo it.**

### A mode is a pair, declared once

A `TerminalMode` value carries its enter bytes and its leave bytes together, so
they cannot be added in one place and forgotten in another. The named modes
(alternate screen, the three mouse modes, cursor style, cursor visibility) are
constants of this type. The two hand-mirrored string literals are deleted; both
sequences are derived from the same declarations.

### Entering is scoped, and leaving is a destructor

`TerminalModes` owns an ordered stack of entered modes and writes bytes to the
terminal. Entering returns a movable RAII guard whose destructor pops. Modes
leave in reverse order of entry, which is the only order that composes.

Process-scoped modes are guards held in `main`. The per-frame cursor hide
becomes a guard whose lifetime is the frame — so defect 1 is not fixed by
adding the missing `?25h`, it is fixed by making the emission of `?25l` and
`?25h` the same statement. A branch cannot skip half of a destructor.

Mechanism chosen: **a stack with RAII guards** over a flags-and-reconcile model
(compute the desired mode set each frame and emit the difference). Reconcile is
more general and is what a long-lived client would want; it is rejected here
because it re-creates the failure this spec exists to remove — a mode is set by
one place and cleared by another, with a data structure in between.

### The undo string is maintained eagerly, not built on demand

A crash handler cannot run destructors, and cannot allocate. `TerminalModes`
therefore keeps a preallocated byte buffer holding the bytes that undo
everything currently entered. A signal handler needs only
`write(STDOUT_FILENO, bytes, length)`, which is async-signal-safe.

**Rebuilding that buffer in place would be a race**: a signal can arrive while
push or pop is halfway through rewriting it, and the handler would emit torn
bytes. Two rules make the buffer safe to read at any instant, and both are
required.

*Atomic publication.* There are two preallocated buffers, each holding its own
bytes and length, and one `std::atomic<Buffer const*> published_`. A rebuild
fills the buffer that is **not** published, then stores the pointer with release
ordering; a handler loads with acquire and writes. Bytes and length are
published together, so a handler can never pair one buffer's length with the
other's bytes. Publishing a length separately from the bytes would reintroduce
the tear.

*The buffer is always a SUPERSET of what is entered.* A signal arriving mid-push
would otherwise find a buffer that does not yet undo the mode whose enter bytes
were already written. So the order is: on enter, **publish the larger undo
first, then write the enter bytes**; on leave, **write the leave bytes first,
then publish the smaller undo**. At every instant the published buffer undoes at
least every mode actually set, and possibly one that is not.

That over-approximation is harmless because a mode-leave sequence is a no-op
when the mode is not set: leaving an alternate screen you are not on, or
disabling mouse reporting that is already off, changes nothing. Erring toward a
superset is therefore the safe direction, which is why the ordering is this way
round and not the reverse.

This is the property that makes the strategy watertight rather than merely
tidy: the correct undo bytes exist as data at every instant, so every exit path
— normal return, exception, terminating signal, and later suspend — uses the
same source of truth instead of its own copy.

`tcsetattr` is not async-signal-safe and stays where it is, in normal context.
The escape-sequence half is what a crash handler can and must do.

### Nothing reaches the terminal that has not been measured

The cell contract — *cell text is safe to emit and occupies exactly its declared
width* — is currently established by `GraphemeLayout` on the document path and
by nothing on the style path. `style.define` must validate each glyph through
the same measurement and reject with a diagnostic naming the offending key,
consistent with how it already rejects unknown keys and bad dimensions.

Validating `style.define` fixes the known instance but does not close the class:
a future writer of cell text has the same hole available, and an audit listing
today's writers goes stale silently. So step 6 makes cell text settable only
through a seam accepting classified text, so bypassing measurement fails to
compile — the same shape as the deferred-queue single-writer fix. The check
comes first, because it is what makes the type change safe to attempt.

### Ownership and layering

All of this is terminal-client concern and lives in `ssg::app`
(`apps/ssg_terminal.{h,cpp}`), which already owns
`terminal_setup_sequence`/`terminal_restore_sequence` and is unit-tested through
`tests/test_ssg_app.cpp`. Nothing terminal-shaped enters the library (**I1**,
**I12**). The `style.define` validation is a library change, because that is
where the command and `GraphemeLayout` both live.

## Invariants

- **INV-mode-paired** (new): every mode SSG enters is entered by constructing a
  guard and left by destroying it. No mode's enter bytes appear in the codebase
  outside its declaration.
- **INV-mode-undo-ready** (new): at any instant, the bytes that undo every
  entered mode exist as a preallocated, atomically published buffer, readable
  and writable by an async-signal-safe call, and always a superset of the modes
  actually set.
- **INV-cell-safe** (new): text becomes a cell only after classification that
  guarantees it emits no mode change and occupies its declared width.
- **INV-app-io-only** (`doc/spec-terminal-robustness.md`): the app contains no
  editor, layout, or colour decision logic. This spec adds terminal-state
  ownership to the app, which is I/O, and adds no editor logic.
- **I7 — Grid-owned UI** and **I15 — Shell geometry**: a glyph wider than its
  cell corrupts the geometry the server describes; validation restores it.
- **I22 — Colour authority**: unchanged; SGR painting continues to derive from
  the theme.

## Considerations

- **Painting must not be swept into the stack.** Cursor positioning and SGR are
  re-issued every frame and owe nothing. Modelling them as modes would put a
  push/pop on the hot path for no benefit. The spec's value comes from the
  distinction, so the distinction must be stated in the code, not just here.
- **`?25h` in the current restore string is settling a frame-scope debt at
  process scope.** Once the frame guard exists, the restore sequence should stop
  mentioning cursor visibility, or the two will disagree about who owns it.
- **Reverse order matters.** Leaving the alternate screen before disabling mouse
  reporting leaves reporting on in the primary screen. The stack must pop in
  reverse and a test must pin the order, since the current literal is correct by
  hand.
- **The frame guard must not cost a write.** Hiding and showing the cursor is
  already two writes per frame inside one buffer; the guard must append to the
  same frame string rather than issue its own syscalls.
- **Suspend/resume is enabled by this, not done by it.** A mode stack that can
  emit "leave everything" and "re-enter everything" is exactly what SIGTSTP and
  SIGCONT need (`files/terminal-mode-findings.md`). Deliberately out of scope
  here; the design must not preclude it, and the re-enter direction should exist
  for that reason even though nothing calls it yet.
- **A guard must be movable but not copyable**, or two objects pay one debt.
- **Style glyph validation is a behaviour change for existing configs.** A
  config using a two-column glyph starts being rejected. That is correct — it
  was silently corrupting the frame — but it must be a named diagnostic, not a
  silent substitution.

## Risks and Mitigations

- **Risk: the frame guard changes the hot path.** Every frame builds a string;
  a mistake here shows as flicker or a lost cursor.
  *Mitigation*: the guard appends to the frame buffer, and the pty oracle below
  asserts the cursor's final state per frame rather than inspecting code.
- **Risk: an eagerly maintained undo buffer drifts from the stack.**
  *Mitigation*: it is rebuilt inside push and pop, both of which are private to
  the owner, and an oracle compares it against an independently derived
  expectation after random push/pop sequences.
- **Risk: reverse-order teardown regresses**, since the current order is
  correct by hand and a stack could plausibly emit either order.
  *Mitigation*: pin the exact byte sequence against today's literal, which is
  known good.
- **Risk: scope creep into suspend/resume and crash handling.** Both are enabled
  by this and neither is required for it.
  *Mitigation*: the plan stops at making the undo bytes available; wiring new
  signals is a separate change.

## Acceptance (Definition of Done)

- Observable: the editor looks and behaves exactly as before — same startup,
  same rendering, same clean exit — verified against the real binary over a pty,
  plus the cursor now surviving a below-minimum resize.
- Budgets: no additional syscalls per frame; no allocation in the crash path.
- Gates: `bash scripts/check.sh` green — 0 warnings, all tests passing.
- Oracles:
  - byte-for-byte parity with today's known-good sequences ->
    `modeStackReproducesTheCuratedSetupAndRestoreSequences`
  - pairing by construction -> `everyEnteredModeIsLeftInReverseOrder`
  - undo buffer correctness -> `theUndoBufferIsAlwaysASupersetOfWhatIsEntered`
  - no torn read -> `theUndoBufferIsNeverObservedMidRebuild` (a thread pushing
    and popping while another samples the published buffer; every sample must be
    a valid, coherent undo string)
  - the live cursor defect -> `aFrameWithNoCaretLeavesTheCursorVisible`
  - injection -> `aStyleGlyphThatEmitsAModeIsRejectedNamingItsKey`
  - width -> `aStyleGlyphWiderThanOneCellIsRejectedNamingItsKey`

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the parity oracle against today's literals, before any refactor: the sequences are known good, so they are the reference the new mechanism must reproduce byte for byte. | `tests/test_ssg_app.cpp` | `modeStackReproducesTheCuratedSetupAndRestoreSequences` (fails until step 2) | - |
| 2 | Add `TerminalMode` (enter+leave declared together) and `TerminalModes` (ordered stack, RAII guards, reverse-order pop, eagerly maintained undo buffer). Declare the six named modes. | `apps/ssg_terminal.h`, `apps/ssg_terminal.cpp` | step 1; `everyEnteredModeIsLeftInReverseOrder`; `theUndoBufferIsAlwaysASupersetOfWhatIsEntered` (random push/pop vs independent expectation); `theUndoBufferIsNeverObservedMidRebuild` (sampler thread, sanitiser build) | INV-mode-paired, INV-mode-undo-ready |
| 3 | Replace the literals in `main` with guards; delete `terminal_setup_sequence`/`terminal_restore_sequence`. **Reproduces today's bytes exactly, cursor visibility included** — this step changes structure only, so step 1's oracle passes unmodified and proves it. | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}` | step 1 unmodified; pty run: unchanged startup and clean exit | INV-mode-paired |
| 4 | Move cursor visibility from process scope to a frame-scoped guard appending to the frame buffer, so hide and show are one statement, and drop the teardown `?25h`. **This is the only step that changes the wire output**, so it is also the only step that edits step 1's expected bytes — a one-line reference change whose diff is the point of the step. | `apps/ssg_main.cpp`, `tests/test_ssg_app.cpp` | `aFrameWithNoCaretLeavesTheCursorVisible` (pty: resize below 20x4, assert the last cursor directive is `?25h`); step 1 with its reference updated in this commit and nowhere else | INV-mode-paired |
| 5 | Validate `style.define` glyphs through `GraphemeLayout`: reject anything emitting a mode or occupying other than one cell, with a diagnostic naming the key. | `src/Style.cpp`, `include/ssg/Style.h`, `tests/test_style.cpp` | `aStyleGlyphThatEmitsAModeIsRejectedNamingItsKey`; `aStyleGlyphWiderThanOneCellIsRejectedNamingItsKey`; pty re-run of the injection probe | INV-cell-safe, I7, I15 |
| 6 | Close the class rather than the instances: make cell text settable only through a seam taking classified text, so an unmeasured string cannot become a cell. Audit the remaining `Style` string fields as its first consumers. | `include/ssg/Renderer.h`, `src/Renderer.cpp`, `include/ssg/Style.h` | assigning a raw `std::string` to cell text no longer compiles; the injection probe finds no route | INV-cell-safe |
| 7 | Document the mode/painting distinction and the three new invariants. | `doc/spec.md`, `doc/spec-terminal-escape-discipline.md`, `doc/config.md` (glyph rules) | doc gates green | - |

## Out of scope

- **Suspend/resume (SIGTSTP/SIGCONT)** and **crash restore (SIGSEGV/SIGINT)**.
  This spec makes both straightforward by providing the undo bytes; wiring the
  signals is a separate change (`files/terminal-mode-findings.md`).
- **Re-enterable terminal mode for shelling out.** Same reason.
- **The browser client's rendering**, which emits no escape sequences.

## Rationale (optional, skippable)

The prompt for this spec was the worry that a mode had once been entered and not
left, and that such a bug should be impossible with a constructor and a
destructor. The history is better than feared — `?1002h` and `?1002l` were added
in one commit, and the two literals do mirror each other today. But the worry is
correct in the general case, and there is a live instance: the per-frame cursor
hide is settled by a conditional, and a below-minimum resize leaves the cursor
hidden.

What makes that instance instructive is that it is not a missing line. Adding
the missing `?25h` would fix the symptom and leave the shape intact: an enter in
one place, a leave in another, a branch in between. The fix is to make the two
emissions the same statement, which is what a guard is for.

The eagerly maintained undo buffer is the part that turns tidiness into a
guarantee. Destructors handle the paths that unwind; a crash is precisely the
path that does not. Keeping the undo bytes as data means the ugly path and the
clean path agree by construction rather than by a second hand-maintained string
— which is the same failure, one level up, as the two literals this spec
deletes.

## Status

Implemented in `73f6420`, `7ed37e3`, `2a9c82b`, `b88c789`, `1d38efa` and this
commit. Gate green throughout (93 tests, 0 warnings).

Two design changes the work itself forced:

**The eagerly published undo buffer was replaced by a constant.** Its own
torn-read oracle showed that double buffering does not survive a writer lapping
a reader, and a signal is not always delivered to the thread that is mid-update.
No lock-free publication of a variable-length buffer fixes that without a
retry the handler cannot afford. But the superset argument the spec already
made says precision is not needed: leaving a mode that is not set does nothing.
So the crash undo is a compile-time constant covering every declared mode in
reverse declaration order — no synchronisation, no allocation, trivially
async-signal-safe, and strictly safer than the design it replaces.

**Frame assembly moved into `ssg::app`.** The cursor-balance oracle was
specified as a pty test because frame assembly lived in the loop. Extracting
`encode_frame` — which the app header already says is where testable code
belongs — turned it into a unit test, and the pty run became a confirmation
rather than the oracle.

Step 6 landed as a scanner rather than a type. `put` is already the only writer
of cell text, and the glyph table is already the only route by which user text
reaches a Style field, so the reachable hole was narrower than the spec assumed:
a new `Style` string field is either in the table and therefore validated, or
unreachable from `style.define`. The scanner asserts exactly that, and that
every default satisfies its own rule. A cell-text type remains the stronger
form and is not needed while `put` holds.
