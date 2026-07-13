# Spec: keymap contexts and command metadata (M6)

## Goal

Make the published keymap the source of truth for "which key does what, where,"
so every client resolves keys the same way, and give the palette human labels
plus each command's bound key sequence. This is the library half of
`doc/spec-navigation.md` step C (context resolution) plus the command-metadata
source the palette needs (`doc/spec-palette.md` §Considerations: `label == id`
and empty `detail` are deferred to M6).

Keyboard reachability model (the universal editor model, VSCode-style):
**every command that can be invoked without a caller-supplied argument is
reachable through the palette** (fuzzy over the full command list, already
built — a selected candidate either dispatches argument-free or opens its own
argument prompt, e.g. `file.open` prompts for a path), and **common,
argument-free commands additionally get a curated key chord**. Milestone M6's
"every action reachable by a two-key chord and the palette" is satisfied by this
pair. Purely programmatic, argument-required commands (`settings.set`,
`cursor.set_position`, `text.insert`, scroll amounts, `tab.activate` by index)
are invoked by their producers (text routing, mouse, other commands), not by a
bare chord or a bare palette execution; they are deliberately **not** chord or
palette targets.

Two concrete unblocks:

1. **Navigation step 2 — context resolution.** A client resolves a keystroke
   against the published keymap using the snapshot's `FocusTarget` as the
   context, so the same physical key maps to different commands per focus
   (`ArrowDown` is `cursor.line_down` in `editor`, `tree.select_next` in
   `panel`). Today the TUI has an ad-hoc `panel`/`editor` branch and a
   hard-coded ESC-chord table.
2. **Palette labels and key-sequence detail.** Palette candidates currently
   publish `label == id` and empty `detail`. With command metadata (human
   label) and the runtime keymap (bound sequence), a candidate for `file.save`
   reads `Save File` with detail `Esc S` instead of `file.save`.

## Background: what exists today

- `include/ssg/input.h` has `KeyBinding {sequence, command_id, context}`,
  `KeymapViewState {name, bindings}`, and `validate_keymap` (checks non-empty
  fields, invalid strokes, duplicate bindings, `*`-shadow `unreachable_binding`,
  and browser-`reserved_binding` prefixes). It does **not** check that `context`
  is a known name, does **not** reject cross-binding strict-prefix ambiguity,
  and there is **no** resolution function.
- The `data/default-keymap.json` machine-encoding contract, `doc/features/
  browser-input.md`, and the `test_input.cpp` generated-coverage tests were a
  **dormant browser-input vestige** (a `prefix` `Ctrl+Shift+KeyM` + `alphabet`
  generating a chord per command, for the removed browser client). They have been
  **retired**: that encoding was browser-consumed and not terminal-decodable, a
  bare generated chord cannot carry a typed argument, and the contract was
  internally inconsistent (`browser-input.md` claimed the reserved fixture
  included `Ctrl+Shift+KeyM` while the fixture omitted it). The normative input
  model it once held — client-local leader, prefix-free contexts, Escape has no
  implicit cancel, the `settings.open` escape hatch — now lives in this spec and
  `doc/spec-navigation.md`. `keymap:true` in `required-commands.json` is read as
  **binding eligibility**, not a requirement that the runtime keymap bind the
  command. The terminal has no browser-reserved chords, so the runtime keymap is
  validated with an **empty** reserved set (the generic `reserved_binding` check
  remains for callers that supply one).
- `settings.open`'s handler currently builds a `PromptKind::settings`
  `PromptRequest` with **zero** inputs, which `valid_request` rejects (it requires
  one), and the error is discarded — so `settings.open` is a silent no-op today.
  K2 fixes this (a settings prompt with one input) so the I24 escape-hatch chord
  actually opens a focused prompt; otherwise binding it is pointless.
- The runtime keymap is empty: `EditorRuntime::Impl::keymap{"default", {}}`. It
  is published on the snapshot (`SessionSnapshotSections::keymap`) with a wire
  codec, but carries no bindings, so no client can resolve against it.
