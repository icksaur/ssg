# spec-syntax-and-diffs

Status: DRAFT v4 (open questions resolved; two spec-reviews folded — general
(11 MUST/3 SHOULD) + diff-color model (4 MUST/3 SHOULD). Diff colors: theme-
derived, contrast+deltaE-gated tints on an orthogonal cell channel + CellGrid
sidecar, emitted via the existing `resolveColor` 256/truecolor path.) Ready for
implementation go-ahead.

Evaluation of what the SSG editor needs to provide (1) OOTB syntax
highlighting, (2) integrated diffs rendered WITH syntax highlighting, and
(3) "diff-following" to jump to agent-generated (Copilot/Claude) diffs.
Optionally (4) LSP completion — recommended DEFERRED.

Grounded in a four-part investigation (syntax pipeline, diff subsystem,
extension seams, and caco's files-applet diff-following — see
`doc/investigation-caco-diff-following.md`).

---

## Goals

After this work:

- Opening a source file shows real token colors (keywords, strings, comments,
  types, functions, numbers…) with no external process — highlighting is
  out-of-the-box for a first language set: C, C++, JavaScript, TypeScript, C#,
  and Lua (if a maintained grammar is available).
- A file under review shows diff line tints (added/removed/modified) **and**
  syntax token colors on the SAME lines simultaneously, plus word-level
  intra-line marks on modified lines (the high-value readability feature).
- When an agent writes diffs into files, the editor can jump to the newest
  changed hunk on demand, and (when following) auto-reveals the freshest change
  as diffs arrive — the caco "follow edits" experience, in-terminal.
- New behavior is added through EXISTING library seams (injected service
  interfaces, DiffModel event injection, CommandSet composition), not a new
  bolt-on plugin subsystem.

Non-goal (this spec): LSP completion is DEFERRED (see §Considerations Q3) but the
design must NOT paint it into a corner — the parser/feature seams stay open for a
future LSP `SyntaxParser` and a completion feature. Also out of scope:
semantic-token highlighting and side-by-side diff view.

---

## Design

### Current state (what already exists — do not rebuild)

Syntax render path is WIRED end-to-end and only the parser is missing:
- `SyntaxModel` holds a `std::shared_ptr<SyntaxParser>` (abstract:
  `hasGrammar(LanguageId)`, `parse(SyntaxParseRequest) -> SyntaxParseOutput`,
  `SyntaxModel.h:187-193`). Default injection is `nullptr`
  (`EditorRuntime.cpp:293`), so every parse returns `GrammarUnavailable` and
  falls back to all-`PlainText` spans.
- Parse requests are hardcoded to `LanguageId::plainText()`
  (`EditorRuntime.cpp:707`) — no language detection.
- Spans -> `SyntaxViewState` -> snapshot `.syntax` -> `Renderer::paintDocument`
  `scopeAt()` -> `syntaxIndex(theme, scope)` -> `CellGridCell.foreground` is all
  wired (`Renderer.cpp:420-436`). All 11 `SyntaxScope`s map to palette indices in
  the default theme.

Diff data + navigation exist; rendering and integration are STUBBED:
- `DiffModel` represents per-line change records (`DiffLineChange{kind,
  baselineLine, targetLine}`) grouped into `DiffHunk`s over a target document.
  Diff data enters `DiffModel` through two internal library seams: `GitDiffFile`
  and `SeededDiffFile` / `NonGitDiffEvent`. These are the model's INPUT seams; per
  spec.md I25 they must be driven by a LIBRARY diff-source component (Git
  integration / watch-vs-baseline), not by the client. The `NonGitDiffEvent` seam
  is the entry point the library diff-source uses for computed/agent diffs.
- `SemanticRole::Diff{Added,Removed,Modified}` and theme colors exist but
  `Renderer::paintDocument` never reads `snapshot.sections().diff`; the cell
  `role` is only ever set to Selection/SearchMatch/Foreground.
- `nextDiffHunk`/`previousDiffHunk` are correct and bound to
  `diff.next_hunk`/`diff.previous_hunk`, but the handlers `(void)`-discard the
  result — no cursor/viewport movement.

The follow-edits state machine already exists:
- `FollowEditsModel` with `FollowMode{Following, Paused}`, `activeTarget`,
  `queuedTargets`, `FollowTarget`, `FollowClientView`, footer projection, and
  `follow_edits.resume`/`follow_edits.pause` commands. This is structurally
  caco's `followEdits` boolean + missed-count badge + jump function.

### Cell composition + diff color channel (the key architectural fact)

`CellGridCell` carries INDEPENDENT `foreground` (syntax color index),
`background` (16-palette index), and `role`. Syntax always lives in `foreground`
and shows through everything below.

Diff backgrounds do NOT reuse the 16-palette `background` index (review-driven
correction). Reason: the 16 palette slots are fully spoken for by syntax + UI
roles, and a theme's single bright red/green is far too saturated to sit under
readable text — and intra-line marks need a SECOND, stronger tier on top of the
row tint, so one green + one red is nowhere near enough. Instead the cell gains a
small orthogonal DIFF TINT channel:

- add `CellGridCell.tint : DiffTint` (enum: `None`, `AddedRow`, `RemovedRow`,
  `ModifiedRow`, `AddedWord`, `RemovedWord`, `ModifiedWord`). Default `None`.
- the tint resolves NOT through the 16-palette but through a theme-shipped
  `DiffTints` color set (see "Diff colors" below) via the EXISTING
  `resolveColor(SrgbColor, depth)` path (`include/ssg/color.h`), which already
  emits truecolor / nearest-256 / nearest-16. No new emission code; a diff tint is
  just another theme `SrgbColor` flowing through the same depth adapter. This is
  how we "break out of the 16" cleanly — the 16-palette stays pristine; diff tints
  are direct RGBs resolved to >=256 color.

