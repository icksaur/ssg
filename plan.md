# Web editor responsiveness

## Goal

Make ordinary web-editor text input, caret movement, and selection changes
performance-independent of total document size. The input-to-next-paint path
may scale with the changed text, active selection ranges, and visible rendered
content, but must not scan, clone, serialize, parse, or replace an entire
document merely because a caret or selection changed.

The SSG library remains authoritative for editing behavior, offsets, syntax,
and published UI state. Browser caches, DOM structure, and scheduling are
presentation implementation details; they must not establish an alternative
editing or selection behavior.

## Targeted work

1. Done: retain document text and syntax DOM in the web client. Add a
   caret/selection-only render path that updates overlays or the small affected
   range without rebuilding the document or assigning document-wide
   `innerHTML`.
2. Done: maintain an incremental byte-to-DOM/UTF-16 projection so cursor and
   selection updates do not repeatedly traverse the complete UTF-8 document.
   Syntax lookup must use indexed or binary-searchable runs rather than
   per-boundary linear scans.
3. Done: coalesce received state, command completions, and pointer-drag moves to one
   paint per animation frame, preserving authoritative command settlement
   order.
4. Done: apply retained-state deltas with copy-on-write only for changed branches.
   Do not clone full document text, syntax, UI tree, or candidate inventories
   for caret-only updates.
5. Done: in the library and HTTP host, cache a revision-keyed immutable flattened
   document snapshot for navigation and publication. Do not construct an
   unchanged idle snapshot before discovering whether publication is needed.
6. Done: pass accepted text edits to incremental syntax parsing, with focused
   differential tests that compare incremental output to a full parse.
7. Deferred pending measurement: consider client-only document windowing only after measuring the retained
   DOM implementation. Do not add viewport geometry or a web-specific document
   transport format to the library contract.

## Implemented boundaries

- The web client retains syntax runs and updates selection through the browser
  highlight API plus a positioned caret overlay. Unsupported browsers show a
  visible refusal rather than silently omitting authoritative selections.
- Document runs and byte projections rebuild only when document text, syntax,
  theme, or local prediction placement changes.
- The runtime caches active flattened text by document identity and revision.
  It is reused for navigation and snapshot assembly; the HTTP route skips
  publishing when the session revision did not change.
- Incremental syntax edits are used only when the syntax model holds a
  compatible prior parse; fallback and language-change paths retain full parse
  behavior.

## Validation

Benchmark large ASCII and Unicode documents with repeated caret movement,
selection extension, typing, and pointer drag. Record input-to-next-paint
latency, DOM mutation counts, long tasks, allocations, and render count per
input. Add focused correctness coverage for UTF-8 offsets, astral characters,
multi-range selection, local prediction settlement, and incremental syntax
equivalence. The benchmark must show that ordinary input and selection work
does not grow with document size.

# Architecture simplification

## Direction

Treat the project as three authority layers:

1. The library owns editor behavior, authoritative semantic state, and typed
   platform/disk services.
2. The protocol serializes that semantic state and typed input without adding
   product behavior or client geometry.
3. Clients own native presentation, layout solving, prediction, and device I/O.

The library may have internal semantic-core, platform-adapter, and optional
presentation-adapter subdivisions. The protocol may separate codecs from
transport lifetime, replay, authentication, and backpressure. These are
mechanism boundaries, not additional product-behavior layers.

Prefer one whole-screen UI structure with many operations over parallel
feature-specific structures. Adding a library feature should require no client
layout change and at most a new widget or typed data binding. Responsive local
operations such as fuzzy narrowing remain client-side predictions over the
library-published candidate universe; they must not acquire a routine
round-trip.

## Current accidental complexity

- `WholeScreenAssembly` builds the semantic UI tree while `ShellState` builds
  and lays out a second shell tree. Features consequently acquire UI nodes,
  shell request fields, shell-node kinds, renderer branches, hit structures,
  and web branches.
- `PresentationSnapshot` and general session deltas carry terminal/grid
  geometry despite dimensionless semantic snapshots already serving native
  clients.
- Browser replay manually mirrors only part of the session delta inventory.
  A field can be correctly added to C++ encoding yet remain stale in the web
  client.
- Pointer and compound interaction policy is split across terminal routing,
  browser routing, and library commands.
- Tree-provider identity, transient prompt/action state, and focus are each
  represented several times and require synchronization.
- Protocol concepts have proliferated: the session frame has many independent
  sections and bespoke deltas, while enum ordinals, field names, validation,
  and replay are handwritten again in JavaScript.
- Component manifests currently feed one broad library target, so intended
  dependency direction is conventional rather than build-enforced.

## Staged roadmap

### 0. Establish complete seam oracles (complete)

- Add exhaustive C++ and JavaScript session-delta replay coverage. Perturb
  every semantic field and prove snapshot -> delta -> replay equality,
  including keymap and settings changes used by browser-local prediction.
- Add a generated or shared inventory check so a new protocol field cannot
  compile while lacking browser replay handling.