- `apps/ssg_main.cpp` hard-codes ESC-chords (`ESC Q/b/s/z/Z/]/p/w/P`) and
  branches arrows/Enter/text on `focus == editor|panel`. `parse_input` yields an
  `InputAction` (+ a `char key` for chords), not `KeyStroke`s.
- There is no command-label source anywhere. `EditorRuntime::Impl::descriptors()`
  sets `SearchCommandDescriptor.label = id`; `p0_command_descriptors()` carries
  only `{id, effect, required_capabilities}`.
- Leader presentation is already wired (`doc/spec-navigation.md` A1/A2): the
  client reports a pending `KeySequence`, the library renders the hint. The TUI
  derives that pending sequence ad hoc in `ssg::app::pending_leader`.

## Design

### Ownership

- **Library owns**: the canonical context set, keymap validation (context-name
  validity, cross-binding prefix-freeness, the `settings.open` escape-hatch
  lock), the curated runtime keymap (compiled-in bindings with contexts), the
  pure resolution function, and the command-metadata table (id → human label)
  and key-sequence display formatter. All are compiled into the library — no
  runtime dependency on repository-relative data files.
- **Client owns**: decoding platform bytes into `KeyStroke`s and committed text,
  holding the transient pending sequence, calling the resolver with the
  snapshot's focus as context, dispatching the resolved command, applying the
  context text handling, and (in `prompt` focus) fulfilling the resolved prompt
  command against the active prompt's client-local view (below). The client
  contains no binding table and no routing rule beyond "context = snapshot
  focus," the text-handling rule, and the bounded prompt-fulfillment rule.

### The canonical context set

A binding's `context` is valid iff it is `*` (global) or the lowercase name of a
`FocusTarget`: `editor`, `panel`, `prompt`. The set is defined once in the
library (`keymap_contexts()`, derived from the `FocusTarget` names plus `*`) so
it cannot drift from the enum. `validate_keymap` emits a new
`KeymapErrorCode::unknown_context` for any other context. This is distinct from
`unreachable_binding` (a `*` binding shadowing a same-sequence context binding),
whose meaning is unchanged.

### Resolution (pure, client-called)

Add pure functions:

```
enum class KeymapMatchKind { none, pending, resolved };
struct KeymapResolution {
    KeymapMatchKind kind;
    std::string command_id;   // set iff kind == resolved
};
KeymapResolution resolve_key_sequence(
    const KeymapViewState& keymap,
    const KeySequence& pending,
    std::string_view context);        // a FocusTarget name

enum class TextRouting { insert, prompt_query, ignore };
TextRouting text_routing(std::string_view context);
```

Resolution semantics (bindings are prefix-free within a resolvable context, so
no timeout is needed; `doc/spec-navigation.md` owns the leader/focus model):

- A binding is **eligible** in `context` iff its context is `*` or equals
  `context`.
- **resolved**: some eligible binding's `sequence` equals `pending`. For one
  sequence a `*` binding takes precedence over a focus binding, so the
  `settings.open` escape hatch (I24) and focus-change chords are never shadowed
  (`doc/spec-navigation.md` N3). Prefix-freeness (validated, below) guarantees at
  most one command resolves.
- **pending**: no eligible binding equals `pending`, but some eligible binding's
  sequence has `pending` as a strict prefix (the chord is mid-entry).
- **none**: neither; the client clears `pending`. A non-matching continuation is
  not suppressed: the client re-processes the offending keystroke through normal
  text/control handling (e.g. `Escape` then an unbound printable clears leader
  and the printable still routes as text; `doc/spec-navigation.md` §The Escape
  leader and cancel).

