# spec-remove-leader

Status: IMPLEMENTED.

Supersedes `spec-mod-key-chords.md` for **disposition**: that spec *migrated* the
multi-stroke chord engine (folding `{Escape, X}` → `{Alt+X}` but keeping
multi-stroke sequences like `{Alt+F, KeyT}`); this spec *deletes* the engine.
The decode-layer details it worked out — `ESC`+printable → `Alt+key`, C0 →
`Ctrl+letter`, and the Shift-semantics rules — still apply and are referenced,
not restated, here.

## Goals

Remove the leader / multi-stroke chord model from SSG entirely, code included, so
it cannot rot into untested architecture debt:

- No binding is longer than a single stroke. There is no chord-pending state, no
  "press A then B" sequence, anywhere — defaults or user config.
- `Escape` is a plain key: one press cancels a prompt / closes find / clears a
  multi-selection. It is never a prefix.
- Frequent actions are single **`Alt+key`** chords (the same bytes the user
  already presses). The long tail is reached through the command palette's fuzzy
  find. Nothing that exists today becomes unreachable.
- The keymap engine, its tests, and the UI plumbing that only served multi-stroke
  chords are deleted, not left dormant.

## Design

### What "the leader" actually is (the removal surface)

The leader is not one thing; it is a mechanism spread across layers. All of it
goes:

- **Type/engine.** `KeymapMatchKind::Pending` (`include/ssg/Keymap.h:94`) and the
  `hasPending` branches in `KeymapMatcher::resolveSequence`
  (`src/Keymap.cpp:355-383`) and `CompiledKeymap::resolve`
  (`src/CompiledKeymap.cpp:51-76`). The prefix machinery `isStrictPrefix` /
  `startsWithSequence` (`src/Keymap.cpp:94-100`) and the `AmbiguousPrefix`
  validation (`src/Keymap.cpp:317-334`) exist only because a short sequence can
  prefix a longer one — impossible when every sequence is length 1.
- **App loop.** The chord accumulator in `apps/ssg_main.cpp` (the `chord.push` /
  `Pending` / "keep collecting" path ~1433-1486, and the hard-coded
  `{Escape, KeyQ}` quit literal ~1455-1457).
- **Snapshot/UI.** `leaderPending` threaded through
  `EditorRuntime::snapshot`/`sections` (`EditorRuntime.cpp:1987-1997`,
  `runtime/snapshot.cpp:113-200`, `editor_runtime_internal.h:334-343`,
  `EditorRuntime.h:176`), the `leaderHint` field
  (`include/ssg/ShellState.h:106`) and its footer rendering
  (`src/ShellState.cpp:484-494`), and the "leader:" hint string
  (`runtime/snapshot.cpp:141-147`).
- **Public config.** `keymap.bind`'s multi-stroke grammar
  (`doc/config.md:95-101`, `KeyCodec::parseSequenceString`
  `src/Keymap.cpp:207`) — space-separated sequences like `"Escape KeyF KeyQ"`.
- **Defaults.** Every `{Escape, …}` binding in `defaultTerminalKeymap`
  (`src/EditorRuntime.cpp:111-205`).

### The chosen mechanism: enforce single-stroke, don't reshape the wire

`KeySequence` stays `std::vector<KeyStroke>` (`Keymap.h:31`) — collapsing it to a
scalar would churn the protocol wire (`KeymapViewState` serialization in
`Protocol.cpp`) for no behavioral gain. Instead:

- `validate()` gains a rule: **any binding whose `sequence.size() != 1` is
  rejected** (`KeymapErrorCode::MultiStrokeBinding`, new). This turns
  "single-stroke only" into an enforced, tested invariant rather than a
  convention. The now-impossible `AmbiguousPrefix` / prefix helpers are deleted.
- `KeymapMatchKind` collapses to `{ None, Resolved }`. `resolve` never returns
  `Pending`; a stroke either resolves or does not. The app loop drops the
  accumulator entirely: one decoded stroke → one `resolve` → dispatch or fall
  through.
