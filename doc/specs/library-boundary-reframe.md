# Library boundary reframe: semantic core, per-client presentation

Status: draft, in review. Proposes moving several AGENTS.md global rules and
supersedes most of `doc/specs/browser-rendering.md`. On merge, its residue is the
reworded global rules, a few CONTRACT lines, and the surviving parts of the
browser work; this file is then deleted.

## The problem

SSG's stated invariant — *every UI element occupies server-described cells on the
shared monospace grid; a client may restyle for readability but never change
geometry* — was the scaffold that shipped the TUI and kept the first client thin.
As a permanent cross-client law it is now counterproductive. The target clients
are a terminal (cells), a web UI (HTML/CSS/JS, which separates layout, style, and
behavior by design), and desktop UIs (native toolkit or a graphics API). Forcing
a monospace grid onto the web throws away the platform's layout engine, native
scrollbars, touch, text shaping, and accessibility; a desktop app does not want
to be a grid either. The grid mandate makes every non-terminal client worse in
order to make them uniform, and uniform-by-geometry was never the actual goal.

The goal is a *similar experience* across clients: the same document and editing
semantics, the same commands and feature set, the same configuration, the same
keyboard model, the same reversibility. None of that requires a shared grid.

## What the code already says

The reframe is cheap because the library is already mostly semantic:

- `SessionSnapshot` and its sections (document text, selection as byte offsets,
  syntax spans, theme roles, tabs, tree, palette candidates, settings, keymap)
  reference neither `Renderer` nor `CellGrid`. The semantic model is independent
  of the grid.
- The grid is a separable consumer layer — `Renderer`/`CellGrid`,
  `Viewport` projection, `GraphemeLayout`, `Style` dimensions — that the terminal
  app drives. `ssg::Renderer{}.render(snapshot)` runs in the host, not the core.
- There are no grid CONTRACT lines. The mandate exists only in AGENTS.md prose,
  enforced by no type or test. Relaxing it changes no compiled contract.

The one real coupling: `EditorRuntime::snapshot()` takes `ViewportDimensions` and
returns a viewport-geometry section (`firstVisualRow`, `ScrollbarMetrics`, wrap).
That is the grid service — optional per client, not a universal law.

## The reframe

**The library owns semantics and behavior; each client owns presentation.**

The library is the authoritative source of editor, workspace, command, theme-role,
and semantic view-model *state*, and of editing *behavior* — one command
implementation, one document model, one selection model, one reversibility model.
Every client consumes that same semantic snapshot/delta and dispatches the same
commands, so the *product* is identical across clients. How each client lays out
and paints that model is its own concern: cells for the terminal, DOM/CSS for the
web, native toolkit or GL for desktop.

The monospace grid becomes a **service the library offers**, not a law it imposes.
`Renderer`/`Viewport`/`GraphemeLayout`/`Style` remain, and a grid client (the TUI,
or a desktop client that wants a terminal feel) uses them. A web client ignores
them and lays out the semantic model with CSS. Geometry is no longer a
cross-client contract.

The consistency guarantee moves from *same cells* to *same semantics, behavior,
feature set, config, and keyboard model*. That is the invariant worth stating and
testing.

### What the library provides to every client

- **Semantic editor state and behavior:** document, selection, history, commands,
  workspace, diffs, syntax spans, theme roles — over the snapshot/delta model.
- **Host filesystem I/O:** the platform adapters, with parity tests. This is a
  genuine cross-client service.
- **The feature set:** an enumerable, versioned surface (command registry +
  capabilities) so every client knows what the product can do and can drive it.
- **Configuration:** settings and the Lua host, shared across clients.
- **Theme as semantic roles:** colors as roles and sRGB values a client maps to
  its medium (CSS custom properties, a terminal palette, native colors). The
  16-color path is a terminal capability, not a wire law.

The library may also grow **client-specific helpers** where they genuinely help —
including web-specific ones (e.g. a delta shaped for DOM diffing) — the same way
it already helps the TUI with grid layout more than strictly necessary. Helping a
client is allowed; mandating a client's presentation is not.

## Global rules to move

Proposed, for review — not yet applied. AGENTS.md's global-rules list is changed
only with explicit sign-off.