**Committed text is not a command binding.** `text_routing(context)` tells the
client where committed text (with no pending chord) goes: `insert` in `editor`
(the client dispatches `text.insert` with the typed argument — the only place a
typed payload is produced), `prompt_query` in `prompt`, and `ignore` in `panel`.
`prompt_query` is fulfilled by the client per active prompt kind (below): for the
**palette** it edits the client-local query; other prompt kinds are out of scope
this milestone (only the palette is TUI-reachable today). This keeps every keymap
binding a bare, argument-free command id while typed input flows through a
separate, explicit routing enum rather than a fictitious command. Resolution and
`text_routing` are pure and never round-trip.

Prompt **editing control keys** (Backspace) are likewise client-local for the
palette, not keymap bindings: when the resolver returns `none`/`pending`-cleared
in `prompt` focus, the client applies the editing key to the active palette
query (backspace deletes a code point, exactly as the palette does today). This
keeps prompt query editing — insert and delete — entirely client-local
(`spec-palette.md` P2), with no invented command ids.

### Prefix-free validation

`validate_keymap` gains a `KeymapErrorCode::ambiguous_prefix` check: within every
resolvable context (each `FocusTarget` context unioned with `*`), no binding's
`sequence` may be a strict prefix of another eligible binding's `sequence`. The
check is order-independent (it does not depend on binding order) and covers
`*`/`*`, focus/focus, and `*`/focus pairs. This makes `resolved` and `pending`
mutually exclusive for any input, so `resolve_key_sequence` is unambiguous
without a timeout.

### The curated runtime keymap

The runtime keymap is a **small, compiled-in, hand-authored** table of bindings
for the argument-free commands the TUI drives, with the context-divergent
navigation keys that make context resolution observable. It is authored in C++
(no data-file dependency) and loaded into `Impl::keymap`.

Only **argument-free** commands are bound: a bare chord dispatches a command id
with no payload, so commands that require a typed argument (`text.insert`,
`cursor.set_position`, scroll amounts, `tab.activate` by index, `settings.set`,
…) are **not** chord targets — they are reached via text routing, mouse, or a
prompt. Exhaustive command reachability is the palette's job, not the keymap's
(see Considerations).

Bindings (all terminal-typable; Escape-led chords and single strokes only; the
terminal has no browser-reserved chords, so none is excluded):

- Global (`*`): `[Escape, KeyS]` → `file.save`, `[Escape, KeyZ]` → `edit.undo`,
  `[Escape, Shift+KeyZ]` → `edit.redo`, `[Escape, KeyP]` → `palette.open`,
  `[Escape, KeyB]` → `panel.toggle`, `[Escape, KeyO]` → `panel.focus`,
  `[Escape, BracketRight]` → `tab.next`, `[Escape, BracketLeft]` →
  `tab.previous`, `[Escape, KeyW]` → `tab.close`, `[Escape, KeyF, KeyT]` →
  `settings.open` (the I24 escape hatch).
- `editor`: `ArrowDown` → `cursor.line_down`, `ArrowUp` → `cursor.line_up`,
  `ArrowLeft` → `cursor.left`, `ArrowRight` → `cursor.right`, `Enter` →
  `text.newline`, `Backspace` → `text.delete_backward`.
- `panel`: `ArrowDown` → `tree.select_next`, `ArrowUp` → `tree.select_previous`,
  `Enter` → `tree.activate`.
- `prompt`: `Enter` → `prompt.submit`, `[Escape, Escape]` → `prompt.cancel`,
  `ArrowDown` → `palette.next`, `ArrowUp` → `palette.previous`. (Prompt query
  editing — printable insert and backspace — is client-local text handling via
  `text_routing == prompt_query`, not a binding.)

This overlay is prefix-free within each resolvable context (each context's
bindings unioned with `*`): all `*` chords share the `[Escape]` prefix but no one
is a strict prefix of another (`[Escape, KeyF, KeyT]` has no `[Escape, KeyF]`
sibling); single-stroke arrows/Enter/Backspace are never a prefix of an
Escape-led chord; and `[Escape, Escape]` in `prompt` is neither a prefix of nor
prefixed by any `*` chord. `ArrowDown`/`ArrowUp`/`Enter` appear in multiple
contexts with different commands, which is allowed (different `context`; not a
duplicate).