- `parseSequenceString` rejects input containing a space (more than one stroke);
  `parseSequence`'s `initializer_list` overload keeps working for single strokes.
- `leaderPending`/`leaderHint` and their rendering are removed outright; there is
  no mid-chord state to show.

This is the minimum that makes multi-stroke *unexpressible* while leaving the
byte-exact protocol and the single-stroke keymap API intact.

### Replacement bindings (nothing becomes unreachable)

The decode change from `spec-mod-key-chords.md` (steps 1–2 there: `ESC`+printable
→ `Alt+key` with case→shift; a lone `ESC` → `Escape`; C0 → `Ctrl+letter`) is a
prerequisite and is adopted here unchanged. Given it, every current leader
default maps to a **single** stroke:

- Frequent → `Alt+<key>`: save `Alt+S`, new `Alt+N`, undo `Alt+Z`, redo
  `Alt+Shift+Z`, file finder `Alt+P`, palette `Alt+Shift+P`, panel `Alt+B`,
  find `Alt+/`, replace `Alt+R`, select-all `Alt+A`, multi-cursor
  `Alt+D/I/K/J`, clipboard `Alt+X/C/V`, find-word `Alt+8`, delete-word
  `Alt+Backspace`.
- **tab.previous / tab.next** cannot use `Alt+[` / stay on brackets cleanly
  (`Alt+[` == CSI introducer, see the superseded spec); rebound to
  `Alt+Comma` / `Alt+Period`.
- **settings.open** was the only 3-stroke default (`{Escape, KeyF, KeyT}`) and is
  the protected escape hatch (INV-settings-hatch). It becomes a single chord,
  `Alt+Shift+T`, and the protected-binding rule keys on the command id, so the
  hatch survives the migration.
- **quit** (`{Escape, KeyQ}`, an app literal) → `Alt+Q`, the literal deleted.
- The long tail that does not earn a key is reached through the palette
  (`Alt+Shift+P`, type, Enter) — unchanged discovery path, a couple more
  keystrokes for rare actions, which the user has accepted.

`Escape` gains single-stroke bindings: `prompt.cancel` (prompt), `find.close`
(where a find/replace prompt is focused), and an editor-context abort of a
multi-selection. These are ordinary length-1 bindings, legal under the new
validation. In particular the current `{Escape, Escape}` → `prompt.cancel`
(`EditorRuntime.cpp:205`) becomes a bare `{Escape}` → `prompt.cancel` in the
prompt context — that single binding is what preserves close-on-Escape.

**The prompt-context find toggles.** `defaultTerminalKeymap` also binds four
size-2 prompt-context sequences that the frequent/tail split above did not name:
`{Escape, KeyC}`→`find.toggle_case`, `{Escape, KeyG}`→`find.toggle_whole_word`,
`{Escape, KeyE}`→`find.toggle_regex`, `{Escape, KeyL}`→`replace.all`
(`EditorRuntime.cpp:212-215`). Left in place, each fails the new
`size()!=1` rule and refuses runtime construction. Disposition: **drop their
keyboard bindings entirely** — all four are already enumerated by the palette
(dispatch is by command id, so a palette-focused prompt still runs them), and a
find-option toggle is a rare, discoverable action that does not earn a chord.
They are not rebound to `Alt+…` because that would collide with the editor-context
`Alt+C`/`Alt+E` (clipboard/…); palette-only is the clean disposition.

## Invariants

- **INV-single-stroke** (new): no binding, default or user, has a sequence length
  other than 1. Enforced by `validate()`; the default map and every `keymap.bind`
  call are checked.
- **INV-settings-hatch**: a `settings.open` binding always exists and is
  reachable; the rule keys on the command id, not the literal sequence, so the
  `Alt+Shift+T` migration preserves it.
- **INV-decode-terminates / INV-reply-never-input**: removing the chord loop must
  not touch CSI/SS3/OSC/mouse parsing; a sequence is consumed whole or not at
  all.
- **INV-browser-reachable** (`doc/spec-mod-keys.md`): the primary modifier (Alt)
  is one the browser delivers to the page; per-combo reachability
  (`Alt+D/F/←/→`) is manually verified, not suite-gated.