- **Remove:** "Every UI element occupies server-described cells on the shared
  monospace grid; a client may restyle for readability but never change geometry,
  semantics, or behavior." Replace with: "The library owns semantic view state
  and behavior; each client presents it in its native idiom. The library offers a
  monospace grid-layout service that grid clients may use; geometry is not a
  cross-client contract. A client may not change semantics or behavior."
- **Reword:** "Every interaction enters through the typed client API and every
  observable view leaves through a snapshot or delta." Keep the snapshot/delta as
  the *semantic* sync channel and the typed API as the one interaction path; drop
  the implication that the snapshot describes pixel/cell geometry. Presentation is
  client-owned; state and commands are not.
- **Reword:** "No feature exists that a current standards-based browser cannot
  expose through the client API." The web is now a first-class client, not a
  constrained mirror. Reframe as: the feature set is library-defined and every
  target client can drive it through the typed API; a client may add native
  presentation affordances (scrollbars, touch, IME) on top.
- **Keep unchanged** (these are the real cross-client guarantees): one behavior
  path (same command implementation, same snapshot/delta model); theme is the
  single source of color as semantic roles; a connection's identity/capabilities
  come from host policy; keyboard-first (every action has a keyboard route);
  every user-visible action is registered and Lua-callable; platform services use
  adapters with parity tests; no blocking modal, destructive actions reversible;
  values live in code.

## What this deletes

Relaxing the grid mandate removes, not adds, work. `doc/specs/browser-rendering.md`
exists almost entirely to force a grid onto the browser; most of it dissolves:

- The **A′-vs-B fork** (publish cell runs vs WASM the layout) — gone. A web client
  lays out the semantic model with CSS; there is no cell-run wire section and no
  WASM projection boundary.
- The **fling / predictive-banding / overscan** machinery — gone. Native overflow
  scroll needs no server-published per-row geometry.
- The **scrollbar affordance-vs-authority** resolution — gone as a puzzle. A web
  client uses a native scrollbar over its own layout; the library's
  `ScrollbarMetrics` is a grid-service detail the TUI uses, not a thing the web
  client must honor.

What **survives** from that spec, because it is client-agnostic and about
semantics, not geometry:

- **Local echo for input latency.** Per-stroke round-trips make perceived typing
  latency equal the RTT (measured ~150 ms at 80 ms one-way); client-side
  prediction with server reconciliation decouples it (measured ~0 ms). This is
  true for any remote client and unrelated to the grid.
- **Deltas over whole snapshots.** The whole snapshot is ~0.5 MB even for a small
  document; incremental deltas are required for bandwidth and latency regardless
  of how a client paints.

## Path forward

1. **Move the invariants (this spec).** Review the reframe and the proposed
   global-rule edits; on sign-off, reword AGENTS.md's global rules and add a
   CONTRACT line where a promise now needs one (see Residue).
2. **Double-check TUI regression safety.** Before relaxing anything, confirm the
   TUI's behavior is pinned by contracts and tests that assert *semantics and
   behavior*, not merely grid goldens — so removing the universal-grid claim
   cannot silently change the terminal. The TUI keeps consuming the grid service;
   nothing about its path changes. Audit `tests/test_ssg_app.cpp`,
   `tests/test_render.cpp`, `tests/test_ui_layout.cpp`, `tests/test_terminal_parity.cpp`,
   and the library-contract tests for behavior coverage independent of the grid
   wire, and fill any gap before proceeding.
3. **Top-down web spike.** Let a web client drive what library affordances it
   actually needs — semantic document access, command dispatch, config,
   filesystem, feature enumeration — and let the boundary changes fall out of real
   use rather than being designed up front. Reuse the `ssg --http` harness from
   Spikes 0–3; carry local echo and deltas forward; drop the grid entirely on the
   web side and lay out with CSS.

## Residue (what survives this file's deletion)

- **Reworded global rules** in AGENTS.md (semantic-core, grid-as-service).
- **CONTRACT lines:** the library owns semantic state and behavior and imposes no
  presentation geometry — the grid is a service, not a contract; a client changes
  presentation freely but never semantics or behavior. Attach on the seam that
  owns the snapshot/command API.
- **Tests:** the TUI-regression behavior coverage from step 2; local-echo
  reconciliation and delta correctness from the surviving browser work.
- The A′/cell-run/scrollbar-authority material in `browser-rendering.md` is
  deleted with that file, its history retained in git.