The assembled keymap is validated with `validate_keymap` (with an **empty**
reserved-sequence set — the terminal has no browser reservations) at runtime
creation, and the `settings.open` escape hatch is asserted; a
validation failure fails `EditorRuntime::create` with a message (the keymap is a
build-time invariant, not user input yet). For the I24 chord to be operable, K2
also fixes `settings.open`'s handler to open a valid `PromptKind::settings`
prompt (one input) instead of the current rejected zero-input request, so the
chord produces `prompt` focus with a visible input rather than a silent no-op.

### The `settings.open` escape hatch (I24)

Add `has_global_binding(keymap, command_id, reserved_sequences)`: true iff some
`*`-context binding names `command_id`, is not browser-reserved (checked against
`reserved_sequences`), and is not `*`-shadowed by an earlier `*` binding of the
same sequence. Runtime assembly runs `validate_keymap` first, then asserts
`has_global_binding(keymap, "settings.open", reserved)`; a test rejects a keymap
that removes, contextualizes, reserves, or `*`-shadows the last valid
`settings.open` binding. Dynamic rebinding (a future keymap-editing surface) will
reuse this check; there is no rebinding path in this milestone, so the
enforcement is a static assembly-time invariant.

### Prompt-focus fulfillment

Some resolved `prompt`-context commands need a client-derived argument because
the prompt's query and selection are client-local by design (`spec-palette.md`
P2/P3). This is not general routing; it is the same bounded prompt-view ownership
the palette already has. The rule, applied by the client only in `prompt` focus
and keyed on the active prompt kind:

- `prompt.submit` for a **palette** prompt is fulfilled as `palette.execute` with
  the client's selected candidate id (the existing server-validated path,
  `spec-palette.md` §4b); for any other prompt it is dispatched as
  `prompt.submit`.
- `prompt.cancel` for a palette prompt is fulfilled as `palette.close`; otherwise
  `prompt.cancel`.
- `palette.next`/`palette.previous` move the client-local selection (no dispatch
  needed for the palette; the client updates its reported view).
- Committed text and Backspace edit the client-local palette query (client-local,
  not commands).

Non-palette prompts (path/find/replace/settings/command-argument) store
authoritative values in server-owned `PromptSurface` inputs; their text-input
editing is **out of scope for this milestone** — the TUI does not yet drive text
into them (only the palette is interactively opened). When they become
TUI-reachable, their text will route to a typed server prompt-input command, not
the client-local query path; this spec's `prompt_query` fulfillment is defined
only for the palette.

The keymap stays generic (it binds `prompt.submit`, not `palette.execute`); the
client fulfills it against the active prompt, so no palette-specific binding or
server round-trip on typing is introduced.

### Command metadata (labels) and palette detail

Add a compiled-in library command-metadata table `command_label(std::string_view
id) -> std::string` (falling back to the id if absent). It is the future home of
richer metadata (category, description). Every command in the published palette
candidate set MUST have a label entry; a test asserts full coverage of the
palette-reachable set so the table cannot silently drift from the registry.

Add `format_key_sequence(const KeySequence&) -> std::string`, a compact human
display form (`Escape`→`Esc`, `KeyS`→`S`, `ArrowDown`→`Down`, joined by spaces),
so `[Escape, KeyS]` renders as `Esc S`.

`EditorRuntime::Impl::palette_view()` builds each `PaletteCandidate` with:
- `id` = command id (unchanged),
- `label` = `command_label(id)`,
- `detail` = `format_key_sequence` of the command's **preferred** bound sequence
  in the runtime keymap, or empty if the command is unbound.

Because the curated keymap contains only terminal-typable ergonomic bindings,
`detail` is always a friendly chord or empty (there are no generated
`Ctrl+Shift+KeyM` chords in the runtime keymap). If a command has multiple
bindings, the preferred one is chosen **deterministically**, independent of
binding order: prefer the shortest `sequence`, then the lexicographically least
`format_key_sequence` form. Detail is derived from the live keymap each snapshot,
so rebinding updates it automatically. Server stays authoritative for label and
detail; the client still fuzzy-ranks locally (`spec-palette.md` P2/P5).