- **INV-one-keymap**: the same single-stroke keymap resolves in the terminal and
  browser hosts; no host-specific fork.

## Considerations

- **Escape is freed *because* the prefix is gone.** The reason a bare `Escape`
  could not simply be bound to cancel today is prefix-freeness: it prefixes every
  `{Escape, X}` chord and `validate()` rejects it
  (`KeymapErrorCode::UnreachableBinding`/`AmbiguousPrefix`). Deleting multi-stroke
  is what makes a single-`Escape` binding legal — the removal and the freeing are
  the same act, not two.
- **This removes the split-read hazard entirely.** The superseded spec worried
  about `ESC`+letter arriving in separate reads. With no chord accumulation, a
  bare `Escape` and an `Alt+letter` are each single decoded strokes; the only
  residual is the decode-layer coalescing window (owned by
  `spec-mod-key-chords.md` step 1), not a keymap concern.
- **Public API break.** `keymap.bind` no longer accepts multi-stroke sequences.
  This is a breaking change to a documented Lua surface, acceptable for a
  single-user pre-1.0 tool; `parseSequenceString` returns an error (rejecting the
  whole `keymap.bind` call, unchanged failure mode) rather than silently binding
  the first stroke. `doc/config.md` is updated and the change is noted in the
  changelog.
- **Shift semantics** (from the superseded spec): default chords use letters
  (case→shift) and unshifted digits/punctuation only — never `Alt+Shift+<digit>`
  or a shifted symbol, which are unbindable in a legacy terminal. `Alt+Shift+Z`,
  `Alt+Shift+P`, `Alt+Shift+T` are letters and legal.
- **Dead code discovery.** After removal, grep for `leader`, `Pending`,
  `parseSequence`, `isStrictPrefix` must show no orphaned references outside the
  unrelated `*Pending*` uses (LSP, scratch, watcher — those are unrelated and
  stay). The removal is complete only when those symbols are gone or single-use.
- **The palette key-hint renderer survives untouched.** `preferredBinding` +
  `formatSequence` (`runtime/snapshot.cpp:324-325`) render a command's key hint
  from its `KeySequence` for the palette. Because `KeySequence` stays a vector,
  a length-1 sequence renders as one stroke with no code change; the function is
  not dead and is not removed. Noted so a later reader does not mistake it for
  leader plumbing.

## Landing order (the plan is NOT engine-first)

The engine tolerates length-1 sequences, so the safe order regenerates the
**defaults first** while the multi-stroke engine still works, then removes the
engine in **one atomic commit**. Removing the engine first would break the app
loop (`apps/ssg_main.cpp:1449` reads `KeymapMatchKind::Pending`) and, worse,
`validate()`'s new rule would reject the still-multi-stroke default map and make
`EditorRuntime::create` refuse to construct (`EditorRuntime.cpp:1826`), killing
every runtime test. Steps 1 and 2 below are therefore a **single commit**, landed
**after** step 0.

## Risks and Mitigations

- *A removed prefix path leaves a binding unreachable.* — The property oracle
  asserts every pre-removal command reachable via a leader default is reachable
  via its single-stroke replacement or the palette.
- *`validate()`'s new single-stroke rule rejects the default map itself.* — The
  default map is regenerated to single strokes in the same change; a test asserts
  `validate({})` is empty on the shipped defaults.
- *Removing `Pending` breaks a caller that switched on it.* — The compiler finds
  every switch; `KeymapMatchKind` is an enum with exhaustive switches, so the
  build fails loudly rather than silently mishandling a case.
- *Settings hatch lost in the rename.* — Oracle asserts `settings.open` resolves
  from the published keymap after removal.

## Acceptance (Definition of Done)

- Observable: in the terminal, `Alt+S` saves, `Alt+Shift+P` opens the palette,
  `Alt+Shift+T` opens settings, a single `Escape` closes an open find prompt in
  one press; no "leader:" hint ever renders. Visual signoff on the one-press
  Escape and the absent hint.