- Restore Windows adapter conformance and compile it in CI; the public
  `fileIdentity`, `setOwnerOnlyPermissions`, and `nativeHandle_` names currently
  disagree with Windows implementation names.
- Pin current terminal cells, hit results, focus transitions, prompt/picker
  lifecycles, and UI-tree presence before deleting parallel paths.

Completion: all semantic fields have cross-language replay oracles and both
required platforms compile their adapters. This phase should not redesign a
contract.

### 1. Route all interactions through one typed ingress

- Define semantic pointer targets and gestures in the existing typed client
  input seam. Move tab close, tree activation, external actions, multi-cursor,
  word selection, and scroll policy out of client command sequences.
- Keep every pointer action keyboard reachable through the authoritative
  command map.
- Keep fuzzy query/ranking and immediate text prediction local. Publish the
  complete candidate inventory and the authoritative activation/presence basis;
  reconcile predicted presentation through the same typed transition results.

Completion: TUI and web submit identical semantic input for identical user
intent; client pointer routers contain device translation but no editor command
sequencing or product policy.

This changes public input and wire contracts and requires a reviewed spec.

### 2. Separate semantic session state from grid presentation

- Remove grid rectangles, shell-node kinds, panes, scrollbars, terminal style,
  selection-scroll projections, and tree windows from the general session
  protocol.
- Move `ViewPresentationState`, `ShellState`, grid caches, and `present()` into
  an optional grid-presentation adapter that consumes semantic library state.
- Keep HTTP transport concerned only with attachment, authorization, replay,
  queues, and connection lifetime.

Completion: the general protocol carries no cells, rectangles, viewport
dimensions, terminal capabilities, or shell layout; terminal output and hit
behavior remain equivalent through focused oracles.

This changes public and wire contracts and requires a reviewed spec.

### 3. Make the UI tree the only whole-screen layout source

- Implement the grid interpreter over `UiSchema`, resolved node state,
  presence, and typed surface backing.
- Give each surface renderer only its solved native rectangle and backing
  data. Derive hit targets from the same solved tree.
- Delete `buildShellTree`, feature-specific `ShellLayoutRequest` geometry, and
  prompt/notice/external-modification shell layout branches.

Completion: every top-level TUI rectangle and hit target originates from one
UI-tree solve; adding or moving a node changes no client layout code.

This changes the grid presentation API and requires a reviewed spec.

### 4. Collapse repeated UI identities and transient paths

- Represent file, Git, and symbol providers as one tree widget bound to a typed
  `TreeProviderId`. Make `TreeModel` the sole active-provider authority.
- Treat schema, resolved node values, presence basis, and focus path as one
  versioned UI-frame contract, without forcing them to share update frequency.
- Express prompt controls and status actions as normal typed nodes/commands.
  Remove the unused footer-prompt surface, redundant prompt projections,
  dedicated status-action transport message, and any unused semantic hit API.
- Replace legacy focus projection plus special booleans with one typed focus
  path/context after compatibility removal is explicit.

Completion: adding a tree provider needs no client surface switch; transient
UI uses the normal node/action path; there is one authoritative focus
representation.

These are public and wire changes and require reviewed specs with compatibility
removal conditions.

### 5. Generate and reduce the protocol surface

- Define one authoritative typed wire manifest and generate field names, enum
  ordinals, validation, JavaScript decoding, and transactional replay.
- Retain bespoke incremental deltas only where payload or latency measurements
  justify them, especially document text and large tree data. Use generated
  replacement/change operations for ordinary sections.
- Split the oversized codec implementation by generated schema/mechanism, not
  by duplicating product behavior in transport or clients.
- Delete handwritten browser section inventories and feature-specific message
  kinds that ordinary command/input frames can express.

Completion: a semantic field or enum is declared once; C++ and JavaScript
consumers cannot drift; every remaining custom delta has a measured reason and
an atomic stale/malformed replay oracle.

This is a wire redesign and requires a reviewed spec.

### 6. Enforce dependency direction in the build

- After reverse dependencies are removed, split targets along actual ownership:
  semantic core, protocol codec, platform services, optional grid
  presentation, and transports/clients.
- Do not create wrapper-only targets. Each boundary must remove forbidden
  includes and make a reverse dependency fail at build time.

Completion: core has no protocol, transport, grid, terminal, or browser
dependency; protocol depends on semantic types; presentation adapters depend on
semantic state; clients depend only on the interfaces they consume.

## Concepts to preserve

- `Document` and piece-tree ownership with typed offsets.
- Dynamic command catalog, typed handlers, and host-supplied principals.
- `ValidatedSchema`, atomic presence/focus reconciliation, and
  `InteractionAuthority` as the single interaction writer.
- Shared `TreeModel`, semantic theme roles, and normalized filesystem/watcher
  adapters.
- Socket-free codecs separated from HTTP/WebSocket transport.
- Local prediction and retained presentation caches that reconcile against
  authoritative library state.