`paintDocument` for a diff cell sets `foreground` from syntax (unchanged) and sets
`tint` (NOT `background`). Trailing blank cells past line end also get the row
`tint` so the whole row reads as a diff row.

OUTPUT CARRIER (review MUST — the tint must actually reach the terminal). Today
`Renderer::render()` copies only `theme.palette` into `CellGrid`
(`src/Renderer.cpp:633-639`) and the terminal encoder resolves palette INDICES
(`apps/ssg_terminal.cpp:67-75,103-111`); a new tint color has no path to output.
Step 3c MUST add a `DiffTints` sidecar to `CellGrid` (the six resolved tint
`SrgbColor`s travel with the grid, like the palette does) AND extend the terminal/
client encoder to emit a diff cell's background via `resolveColor(diffTints[tint],
depth)`, with a browser/terminal parity test. At output: if `tint != None`,
background = `resolveColor(sidecar.diffTints[tint], depth)`; else the normal
palette background. Foreground (syntax) is unchanged either way, so syntax always
composes over the tint.

Precedence (background selection): the CURRENT renderer gives SearchMatch over
Selection (`Renderer.cpp:423-433`); preserve that. A diff `tint` applies ONLY when
no higher role-background applies: **SearchMatch > Selection > DiffWord > DiffRow >
normal background**. A selected or searched cell shows the role background (16-
palette) and suppresses the tint, so selection/search stay visible over diffs.
Within diffs, a word mark outranks its row tint on the same cell.

### Diff colors (theme-derived tints, contrast-guaranteed)

The `DiffTints` set is DERIVED by the Theme from the theme's OWN hue anchors, so
every theme gets readable, non-clashing diff colors for free (pit of success) and
no color enters outside theme data (honors the `color.h` "theme is the sole source
of color" invariant — derivation happens INSIDE `Theme`, the client only depth-
adapts via `resolveColor`).

Anchors: the theme already defines `SemanticRole::GitAdded` / `GitDeleted` /
`GitModified` — its own add/delete/modify hues (each maps to a palette `SrgbColor`).
Use those as the add/remove/modify hue anchors; do NOT guess which of the 16 slots
is "green".

Derivation (mechanism — chosen to satisfy the guarantee below, free to change):
for each kind, blend the anchor hue toward the editor `Background`, desaturated:
- Row tint = `blend(Background, desaturate(anchor, sRow), aRow)` with a SMALL
  weight `aRow` (a subtle wash, ~caco 18%). Because a subtle wash barely moves the
  background, existing syntax-vs-background contrast is nearly preserved — which is
  precisely why syntax stays readable on diff rows.
- Word mark = same anchor, STRONGER weight `aWord > aRow` (~caco 45%) and higher
  saturation, so intra-line marks pop above the row tint.

Guarantee (the INVARIANT the mechanism serves — this, not the blend, is load-
bearing). Readability is RELATIVE, not an absolute WCAG floor. IMPLEMENTATION
FINDING (folded after visual signoff): an absolute `contrast(tint, fg) >= 3.0`
against every syntax foreground is unachievable for a normal theme — a
mid-luminance accent (e.g. a purple keyword that itself only ~clears 3.0 on the
plain Background) forces every hued wash to near-black, erasing the hue and
producing an ugly fallback. So the rule is: for each tint and each applicable
foreground, on the RESOLVED (post-`resolveColor`) RGBs at BOTH Truecolor and
Indexed256,

    contrast(tint, fg) >= max(kFloorContrast, kRetainContrast * contrast(Background, fg))

i.e. the wash must retain a fraction (kRetainContrast = 0.80) of the contrast the
foreground already had against the editor Background, never below a hard floor
(kFloorContrast = 2.1). Because a subtle wash barely moves Background, existing
per-foreground contrast is nearly preserved — which is why syntax stays readable
on diff rows. "Applicable fg" = every syntax scope foreground + the default
`Foreground`.

Distinctness is a SECONDARY, best-effort property judged at TRUECOLOR ONLY:
1. Row-visibility: `deltaE(row tint, Background) >= ~2` at Truecolor (a diff row
   reads as diffed). Asserted.
2. Kind-separation (CIELAB CIE76): the shipped theme's derived washes separate
   Added/Removed/Modified well at Truecolor (dE ~5-11 for the default theme), but
   this is NOT a hard gate — Indexed256 quantization can collapse subtle washes,
   and diff kinds also read apart STRUCTURALLY (added rows vs phantom removed rows
   vs word-marked modified rows). Not asserted as a per-theme invariant.
The derivation prefers the readable derived washes when they are also
truecolor-distinct (their hue comes from the theme's Git anchors); a fixed
fallback set is used ONLY to rescue a degenerate theme whose near-monochrome
anchors make the derived tints indistinguishable, and otherwise the readable
derived washes are kept (readability is primary). Distinctness is judged at
Truecolor because 256-quantization distinctness is not reliably achievable.

Feasibility + failure semantics (`Theme::snapshot()` is `noexcept` at
`Theme.h:211`, so derivation is TOTAL and never throws). Given a theme with
adequate baseline syntax-vs-Background contrast, a subtle wash trivially satisfies
the relative gate (it barely moves Background). The word-mark weight is the
strongest value still passing the gate. If neither the derived washes nor a fixed
fallback is both readable and truecolor-distinct, the readable derived washes are
returned (readability first; distinctness degrades to structural cues).

ANSI16 degradation: on a 16-color terminal `resolveColor` collapses tints to
nearest ANSI slots and hue guarantees CANNOT hold (accepted limitation, same
category as 16-color syntax). On Ansi16, diff rows MAY fall back to the existing
16-palette `Git*`-style role background (no separate word-mark tier). The gate is
asserted only for Indexed256 and Truecolor.

Ownership + override: `Theme::snapshot()` computes `DiffTints` and ships them in
`ThemeSnapshot` (a small `struct DiffTints { SrgbColor addedRow, removedRow,
modifiedRow, addedWord, removedWord, modifiedWord; }`). A theme MAY override any
tint with an explicit color; an override is CLAMPED to satisfy the same gate (not
rejected, not silently trusted) — the derivation path and the override path both
end in the same clamp so the guarantee is unconditional. Default = derived.

Contract update (review MUST): `include/ssg/color.h:5-12` currently says only the
16 palette colors are authoritative and adaptation "introduces no color into the
snapshot/API". `ThemeSnapshot::diffTints` extends the authoritative color set, so
step 3c updates that contract comment (and clarifies `SrgbColor::fromSerializedChannels`
at `Theme.h:20-26`) to bless Theme-INTERNAL derivation while keeping the rule that
no RENDERER/CLIENT mints color — the client still only depth-adapts.

### Mechanism choice: tree-sitter (OOTB) over LSP for syntax

Syntax highlighting is implemented as a tree-sitter-backed `SyntaxParser`
injected into `SyntaxModel`. Rationale over the LSP alternative: SSG has protocol
framing but NO real LSP client, NO process spawning, and NO semantic-tokens
support — LSP highlighting would require building all three. Tree-sitter is a
single implementation of an interface that already feeds a fully-wired render
path, and works with zero external processes. LSP remains a future second
`SyntaxParser` implementation (semantic tokens -> `SyntaxScope`) behind the same
seam if ever wanted; this spec does not build it.

The tree-sitter parser + grammars are compiled UNCONDITIONALLY. Highlighting is
disabled at RUNTIME by constructing the runtime with a null
`EditorRuntimeConfig::syntaxParser`, which yields plain text; the shipped app
opts in via `defaultSyntaxParser()`.

Historical note: this was originally an optional CMake component
(`SSG_TREESITTER`) so the library could build with no tree-sitter dependency at
all. That capability was retired in doc/spec-grammar-pipeline.md Phase B: the
flag protected a configuration nobody used while taxing every change with a
doubled build-and-test gate, of which 80 of 81 tests were identical. The cost is
real -- every consumer, including `add_subdirectory` embedders, now compiles the
vendored C and therefore needs a C toolchain -- and is guarded by the
`test_embed_consumer` compatibility oracle.

### Agent diffs -> follow

Agent diffs enter through the `DiffModel` external-diff seam, revision-stamped.

BASELINE SEMANTICS (review MUST — the injection shape must change). Today
`DiffModel` computes each event against its own PRIVATE `acknowledgedContent` and
ADVANCES that baseline after every event (`DiffModel.cpp:270-284`), i.e. diffs are
event-to-event, not against a stable owner-supplied baseline. The stated
"diff vs a baseline snapshot" therefore cannot be expressed by the current
`NonGitDiffEvent`. Decision: EXTEND the external-diff injection to carry an
explicit `{baselineContent (or baseline revision), targetContent}` so the LIBRARY
diff-source component owns the baseline (content at open / last-accepted), and the
model computes target-vs-baseline without mutating an internal baseline.
(Alternative — adopt incremental event-to-event semantics and document it — is
rejected because "follow the newest change since I last looked" needs a stable
baseline.)

NEWEST-HUNK TARGET (review MUST — current `targetFor` just returns
`file.hunks.back()`, `FollowEditsModel.cpp:174-178`, which is wrong for a bottom-
unchanged file). On each applied diff, set `FollowTarget` to the newly-INTRODUCED
hunk: diff the incoming hunk set against the prior revision's hunk set and pick a
hunk not present before (by target range), not merely the last. Define hunk
correspondence when ranges shift (match by baseline range identity, fall back to
target range). `diff.next_hunk`/`diff.previous_hunk` provide manual stepping and
MUST move the caret + reveal (fixing the `(void)` discard).

BURST + REVEAL (review MUST — `applyNonGitEvent` exposes no burst boundary and
follow offsets never touch runtime selection/viewport; `runtime.follow` is used
only for attach/snapshot/pause-resume). Add batch metadata (a burst id or an
explicit "apply these N events, reveal once" API) so only the LAST file in a
burst activates/reveals (caco lesson #4). Add ONE domain operation that: opens the
target file, moves the caret to the target hunk mapped through the row projection
(below), and performs a PROGRAMMATIC reveal — and that programmatic reveal MUST
NOT flip `Following -> Paused` (only a user scroll/caret move does; the
`NavigationClass` distinction exists at `FollowEditsModel.h:21-25` but runtime
does not yet exercise it).

### Inline-overlay diff rendering + phantom removed rows (Q5)

Diffs render as an INLINE OVERLAY on the editable document, not a separate pane.
Added and modified lines already exist as real text in the editable buffer, so
they need only a diff tint stamped over the normal syntax-colored cells — editing
still works normally. The genuine complexity is REMOVED lines: they have no text
in the current buffer, so they are PHANTOM ROWS the renderer synthesizes between
real document rows. Removed baseline text MUST NOT be inserted into the editable
`Document` (review MUST) — it lives only in the projection.

ACTIVE-DIFF IDENTITY (review MUST). `sections().diff` (`session_snapshot.h:35-57`)
holds EVERY file's diff, but `DocumentViewState` carries NO file identity
(`snapshot.h:11-16`), so the renderer cannot today pick the `DiffFileView` for the
document it is painting, and matching by text is ambiguous. Prerequisite step:
publish stable document/file identity into the rendered document view-state (or a
per-document active-diff projection) so the renderer selects the correct
`DiffFileView` by identity + revision. This lands BEFORE diff rendering (Plan 3b).

COORDINATE OWNERSHIP (review MUST — name the owner + API). Introduce ONE typed
row projection — a `RealRow | PhantomRow` sequence — that maps visual screen rows
to either a buffer line (real, editable) or a baseline removed line (phantom,
non-editable). `Viewport`/`ViewportViewState` already owns the render/hit row
mapping (`Viewport.h:74-88`); it is the owner. Every consumer routes through it:
renderer row iteration, hit-testing (`HitTester.cpp:29-59`), selection's visual-
row reconstruction (`Selection.cpp:129-148,472-526`), caret up/down/page movement
(`runtime/editing.cpp`, `runtime/presentation.cpp`), viewport reveal/scroll, and
follow target mapping (`FollowEditsModel.cpp:26-30`, which currently computes
line-based offsets ad hoc). `Selection` and follow MUST NOT reconstruct visual
rows independently. When no diff is shown the projection is identity (zero cost,
zero behavior change).

PHANTOM ROW SEMANTICS (review MUST — enumerate, then oracle each). A `PhantomRow`:
- renders baseline text tinted `DiffRemoved` across the full row width;
- is non-focusable: a CLICK on a phantom row resolves to the nearest editable
  buffer position (the start of the following real row), never a phantom "offset"
  — current hit-testing always yields a valid byte offset (`HitTester.cpp:29-59`),
  so phantom rows must map deterministically, not return a fake offset;
- caret up/down and page movement STEP OVER phantom rows (skip to the next real
  row) but the viewport still scrolls them into view;
- a DRAG across phantom rows selects only the spanned REAL buffer text (phantom
  rows contribute visual painting, not selection content);
- scrollbar totals and reveal centering count phantom rows as visual rows;
- wrapped removed lines and phantom rows at file start/EOF are covered by the
  same projection (a removed hunk before line 0 or after the last line is a
  leading/trailing phantom block).

REMOVED-ROW SYNTAX (review MUST — baseline text has no target offset, so the
existing `SyntaxViewState` cannot color it). Decision: phantom removed rows are
TINT-ONLY (DiffRemoved background, default foreground) in this spec — syntax
coloring of removed baseline text is explicitly OUT of scope and NOT undefined.
(Syntax on removed rows would require parsing and caching a baseline
`SyntaxViewState` per file; deferred as a follow-up. Real added/modified rows keep
full syntax since they have valid target offsets.)

Invariant this introduces (see §Invariants "buffer-vs-visual coords"): when
phantom rows are present, buffer offsets and visual screen rows diverge, and the
single `Viewport`-owned `RealRow|PhantomRow` projection is the enforcing
mechanism. Identity mapping when no diff is shown.

Intra-line marks (Q6, high value): modified lines carry word-level add/del marks.
The library computes an intra-line diff (token/word ranges) for each modified
hunk line and exposes mark ranges in the diff view-state; the renderer sets the
cell's `tint` channel to the `*Word` kind (stronger than the row tint) for marked
cells. Carrier = the DiffTint channel (§"Cell composition"), NOT a 16-palette
slot; `foreground` stays syntax, so marks compose with syntax color by
construction. Word tints are derived + contrast-gated in step 3c.

### LSP + completion extensibility (Q2, Q3) — build seams, not the features

This spec implements tree-sitter only, but must leave two doors open. Transport-
agnostic result types ALONE are insufficient (review MUST): `SyntaxParser::parse`
is SYNCHRONOUS and `EditorRuntimeConfig` exposes NO parser injection
(`EditorRuntime.h:15-25`) — the default parser is constructed INSIDE the library
runtime (`EditorRuntime.cpp:278-299`), so a future async, app-owned LSP transport
could not "drop in." Requirements to avoid a rewrite:
- Syntax via LSP later: (a) keep `SyntaxScope`/`SyntaxParseOutput` parser-agnostic
  (no tree-sitter types in the public interface); AND (b) add a RUNTIME injection
  seam so the app supplies the parser/provider (extend `EditorRuntimeConfig` or an
  equivalent factory) instead of the runtime hard-constructing it; AND (c) accept
  that an async LSP source publishes syntax results out-of-band — leave room for
  an app-published syntax-result path (the model already accepts a view-state it
  did not compute). This spec adds the injection seam (Plan 3) even though it only
  injects the tree-sitter parser today.
- Completion later: do NOT delete or narrow the modeled completion types
  (`LspCompletionItem`/`ViewState`/`Acceptance`) nor the `LspFeatureController`
  seam. The feature stays a `CommandSet` + view-state + delta like every other,
  so adding it later is the same shape as this work, requiring no new subsystem.
No LSP client, transport, or completion UI is built here.

### Extension model (the "plugin in existing terms")

No new plugin subsystem. Three existing seams carry all three features:
- injected service interface: `SyntaxParser` (syntax).
- data injection: `DiffModel` external-diff events (diffs, incl. agent diffs).
- behavior unit: `CommandSet` composition in
  `EditorSessionBuilder::p0CommandDescriptors` (commands for all of it).

---

## Invariants

- Library/app boundary (feature vs. mechanism — see spec.md I25): the library
  owns every FEATURE and its orchestration, including I/O-driven ones. Diff
  SOURCING is a library concern: the library owns Git integration, filesystem-
  watch orchestration (via the existing library `FilesystemWatcher`), baseline
  tracking, diff computation, and follow. Only the RAW platform mechanism the
  library cannot portably provide — native filesystem events, the git subprocess/
  process spawn, a language-server socket — is a narrow injected adapter behind a
  library interface (the `FilesystemWatcher` and LSP process/stream adapters are
  the templates); the adapter carries bytes/events, never feature logic. The
  client does NOT watch, compute, or inject diffs; it renders and translates
  input only. (An earlier draft wrongly placed "watch, compute vs baseline,
  inject" in the app — corrected.) The tree-sitter parser runs in-process (a pure
  computation over text), also library-side.
- Buffer vs visual coordinates (enforcing rule for a mechanism consequence): the
  divergence between buffer offsets and visual rows is a CONSEQUENCE of choosing
  phantom-row rendering; the load-bearing invariant is that exactly ONE component
  (`Viewport`) owns the `RealRow|PhantomRow` mapping and every caret/selection/
  hit-test/reveal/follow site routes through it — no site does ad-hoc row
  arithmetic. Buffer offsets never include phantom rows; identity when no diff.
- Active-document/diff coherence: the diff a document renders is selected by
  stable document/file IDENTITY and matching revision, never by text matching;
  `sections().diff` holds all files, so an unidentified document must render NO
  diff rather than a guessed one.
- Follow reveal never pauses following: a PROGRAMMATIC reveal/caret move issued by
  follow (or diff navigation) must not flip `FollowMode::Following -> Paused`;
  only a USER navigation does. (The `NavigationClass` distinction exists at
  `FollowEditsModel.h:21-25`; runtime must start exercising it.)
- Public syntax types are transport-agnostic AND the parser is runtime-injected:
  no tree-sitter (or any vendor) type appears in `SyntaxScope`/`SyntaxParseOutput`/
  the syntax view-state, and the runtime accepts the parser via an injection seam
  rather than hard-constructing it — so an alternative `SyntaxParser` (e.g. LSP
  semantic tokens) can be supplied without editing library runtime.
- Diff colors are theme-derived and contrast-gated: the `DiffTints` set is
  produced INSIDE `Theme` from the theme's own `Git*` anchors and MUST pass the
  readability/distinctness gate (§Design "Diff colors"); no renderer or client
  derives or substitutes color — the client only depth-adapts via `resolveColor`,
  so every client presents identically. Diff tints never consume a 16-palette slot.
- Wire contract is by explicit string literals (command ids, section keys), not
  C++ identifier names. New commands/sections add literals; goldens
  (`test_protocol`, snapshot goldens) must be regenerated deliberately, never
  silently.
- Snapshot/delta symmetry: any new view-state (richer diff/follow state, phantom/
  mark data, document identity) that ships to clients has a `derive`/`replay`
  delta codec and round-trips; the codec change LANDS IN THE SAME STEP as the
  state-shape change, not deferred.
- Objects valid at construction; commands are members of the domain object;
  `*CommandSet` files stay thin registration tables (per cpp-values.md).

---

## Considerations

RESOLVED DECISIONS (2026-07-20):

- Q1 — tree-sitter vendored under `vendor/` (originally behind `SSG_TREESITTER`,
  compiled unconditionally since spec-grammar-pipeline Phase B). Language
  set: C, C++, JavaScript, TypeScript, C#, Lua (Lua only if a maintained grammar
  is available; if not, ship the other five and note Lua as follow-up). Grammars
  are C, compiled into the library behind the flag.
- Q2 — syntax mechanism = tree-sitter OOTB now; LSP is a future second
  `SyntaxParser` behind the same seam (see Design "extensibility"). Public syntax
  types stay transport-agnostic so LSP semantic tokens drop in later.
- Q3 — completion DEFERRED, but no corner-painting: keep modeled completion types
  and the `LspFeatureController`/`CommandSet` seams intact so the feature is a
  later same-shaped add, not a rewrite.
- Q4 — diff detection is LIBRARY-side via the existing library `FilesystemWatcher`
  (see spec.md I25): the LIBRARY observes file writes (through the injected native-
  watch mechanism adapter), tracks the baseline, computes the diff, and drives
  follow. The client does nothing. NOTE (review MUST): the current `NonGitDiffEvent`
  computes against the model's private, self-advancing `acknowledgedContent` — it
  does NOT accept an owner-supplied baseline. The internal diff-source flow must
  carry `{baseline, target}` explicitly (see Design "Agent diffs -> follow"), with
  the baseline (content at open / last-accepted) owned by the library diff-source
  component, not the client.
- Performance premise correction (review MUST): the "18.8 ms open" figure earlier
  drafts used was the `eol_scan` SUB-PHASE, not the open. Per
  `spec-large-files-loading.md`, the LF-1 pre-optimization baseline is ~106 ms and
  LF-2..LF-3b shipped optimizations on top; the CURRENT open must be RE-MEASURED
  on the actual harness (`startup_benchmark file_open` / `open_path_benchmark`) at
  implementation time, and tree-sitter parse/highlight latency budgeted
  SEPARATELY from open. Also `refreshSyntax` currently runs `syntax.run`
  SYNCHRONOUSLY (`EditorRuntime.cpp:698-710`), so the "off the edit path" claim is
  not yet true — either move parse off the synchronous refresh or budget it on the
  edit path explicitly.
- Q5 — INLINE OVERLAY (not a separate pane). Added/modified lines are tinted over
  the real editable buffer; removed lines are non-editable PHANTOM ROWS. See
  Design "Inline-overlay diff rendering + phantom removed rows".
- Q6 — word-level intra-line marks are IN SCOPE (high readability value),
  implemented as a diff-view-state mark range that composes with syntax fg.

Edge/complexity notes:
- Incremental reparse: `SyntaxParseRequest` already carries edits
  (`SyntaxEdit`/`SyntaxPoint`); the tree-sitter parser must map document edits to
  tree-sitter's `TSInputEdit` and reuse the prior tree, or re-parse whole on
  first cut (simpler; measure).
- Language detection: `LanguageId` from file extension (small table); unknown =
  plain text. Lives where the parse request is built (`EditorRuntime`).
- Diff role is line-level but the renderer stamps per cell; a blank/short line
  still needs its full width tinted to read as a diff row (caco dual-gutter
  lesson). Decide how trailing cells past line end get the diff background.
- Phantom removed rows make buffer coords != visual coords. All caret/selection/
  hit-test/reveal math must route through the single buffer<->visual mapping
  (Design Q5); this is the highest-risk part of the change. Identity mapping when
  no diff present.
- Diff color derivation: anchors are `GitAdded`/`GitDeleted`/`GitModified` (theme-
  owned). Contrast metric = WCAG relative-luminance ratio; pick concrete
  thresholds (a comfortable T_read for row tints, a floor for word marks) during
  step 3c and pin them in the oracle. The subtle-wash row tint nearly preserves
  existing syntax contrast; word marks are the risky tier to gate. Compute
  derivation in a colorspace where "darken+desaturate toward background" is clean
  (HSL/HSV or luminance blend in sRGB); the blend is a mechanism, the contrast
  gate is the invariant. Handle pathological (near-monochrome / low-contrast)
  themes by clamping luminance to meet T_read.
- Follow guards: caco needs scroll-suppression guards because the browser fires
  scroll events on re-render. In a terminal the analog is: a programmatic reveal
  must not flip `Following`->`Paused`; only a user scroll/caret move does.

---

## Risks and Mitigations

- Tree-sitter build complexity / portability -> vendor pinned grammar sources and
  keep the null-parser plain-text path as the always-available default. The
  `SSG_TREESITTER` opt-out that originally covered this was retired (see above);
  `test_embed_consumer` now proves the embedder build path.
- Highlighting perf on large files -> `scopeAt` is O(log n) per cell and only
  visible rows render. Tree-sitter PARSE of a whole 10 MiB file is its own cost
  (tens of ms range) and currently runs synchronously in `refreshSyntax`
  (`EditorRuntime.cpp:698-710`); either move it off the synchronous path or budget
  it explicitly. Re-measure open (post-LF-3b) AND parse/highlight separately on
  the 10 MiB fixture; do not conflate the two.
- Diff/selection color collision -> diff is a TINT channel resolved via
  `resolveColor` (>=256 color), never a 16-palette slot; precedence SearchMatch >
  Selection > DiffWord > DiffRow > normal bg (selection/search suppress the tint).
  Readability guaranteed by the theme-side contrast gate, pinned by a contrast
  oracle over the shipped theme(s).
- Unreadable diff text -> theme-derived tints from `Git*` anchors, darkened+
  desaturated toward Background with a WCAG-contrast clamp; word marks
  (strongest tier) are the primary oracle target; pathological themes clamp
  luminance to meet the readability floor.
- Follow "jumps to wrong hunk" -> the newest-not-topmost rule pinned by a hand-
  case oracle over two successive diff revisions with prior hunks at BOTH top and
  bottom and the new hunk in the middle (so the current `hunks.back()` shortcut
  fails the oracle).

---

## Acceptance (Definition of Done)

- Observable (NEEDS VISUAL SIGNOFF): a source file (each shipped language:
  C/C++/JS/TS/C#/Lua) opens with token colors; a seeded diff shows added/removed/
  modified row tints (theme-derived, readable — token colors stay legible on the
  tint) WITH those token colors on the same lines; removed lines appear as phantom
  rows the caret skips; modified lines show word-level marks; issuing
  follow/next-hunk reveals the newest changed hunk. Provide terminal captures /
  cell-grid dumps for signoff.
- Budgets (review MUST — rebased on the real harness): re-measure the CURRENT
  10 MiB open (post-LF-3b) via `startup_benchmark file_open` / `open_path_benchmark`
  and record it; tree-sitter parse+highlight of the 10 MiB fixture is budgeted
  SEPARATELY (target: name a number after a first measurement, not before); no
  per-frame regression in `Renderer` for the visible-rows render. Do NOT cite the
  18.8 ms eol_scan sub-phase as an open budget.
- Gates: `bash scripts/check.sh` green (0 warnings, all tests).
- Oracles:
  - syntax mapping: golden of `(byteOffset -> SyntaxScope)` for a fixture in EACH
    shipped language (not just C/C++), including a multibyte-UTF-8 case (byte
    offsets land on scalar boundaries) and an overlapping-capture precedence case;
    produced by the parser, independent of the renderer; MUST fail before impl.
    Lua's fixture is conditional on the Lua grammar shipping (consistent with Q1).
  - cell composition: renderer test asserting a cell on a diff line has
    `foreground==<syntax index>` AND `tint==AddedRow` (or the right kind)
    simultaneously, including trailing blank cells tinted; plus precedence cases
    (search suppresses tint, selection suppresses tint, word mark over row tint,
    search-over-selection preserved).
  - diff colors (readability property oracle): for each shipped theme, on RESOLVED
    RGBs at Truecolor AND Indexed256 (post-`resolveColor`, not the ideal), for
    EVERY (diff tint x syntax-scope foreground) pair and (diff tint x default
    Foreground), assert the RELATIVE readability rule `contrast(tint, fg) >=
    max(kFloorContrast, kRetainContrast * contrast(Background, fg))` (WCAG luminance
    ratio, computed INDEPENDENTLY in the test). Plus row-visibility:
    `deltaE(row tint, Background) >= ~2` at Truecolor (a diff row reads as diffed).
    Strong kind/word deltaE is NOT asserted per-theme (best-effort; see Design).
    Exercise the derivation across the default theme (derived washes kept), a
    near-monochrome-ANCHOR fixture and a light-background fixture (readability holds
    via the fixed fallback), confirming readability is total.
  - color path: `resolveColor(tint, Indexed256)` yields a 256-cube/gray index
    (16..255, not an ansi16 slot) and `resolveColor(tint, Truecolor)` is identity —
    confirming tints break the 16 without touching the palette; and the Ansi16
    fallback path renders diff rows via the role-background degradation (no word
    tier).
  - color path: `resolveColor(tint, Indexed256)` yields a 256-cube/gray index
    (16..255, not an ansi16 slot) and `resolveColor(tint, Truecolor)` is identity —
    confirming tints break the 16 without touching the palette; and the Ansi16
    fallback path renders diff rows via the role-background degradation (no word
    tier).
  - phantom rows: oracle over a hunk with removed lines asserting (a) a click on a
    phantom row resolves to the following real buffer offset, (b) caret down steps
    over phantom rows, (c) a drag spanning phantom rows selects only real text,
    (d) scrollbar total counts phantom rows; and identity behavior (all unchanged)
    when no diff is shown.
  - follow target: hand-case over two diff revisions (prior hunks top AND bottom,
    new hunk middle) asserting `activeTarget` = the newly-introduced hunk, not
    `hunks.back()`.
  - navigation: `diff.next_hunk`/`previous_hunk` move the caret/viewport to the
    hunk (assert selection + reveal, via the row projection), replacing the
    discarded-result test; and a programmatic reveal does NOT flip Following->Paused.
  - intra-line marks: golden of word-level mark ranges for a modified line, plus a
    renderer assertion that a marked cell keeps its syntax `foreground`.
  - round-trip: every new/extended view-state delta (diff incl. phantom/mark,
    follow, document identity) `derive`->`replay`==identity, in the SAME step that
    adds the state.

---

## Plan

DEFERRED: LIBRARY diff SOURCE (not app-side — see spec.md I25). The library
diff/follow machinery (rendering, phantom rows, tints, marks, newest-hunk follow,
external-diff injection API) is built and green, but no diff SOURCE is wired yet: a
LIBRARY-owned component that tracks a baseline and produces diffs — from the Git
working tree and/or from filesystem-watch-vs-baseline (via the existing library
`FilesystemWatcher` + its injected native-watch adapter). The client is never
involved. Deferred pending a proper design; today the diff subsystem is exercised
only by tests. A code review (2026-07-21, branch `feat-syntax-diffs` vs master)
surfaced findings that live in this deferred subsystem — resolve them AS PART OF
the diff-source design/wiring, not in
isolation (their correct fix depends on how diffs get injected):

- MUST — active-diff coherence (`src/DiffModel.cpp` `fileForDocument`):
  `document.revision != revision` compares the document EDIT-revision against the
  DiffViewState revision, which are independent counters. On mismatch the overlay
  silently returns nullopt (no diff shown) with no failing test. The diff-source
  design MUST define the revision contract: injected diffs carry the exact
  document revision they were computed against, and a mismatch must be observable
  (assert/log/dedicated staleness result), never a silent empty. This is the #1
  correctness item for diff wiring.
- SHOULD — follow/reveal offsets use `rowProjectionUnwrapped`
  (`src/FollowEditsModel.cpp:247`, `src/EditorRuntime.cpp:876`), so published
  follow offsets are wrong when word-wrap is on. Use the wrap-aware projection for
  reveal.
- SHOULD — next/previous hunk uses the ACTIVE document's caret line even when the
  command payload targets a different diff file (`src/runtime/navigation.cpp:134`).
  Use the targeted file's own current position, not the active caret.
- NIT — phantom removed-row text strips a trailing `\n` but not `\r`
  (`src/Viewport.cpp` `lineText`), so CRLF baselines can show a stray carriage
  return. Strip both.

Review areas confirmed CLEAN: tree-sitter integration + query cache lifetime/
thread-safety; color derivation + readability gate + precedence (SearchMatch >
Selection > DiffWord > DiffRow) in renderer and encoder; protocol/snapshot
additive symmetric round-trip; external-diff burst atomicity; single-owner
row-projection usage across consumers.

NOT A BUG (review SHOULD re-examined) — per-document `syntaxModels` are keyed by
`FileDocumentId` and persist for the session, but this is CONSISTENT with document
lifetime, not a leak: closing a tab does NOT destroy the `Document` (the
`Workspace` never removes documents — `close()` journals for recovery + scratch
cleanup only; the document stays reopenable, IDs never reused). So a syntax model
lives and dies exactly with its document, identical to the pre-existing
`histories` map and the documents themselves. The reviewer's "never evicted on
close" conflated tab-close with document-destruction. The only real (pre-existing,
feature-independent) question is whether closing a tab should eventually FULLY
destroy a document and free its bytes + history + syntax together — a session-
memory policy that predates this work and applies uniformly; out of scope here.

Ordered; each step green before the next. Codec/round-trip work lands IN the step
that changes a shipped state shape (not deferred to a trailing step).

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Language detection: `LanguageId` from file extension; build parse requests with the real language | `SyntaxModel.h`/`.cpp` (or a small `LanguageId` detector), `EditorRuntime.cpp:707` | golden: extension->LanguageId table | wire-literals |
| 2 | Tree-sitter `SyntaxParser` impl for C, C++, JS, TS, C#, Lua(if grammar avail), originally behind `SSG_TREESITTER`; vendor grammars; produce `SyntaxSpan`+scopes; keep public types parser-agnostic | new `src/TreeSitterParser.*`, `vendor/tree-sitter*`, `cmake/components/treesitter-syntax.cmake`, `CMakeLists.txt`, `tests/fixtures/syntax/*` | golden: byteOffset->SyntaxScope per shipped language + multibyte + overlap cases (fails before impl) | lib/app boundary; syntax types transport-agnostic |
| 3 | Runtime parser-injection seam: app supplies the parser (default tree-sitter) instead of runtime hard-constructing; null stays the no-treesitter default | `EditorRuntime.h:15-25` (config/factory), `EditorRuntime.cpp:278-299`, app wiring | build+run both cmake configs green; a test injects an alternate parser without editing runtime | parser runtime-injected |
| 3b | Publish stable document/file identity into the rendered document view-state (or a per-document active-diff projection) so a document selects its `DiffFileView` by identity+revision | `snapshot.h` (DocumentViewState), `session_snapshot.h`, `Protocol.cpp`, snapshot/protocol goldens, matching `*DeltaCodec` | round-trip identity; unidentified doc renders no diff | active-diff coherence; snapshot/delta symmetry; wire-literals |
| 4a | `RealRow\|PhantomRow` projection OWNED BY `Viewport`; route renderer/hit-test/selection/caret-movement/reveal/follow through it; identity when no diff | `Viewport.h/.cpp`, `Selection.h/.cpp`, `HitTester.cpp`, `runtime/editing.cpp`, `runtime/presentation.cpp`, `FollowEditsModel.cpp` | phantom oracle: click->next real offset, caret steps over, drag selects real only, scrollbar counts phantom; identity when no diff | buffer-vs-visual coords (one owner) |
| 3c | Theme-derived `DiffTints` (add/remove/modify row + word) from `Git*` anchors, darken+desaturate toward Background, clamped to the min-contrast + deltaE gate (total/`noexcept`, fallback to safe set); ship in `ThemeSnapshot`; add `CellGridCell.tint` + a `DiffTints` sidecar on `CellGrid`; extend terminal/browser encoders to emit tint bg via `resolveColor`; update the `color.h` contract comment | `Theme.h`/`Theme.cpp`, `Renderer.h` (cell `tint` + CellGrid sidecar), `Renderer.cpp:633-639`, `apps/ssg_terminal.cpp:67-111`, browser client encoder, `color.h` (comment), `Protocol.cpp:4376-4394,4754-4765` + protocol/snapshot goldens + `ThemeSectionDelta` derive/replay | contrast+deltaE property oracle on resolved RGBs (Truecolor+256) over shipped + near-mono-anchor themes; theme-sanity precondition; terminal/browser parity; ThemeSnapshot round-trip | diff-colors theme-derived + contrast-gated; snapshot/delta symmetry; wire-literals |
| 4b | Diff overlay in renderer incl. phantom rows: select active `DiffFileView` (3b), set per-cell `tint` (row kind) NOT a 16-palette background, precedence SearchMatch>Selection>DiffWord>DiffRow>normal; full row width incl. trailing cells; phantom rows render baseline text tinted RemovedRow (tint-only, no syntax) | `Renderer.cpp:420-443`, helper `diffTintAt` | renderer test: tint + syntax fg coexist; precedence cases; phantom-row render | buffer-vs-visual coords; diff-colors |
| 5 | Wire `diff.next_hunk`/`previous_hunk` handlers to move caret + reveal viewport via the 4a projection; programmatic reveal must not pause follow | `src/runtime/navigation.cpp` (diffCommand), `FollowEditsModel` (NavigationClass use) | navigation oracle: caret+reveal at hunk; reveal does not flip Following->Paused | wire-literals; reveal-not-pause |
| 6 | Extend external-diff injection to carry a library-diff-source-owned `{baseline,target}` (not self-advancing `acknowledgedContent`); newest-introduced-hunk `FollowTarget`; burst API so only last file reveals; ONE domain op opens file + moves caret (via projection) + programmatic reveal | `DiffModel.*` (injection shape + baseline), `FollowEditsModel.*` (`targetFor`), `EditorRuntime`/runtime glue, library diff-source + `FilesystemWatcher` wiring, matching `*DeltaCodec`, goldens | hand-case: activeTarget=newest hunk (prior top&bottom, new middle); baseline-vs-target diff correct; round-trip | snapshot/delta symmetry; feature-not-mechanism (I25); reveal-not-pause |
| 7 | Word-level intra-line marks: intra-line diff for modified lines -> mark ranges in diff view-state; renderer sets per-cell `tint` to the `*Word` kind over the row tint, keeping syntax fg; word tints derived+gated in 3c | `DiffModel.*`, `Renderer.cpp`, matching `*DeltaCodec`, goldens | golden: intra-line mark ranges; renderer test marks keep syntax fg; contrast gate covers word tints; round-trip | snapshot/delta symmetry; diff-colors |

Completion (Q3): out of scope. The parser-injection seam (step 3) and untouched
completion types keep it a later same-shaped add.

---

## Rationale (skippable)

The investigation's biggest surprise inverted the initial "lean on LSP to cap
complexity" assumption: for syntax, LSP is the HIGH-complexity path here (no
client/transport/semantic-tokens exist) while tree-sitter is a single interface
implementation feeding an already-complete render path. The second surprise is
how much is already built — the syntax render pipeline, the diff data model +
navigation logic, and the follow-edits state machine all exist. The spec review
sharpened the real scope: the cheap parts are genuinely cheap (inject a parser,
merge a diff tint), but three areas carry real design work the first draft
under-weighted — (a) phantom removed rows force a single-owner buffer<->visual
coordinate projection touching selection/hit-test/caret/reveal/follow; (b)
external diffs need a library-diff-source-owned baseline because the current model self-advances
its baseline; and (c) the follow "newest hunk" and reveal-without-pausing must be
built, not assumed, since `targetFor` returns `hunks.back()` and reveal never
touches runtime selection today. caco's files-applet validates the follow design
and contributes the "newest-not-topmost" and "last-file-only burst" refinements
(see the investigation report). The whole change still lands on existing seams —
an injected `SyntaxParser`, `DiffModel` injection, `CommandSet` composition —
library terms" requirement without a new plugin system.