- Budgets: n/a (removal; no new hot path).
- Gates: `bash scripts/check.sh` green (build + 93 suites, 0 warnings, -Werror).
  Suites that only exercised multi-stroke resolution are deleted or reduced, not
  left asserting removed behavior.
- Manual: INV-browser-reachable / INV-one-keymap signed off against
  Firefox/Edge/Chrome by checklist.
- Oracles: see the Plan's Oracle column.

## Plan

Depends on `spec-mod-key-chords.md` steps 1–2 (decode: `ESC`+printable → `Alt`,
C0 → `Ctrl`, lone `ESC` → `Escape`) landing first; the rest of that spec's plan
(the multi-stroke Alt-fold) is dropped in favor of the steps below. **Steps 1+2
are one atomic commit landed after step 0** (see Landing order); the others are
independently committable.

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 0 | Regenerate `defaultTerminalKeymap` to single strokes (mapping in Design): frequent→`Alt+key`; settings→`Alt+Shift+T`; tab.prev/next→`Alt+Comma`/`Alt+Period`; quit→`Alt+Q`; bare `Escape`→prompt.cancel(prompt)/find.close/editor-abort; **drop** the four `{Escape,C/G/E/L}` find toggles (palette-only). Engine still multi-stroke-capable, so this lands alone. | src/EditorRuntime.cpp, tests/runtime/test_runtime_snapshot.cpp | resolveSequence per binding; `validate({})` empty; property: every former leader command reachable by one stroke or palette | INV-settings-hatch |
| 1 | Collapse `KeymapMatchKind` to `{None, Resolved}`; delete `hasPending` branches in `resolveSequence`/`CompiledKeymap::resolve`; delete the app-loop chord accumulator and quit literal | include/ssg/Keymap.h, src/Keymap.cpp, src/CompiledKeymap.cpp, apps/ssg_main.cpp, tests/test_input.cpp, tests/test_ssg_app.cpp | app-loop: one decoded stroke → one resolve → dispatch; `Alt+S` saves, `Alt+Q` quits, bare `Escape` cancels in one press | INV-decode-terminates |
| 2 | Add `validate()` rule rejecting `sequence.size() != 1`; delete `isStrictPrefix`/`startsWithSequence` and the `AmbiguousPrefix` check; reject multi-stroke (space) in `parseSequenceString` | include/ssg/Keymap.h, src/Keymap.cpp, tests/test_input.cpp | property: a 2-stroke binding rejected; `"Escape KeyS"`→error, `"Alt+KeyS"`→ok; 1-stroke map validates empty | INV-single-stroke |
| 3 | Remove `leaderPending` params and `leaderHint` field + footer rendering + "leader:" string | EditorRuntime.{h,cpp}, runtime/snapshot.cpp, editor_runtime_internal.h, ShellState.{h,cpp}, tests/test_ui_layout.cpp | grep: no `leaderHint`/`leaderPending` symbol remains; UI test has no leader node | INV-one-keymap |
| 4 | Docs + dead-code sweep: config.md (single-stroke grammar, no sequences), spec-mod-keys.md (leader removed), mark spec-mod-key-chords.md superseded, changelog break note; regenerate commands.md | doc/*, tests/test_commands.cpp | test_commands reference current; grep sweep clean | - |

## Rationale (optional, skippable)

The leader bought two things: a way to reach many commands from one always-safe
key, and browser/terminal portability. The palette already provides the first
(fuzzy find over the full command surface), and single `Alt+key` chords — bytes
the user already presses and which survive browser chrome — provide direct access
to the frequent handful. So the leader's job is fully covered without it, and
keeping the multi-stroke engine only to service a feature nobody invokes is the
textbook definition of the debt this change removes: code with tests, invariants,
and UI plumbing, but no live purpose. Removing it also dissolves a class of
problems the migration spec had to manage (Escape-vs-Alt split reads, prefix
ambiguity, the two-Escape cancel), because those all stemmed from Escape being a
prefix. The one real cost — a breaking change to `keymap.bind`'s multi-stroke
grammar — is acceptable for a personal, pre-1.0, single-user tool and is the
point, not a regret.