## Invariants

- K1 (context validity): every binding's `context` is `*`, `editor`, `panel`, or
  `prompt`; `validate_keymap` rejects any other with `unknown_context`. The set
  is derived from `FocusTarget`, so it cannot drift.
- K2 (prefix-free contexts): within every resolvable context (its bindings ∪
  `*`), no binding's sequence is a strict prefix of another's; `validate_keymap`
  rejects violations with `ambiguous_prefix`, order-independently. Hence
  `resolved` and `pending` are mutually exclusive.
- K3 (pure client-side resolution): `resolve_key_sequence`/`text_routing` are
  pure functions of (published keymap, pending sequence, focus context); they
  never mutate state or round-trip, and typing/dispatch stay client-local
  (`doc/spec-navigation.md`, `spec.md` I17 carve-out).
- K4 (`*`-precedence): for one sequence, a `*` binding resolves in every context
  and takes precedence over a focus binding (`spec-navigation.md` N3).
- K5 (bindings are argument-free-usable): every runtime keymap binding names a
  command that dispatches successfully with an **empty payload** — i.e. it never
  fails for a missing or mistyped caller argument. (The command-argument codec
  registry is not the arity source: many argument-*optional* commands such as
  `cursor.line_down` and `text.newline` carry a typed codec yet dispatch fine
  with no payload; only commands that *require* an argument — `text.insert`,
  `cursor.set_position`, `settings.set`, `view.scroll_lines` — reject an empty
  payload.) Argument-required commands are reached via text routing / mouse /
  prompts, not chords. Exhaustive command reachability is the palette, not the
  keymap.
- K6 (escape-hatch lock, I24): the runtime keymap always has a valid, unreserved,
  unshadowed `*` binding for `settings.open`; assembly rejects a keymap without
  one.
- K7 (server-authoritative metadata): command label and the palette `detail`
  key-sequence are produced by the library from the metadata table and the
  runtime keymap; the client renders them and invents neither (`spec-palette.md`
  P1/P3).

## Considerations

- **Coverage is the palette's job, not the keymap's.** An earlier draft made the
  runtime keymap exhaustive by reusing the browser encoding
  (`Ctrl+Shift+KeyM`+letters). That is wrong for a terminal: the prefix is
  browser-reserved and not terminal-decodable, and a bare chord cannot supply the
  typed argument many commands require, so "coverage" there proves resolution,
  not operability. The correct model (and the universal editor model) is: the
  palette provides reachability over every command invocable without a
  caller-supplied argument (a candidate dispatches argument-free or opens its own
  argument prompt), and the keymap provides curated chords for common
  argument-free commands. Purely programmatic argument commands are neither chord
  nor palette targets. This satisfies milestone M6's "reachable by chord and the
  palette."
- **Catalog semantics.** `keymap:true` in `required-commands.json` denotes a
  command that is *eligible* to be bound; it does not require the terminal
  runtime keymap to bind it. The former "the default keymap covers every command"
  clause belonged to the retired browser encoding contract and no longer applies;
  the terminal runtime keymap deliberately binds a curated subset, and exhaustive
  reachability is the palette's job.
- **Argument-free is checkable.** Whether a command is dispatchable with an
  empty payload is observable by dispatching it against a fresh runtime: a
  command that requires an argument fails with a "requires …" / "wrong type"
  message, while argument-optional and argument-free commands succeed (modulo
  benign state failures such as "no active prompt"). K5's oracle dispatches each
  bound command with an empty payload and asserts no argument-shaped failure.
- The runtime keymap is compiled-in C++ with no data-file dependency, so library
  consumers need no repository-relative resources. (The former
  `data/default-keymap.json` browser-encoding contract and its coverage test have
  been retired; see Background.)
- `KeymapViewState` already round-trips on the wire; the curated bindings ride
  the existing codec with no protocol change. Any hex/round-trip fixture that
  assumed an empty keymap is updated.
- Resolution lives in the library (not the app) so a future browser/`--http`
  client resolves identically; only byte→`KeyStroke` decoding is client-local.
- Only process quit remains an app-local key (terminal lifecycle, no registered
  command). All editor/panel/prompt/global actions — including tab and panel
  commands — resolve through the published keymap, satisfying
  `spec-navigation.md`'s "no hard-coded chord table" DoD.

## Risks and mitigations

- **Resolution ambiguity**: K2 validates prefix-freeness per resolvable context;
  fixtures construct strict-prefix pairs for `*`/`*`, focus/focus, and `*`/focus
  and assert `ambiguous_prefix`.
- **Context drift**: a test asserts `keymap_contexts()` equals `{*, editor,
  panel, prompt}`, so adding a `FocusTarget` value forces a deliberate update.
- **Curated keymap invalid at assembly**: a test asserts the assembled runtime
  keymap validates clean (empty reserved set) and that `EditorRuntime::create`
  succeeds; another asserts create fails with a message for a deliberately
  invalid keymap.
- **Escape-hatch loss**: a test asserts assembly rejects removal /
  contextualization / reservation / `*`-shadow of the last `settings.open`
  binding (K6).
- **Regressing TUI input**: K3b keeps a PTY smoke test proving `Esc S` saves,
  `ArrowDown` moves the caret in `editor` and the tree selection in `panel`, the
  palette still opens/types/selects/executes, and an unbound continuation after
  `Escape` clears leader and still inserts the printable.
- **Detail/label staleness**: palette `detail` is derived from the live keymap
  each snapshot with a deterministic binding preference; a test rebinds a command
  and asserts its candidate detail changes, and asserts full label coverage of
  the palette set.
- **Terminal Escape ambiguity**: a lone `Escape` byte is both a complete leader
  stroke and the introducer of a CSI/SS3 sequence. K3a resolves this with a
  bounded parser contract (below), not by guessing.

## Acceptance (Definition of Done)

- Observable: in the TUI, `ArrowDown` moves the caret when the editor is focused
  and the tree selection when the bar is focused; `Esc S` saves; tab and panel
  chords work through the keymap; the palette lists commands by human label with
  their chord shown as detail; the leader hint still appears mid-chord. No
  `panel`/`editor` `switch` on focus and no hard-coded chord table remain in the
  app (only process quit is app-local).
- Gates: `cmake --build build` clean; `ctest --preset dev` green; full
  `ctest -E performance_measurement` green.
- Oracles: see per-step Oracle column.

## Plan

Each step builds and ships independently, in order. K1 delivers the pure
contract; K2 loads the curated runtime keymap; K3a/K3b migrate the TUI; K4
delivers palette labels/detail. K1+K2 unblock navigation step 2; K4 unblocks
palette labels/detail.

| Step | Work | Files | Oracle |
|---|---|---|---|
| K1 | Add `keymap_contexts()` (derived from `FocusTarget`); `KeymapErrorCode::unknown_context` and `ambiguous_prefix` with their `validate_keymap` checks (context-name validity; order-independent strict-prefix per resolvable context); the pure `resolve_key_sequence` + `text_routing` with `*`-precedence / pending / none semantics; and `has_global_binding`. Update `doc/spec-navigation.md` step C wording to name `unknown_context` and delete its duplicated Step C row | `include/ssg/input.h`, `src/input.cpp`, `include/ssg/focus.h` (or reuse the `FocusTarget`-name helper), `tests/test_input.cpp`, `doc/spec-navigation.md` | `keymap_contexts()` == `{*,editor,panel,prompt}`; unknown context → `unknown_context`; strict-prefix (`*`/`*`, focus/focus, `*`/focus) → `ambiguous_prefix`, order-independent; exact-command-per-context resolution table (same key → different command per context); a `*` chord resolves in every context and beats a same-sequence focus binding; strict-prefix input → `pending`; non-match → `none`; `text_routing` per context; `has_global_binding` true/false cases (present vs reserved vs shadowed vs contextualized) |
| K2 | Author the curated runtime keymap as a compiled-in C++ table; fix `settings.open` to open a valid one-input settings prompt; at `EditorRuntime::create` run `validate_keymap` (empty reserved set) and assert `has_global_binding(keymap, "settings.open", {})`, failing create with a message on error (a guard protecting future edits to the compiled table; its predicates are unit-tested in K1); load it into `Impl::keymap` so it is published | `src/editor_runtime.cpp` (keymap table + assembly + load), `src/runtime/presentation.cpp` (`settings.open` prompt), `tests/runtime/test_runtime_snapshot.cpp` | the published keymap validates error-free (empty reserved set) and is non-empty; every bound command dispatched with an empty payload against a fresh runtime produces no argument-shaped failure (K5); `EditorRuntime::create` succeeds; dispatching `settings.open` yields `prompt` focus with a visible settings input (effect, not just resolution); `resolve_key_sequence` over the published keymap returns `cursor.line_down` for `ArrowDown@editor`, `tree.select_next` for `ArrowDown@panel`, `file.save` for `[Escape,KeyS]@*`, and `settings.open` for `[Escape,KeyF,KeyT]` in every context |
| K3a | Add byte→`KeyStroke` decoding to the terminal app: map arrows/Enter/Backspace/named keys/printables to `KeyStroke`s or committed text; and resolve fragmented ANSI vs. a standalone `Escape` with a bounded contract — after an `Escape` byte, if the next byte is already buffered it disambiguates (`[`/`O` → CSI/SS3; else a chord stroke); if none is buffered, a short bounded read (a few ms) distinguishes a lone `Escape` stroke from a fragmented sequence (timeout → the `Escape` stroke) | `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | decode table: byte sequences → expected `KeyStroke`/committed text; buffered `Esc [ A` → `ArrowUp`; buffered `Esc x` → `Escape` then `x`; lone `Escape` with no follow byte → the `Escape` stroke (timeout branch is unit-tested via the pure decode entry that takes an "input exhausted" flag); SGR mouse still decodes |
| K3b | Replace the TUI's hard-coded chord table and `focus==editor|panel` branch with: maintain a pending sequence, `resolve_key_sequence(published_keymap, pending, snapshot_focus)`, dispatch the resolved command (applying prompt-focus fulfillment for palette prompts), route committed text per `text_routing`, and report the pending sequence for the leader hint via the existing seam; keep only process quit app-local | `apps/ssg_main.cpp`, `tests/test_ssg_app.cpp` | `test_ssg_app` end-to-end table (bytes → resolved command per focus, incl. `ArrowDown` differing editor vs panel, `Esc S` → `file.save`, and an unbound continuation after `Escape` clearing leader while still inserting the printable); PTY demo: `Esc S` saves, `ArrowDown` moves caret in editor and tree in panel, palette opens/types/selects/executes |
| K4 | Add compiled-in `command_label(id)` metadata (full coverage of the palette-reachable set) and `format_key_sequence(seq)`; build palette candidates with `label = command_label(id)` and `detail` = `format_key_sequence` of the deterministically-preferred bound sequence from the runtime keymap (shortest, then lexicographically least form), empty if unbound | `include/ssg/command_metadata.h`, `src/command_metadata.cpp`, `include/ssg/input.h`/`src/input.cpp` (`format_key_sequence`), `src/runtime/snapshot.cpp`, `tests/test_palette.cpp` and/or `tests/runtime/test_runtime_navigation.cpp` | the `file.save` candidate has label `Save File` and detail `Esc S`; an unbound command has empty detail; every palette candidate has a non-id label (coverage); a command with two bindings shows the shortest/least; rebinding a command changes its candidate detail |
