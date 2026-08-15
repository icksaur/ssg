# UI-VM phase 7: the whole-screen node tree, over the built presence/mutation model

Status: in-flight design. Delete on merge. The durable residue lands in the
library's UI-model types, the snapshot/mutation channel, the conformance tests,
and any CONTRACT line — never in this file. Read `ui-vm.md` (the parent) first.

## North star: make responsive clients easy to write

The reason this tree exists is to make a new client cheap to write and inherently
responsive. The tree is a small web-inspired DOM — nodes with a few layout fields
(axis, size, inset, gap) and flags (presence), enough for TUI↔web parity, not a
re-implementation of CSS. The library owns the whole-screen tree, each node's
placement and visibility, and the authoritative data channels; a client owns only
its native layout solver and its pixels.

Two consequences bind every decision below:

- **Responsiveness lives in the client.** Fuzzy-find, text input, and retained-mode
  prediction of content happen in the client with no round trip. The library ships
  the *full* candidate list; the client narrows and filters it by the user's input
  locally and decides what is visible and clickable. Typing in the finder never
  waits on the host.
- **Library growth costs a client nothing in layout.** When the library adds a
  feature, a client needs zero layout changes and at most a new widget. Most new
  UI is new tree nodes the existing solver already lays out; only a widget with its
  own data needs (fuzzy-find, a future surface) adds a channel.

## What already exists, and what this spec must NOT reinvent

A first draft proposed a visibility flag on `UiNode`, a mutation patch, an
optimistic-apply identity, and a focus-reconciliation rule. All of that is
**already built and unit-tested in the library** and must be built upon, not
re-authored:

- `include/ssg/MutationPatch.h` — `MutationPatch` (a generation, a `PresenceBasis`,
  an `ApplicationId`, and an ordered `MutationOp{Show|Hide|Toggle, target}` list),
  and `applyMutationPatch`, the reference interpreter. It already enforces:
  generation match; no dangling target; two ops on one node rejected; hiding a
  container hides its subtree; showing a node implies showing its ancestors; hiding
  an ancestor while showing a descendant is a rejected contradiction; deterministic
  order-independent replay; a stale-basis prediction rejected.
- `PresenceConfig` (in `MutationPatch.h`) — the server-authoritative present flag
  per node at a generation and basis. No public mutator: presence changes only via
  `initial(schema, hidden)` or `applyMutationPatch` (which bumps the basis), so a
  stale patch cannot slip past the basis check.
- `ApplicationId` / `PresenceBasis` — the optimistic-prediction identity and
  pre-state identity, so a client can apply before the library confirms and a late
  acknowledgment names exactly the prediction it confirms or rejects.
- `include/ssg/KeyboardFocus.h` — `BaseFocus{Editor,Panel}` plus a transient LIFO
  capture stack; `effectiveTarget()` derives the single routing `FocusTarget`;
  `reconcile(PresenceConfig)` pops any capture whose node went absent; at most one
  prompt-backed capture.
- `include/ssg/InteractionState.h` — `UiInteractionState`, the canonical owner that
  holds one `ValidatedSchema`, derives initial presence, and whose `apply(patch)`
  updates presence and reconciles focus in ONE atomic call; `captureFocus` admits a
  capture only onto a present node of the schema.

None of these is on the wire or consumed by a client yet. Only the schema
(`UiTreeProtocol`) and the dynamic value state (`UiStateProtocol`) are published.
So the model of presence, mutation, optimistic prediction, and focus is DONE as
library logic; phase 7 is the work of (a) shaping the tree to span the whole
screen, (b) adding the one missing leaf primitive, and (c) putting the built model
on the wire and into both clients.

Because whole-screen tree migration and optimistic mutation reconciliation are two
separable high-risk designs, this spec is split into two independently-reviewed,
independently-merged subphases. 7A carries no optimistic mutation; 7B adds it.

## Subphase 7A: the whole-screen retained tree (authoritative presence, no prediction)

### 7A.1 `UiSchema` is a single root `UiNode`; `RegionRole` is removed

Today `UiSchema` is `{generation, vector<UiRegion>}` and `UiRegion` is
`{RegionRole, UiNode root}`. Placement belongs in tree structure, not an enum: a
node is at the top because it is the first child of the root column. `UiSchema`
becomes a generation plus one root `UiNode`. The whole screen is one tree; its
top-level shape is a column of `header`, a body row of `panel` and `content`, and
`footer`.

Well-known areas are named by **typed well-known identities**, not bare strings. A
closed enum of well-known node roles (header, footer, panel, content, and the view
surfaces below) maps to reserved `UiNodeId`s, and the schema validator checks, per
well-known identity, its **required existence, node kind, and ancestry** — not just
id uniqueness. A raw string id is not a placement contract; the typed identity plus
its validated position is. A native client that must hand a subtree to its own
toolkit keys off the typed identity; the TUI and web solve the whole tree.

`RegionRole` and `UiRegion` are deleted. Header and footer become subtrees of the
root, unchanged in content; their authoring (`ssg.chrome` → `ChromeDecode`), TUI
lowering (`ChromeLowering`), codec, and web interpreter carry over. A differential
oracle proves header/footer render byte-identically through the new root shape.

**Client capability covers each surface id, not just the `View` kind.** The client
profile today (`ClientUiProfile` / the web's `WEB_UI_PROFILE`) enumerates supported
widget kinds and region roles; supporting the generic `View` kind does not prove a
client renders every closed surface id. So the profile gains a **per-surface
declaration** — the set of surface ids the client implements — and the conformance
check rejects a schema naming a surface the profile lacks, the same loud early
failure as an unsupported widget kind. This is what lets a minimal client (say, one
without a git panel) refuse `gitstatus` explicitly rather than draw nothing.

### 7A.2 The opaque view leaf, and its authoritative backing section

The current `WidgetKind` vocabulary cannot express the tab-view, document, file
tree, git status, or finder results. This adds one leaf kind: an opaque **view**,
whose descriptor names a client-rendered surface by a value from a closed
surface-id enum. A view leaf carries no child widgets and no resolved text.

Ownership is split precisely, resolving `ui-vm.md`'s library-authority rule against
"the client renders it":

- The **library** owns the view leaf's slot, size, and presence in the tree, AND
  the surface's **semantic input**, delivered through an existing authoritative
  snapshot section. Each surface id is mapped, in this spec and enforced by a test,
  to the section that backs it:
  - `tabview` ← the tab-list AND the active document body. The document is NOT a
    separate peer; it lives inside `tabview`. The client renders the tab strip and
    the document text from the authoritative document/tab sections the library
    publishes. Scroll position is **client-local native state**, not a published
    authoritative value: the library publishes document semantics (text, caret,
    total line count); the client owns its own viewport offset and computes its
    medium's scrollbar from that local offset against the published line count.
    Scrollbar geometry is never on the wire.
  - `filetree` ← the filesystem tree-provider snapshot.
  - `gitstatus` ← the git tree-provider snapshot.
  - `findresults` ← the finder's FULL candidate universe, `PaletteViewState`
    (`std::vector<PaletteCandidate>`, `include/ssg/PaletteSearcher.h`), already
    published in the session snapshot as `palette` (`include/ssg/session_snapshot.h`).
    This is the authoritative full list. `PaletteReport { rows }` is the
    host-side FILTERED/WINDOWED result and is explicitly NOT the channel — the
    client computes its own filtered view. Ownership splits three
    ways here, per the north star: the **library** owns the node's placement/size/
    presence in the tree AND the authoritative `PaletteViewState` candidate universe
    on that channel; the **client** owns the fuzzy-find itself — it narrows the full
    list by the user's live text input locally (its own `PaletteReport`-equivalent),
    with no round trip, and decides which rows are visible and clickable. So
    "contents of the finder" (the candidate universe) is library-authoritative;
    "what matches the prompt right now" is a client concern computed in the client.
    This is the exemplar of a widget that needs a special data channel beyond the tree.
- The **client** owns only the pixels/DOM that present that section's data in its
  medium (and, for `findresults`, the local match/filter over the library's list).

A view leaf naming a surface a client lacks is a loud conformance failure at
schema check, never a silent empty render.

Content peers are exactly `tabview` and `findresults`, mutually exclusive: at most
one present. There is no separate `document` surface id.

**Fuzzy-find is local execution of a shared contract, not a second behavior path.**
Letting each client narrow the candidate list locally would create two matching
implementations that could rank differently — the divergence the global one-behavior
rule forbids. The resolution: the **match-and-order contract is shared, deterministic,
and specified as an algorithm**, even though each client executes it locally for
responsiveness. The algorithm is the one `PaletteSearcher` already implements, stated
here as the contract so the oracle checks against a specification, not a copied
expected blob:

- **Match:** a candidate matches a query iff the query is a case-folded subsequence
  of the candidate's `label` OR its `id`. A non-subsequence is no match and is
  dropped. **Units and folding are pinned cross-language:** iteration and length are
  over raw **UTF-8 bytes** (the web matcher iterates the same UTF-8 byte sequence,
  NOT UTF-16 code units); folding is **ASCII-only** (`A`–`Z` → `a`–`z`, nothing
  else — not locale `tolower`), so a non-ASCII byte folds to itself and compares by
  byte. The existing C++ `std::tolower` call becomes an explicit ASCII fold during
  implementation (it is locale-sensitive today). The corpus includes non-ASCII cases
  proving the two clients agree byte-for-byte.
- **Score (higher is better), computed per matched position (over UTF-8 bytes):** a
  base per-matched-byte score, a word-boundary bonus when the matched byte begins a
  word (index 0 or preceded by `/ _ - .`), a contiguity bonus when adjacent to the
  previous match, and an exact-case bonus when the raw (unfolded) byte matches
  exactly; then subtract `min(candidate byte length, length cap)` (shorter candidates
  preferred). A candidate's score is the max of its `label` and `id` scores. The
  concrete weights and the length cap are named constants defined in
  `PaletteSearcher`; the client hardcodes none — it reads their values from the
  library-published matcher parameters (below), and the corpus oracle pins the
  resulting behavior.
- **Order:** stable sort by descending score; ties broken by `label` ascending, then
  `id` ascending. (Stability makes the input order the final tie-break.)

The library exposes this as a pure reference function over `PaletteViewState` + a
query. **One enforceable source for the weights, no duplication.** Rather than
hardcode the scoring weights and length cap in each client (which the corpus oracle
would only catch as drift *after* the fact), the library **publishes the matcher
parameters** as **typed fields of `PaletteViewState`** (the named weight/cap
constants), so they arrive atomically with the candidate universe on the same
channel — a client can never match against candidates without the parameters that
score them. The web matcher consumes the published parameters and hardcodes none;
the C++ constants in `PaletteSearcher` are the single source of truth. The
`PaletteViewState` codec (in `src/Protocol.cpp`, where the `palette` section is
encoded) carries the parameter fields with round-trip fidelity and malformed-field
rejection, pinned by `tests/test_palette_protocol.cpp`. The **cross-client corpus
oracle** is an *algorithm* oracle, not a golden: its expected results are derived
from the rules above (independently re-derivable by hand for the fixed corpus), and
both the C++ reference and the web local matcher must reproduce them identically. A
client may run its own matcher for speed; it is conformant only if it matches. The
responsiveness is local; the behavior is one specified contract with one parameter
source.

### 7A.3 Presence published as the single authority; both clients render a hidden node unlaid

There must be exactly ONE presence authority. Phase 6 already ships
`UiNodeState.present` in the dynamic value-state section (always `true` today);
`PresenceConfig` is a second, basis-bearing authority. This spec removes
`present` from the value-state section and publishes presence as its own
generation-and-basis-stamped section derived from `PresenceConfig`. Value state
carries resolved values only; presence carries visibility and its basis. (Rejected
alternative: folding basis into the value-state section — presence mutates on a
different cadence than value resolution and carries a basis the value state has no
use for, so a separate section keeps each channel single-purpose.)

The presence section's correspondence to the schema is validated before a client
interprets it, not merely round-tripped: exactly one presence record per validated
schema node, the section's generation equal to the schema's, and the record id-set
equal to the schema's node-id set — a missing, duplicate, or foreign-node record is
a rejected frame. Round-trip fidelity alone cannot catch those; the id-set/generation
agreement is the contract.

A client caches the schema (superset of every node that could show) and consults
the presence section to lay out only present nodes; a hidden node stays in the
retained tree and is not laid out. The panel and content semantics become sibling
presence:

- `panel` contains `filetree` and `gitstatus` as peer view leaves; at most one
  present, and the panel subtree itself may be absent. This re-expresses today's
  `ShellState::showPanelProvider` (`src/ShellState.cpp`) and the header
  `path`/`branch` → `panel.show_files`/`panel.show_git_status` mapping
  (`src/runtime/snapshot.cpp`).
- `content` contains `tabview` and `findresults` as peers; exactly one present.

Effective vs local presence is defined explicitly (the parent/child rule already in
`applyMutationPatch`): a node lays out iff it and every ancestor are present.

**Remembered panel provider.** Hiding `panel` sets both `filetree` and `gitstatus`
absent, so presence alone cannot say which provider to restore on re-show. The
authoritative owner of the remembered selection is the **library runtime** (the
successor to today's `ShellState` provider index), a small "last-active panel
provider" hint held beside the tree, NOT in the tree and NOT in presence. `panel.toggle`-on
deterministically produces a patch that shows `panel` and the remembered child
together; `panel.show_files`/`panel.show_git_status` update the hint and produce a
patch showing that child. In 7A, where presence-changing commands round-trip, the
hint provides **authoritative restoration** — re-showing lands on the right
provider deterministically, but not instantly; the round trip still occurs.
Instant, state-dependent re-show (predicting the show-panel+remembered-child patch
client-side from the retained hint) is a 7B prediction, deferred there with the
rest of the optimistic path. The hide/show and provider-switch sequences are
pinned as independent tests naming the hint as the authority, not left implicit.

In 7A the **library is the sole presence authority**: a presence-changing command
round-trips, the library applies the patch through `UiInteractionState`, and
republishes presence. No client-side prediction yet. This keeps 7A a pure
retained-tree-plus-authoritative-presence step, reviewable without the
reconciliation protocol.

### 7A.4 The concrete canonical tree, and alignment resolved (omitted)

The canonical whole-screen tree is committed here, not to implementation:

```
root                 UiNode, container, axis=Column
├─ header            container, axis=Row     (size Exact, header row height)
├─ body              container, axis=Row      (size Flex — fills the middle)
│  ├─ panel          container, axis=Column   (size Exact, sidebar width)
│  │  ├─ filetree    view leaf (surface filetree)   (size Flex)
│  │  └─ gitstatus   view leaf (surface gitstatus)  (size Flex)
│  └─ content        container, axis=Column   (size Flex — fills remaining width)
│     ├─ tabview     view leaf (surface tabview)     (size Flex)
│     └─ findresults view leaf (surface findresults) (size Flex)
└─ footer            container, axis=Row     (size Exact, footer row height)
```

**Sizing contract, including opaque views.** An opaque view leaf has no content to
hug, so `Auto` is meaningless for it: a view leaf must be sized `Exact` or `Flex`,
never `Auto`, and the schema validator rejects an `Auto` view leaf. The extent unit
is the abstract layout unit `Size` already uses (the TUI maps it to cells, the web
to `ch`), so `Exact` is medium-agnostic. Concretely: the four view leaves are all
`Flex` (each fills its container's slot); the `panel` container is `Exact` (a
library-owned sidebar-width constant in abstract units — the successor to today's
sidebar column count), so the body row gives `panel` its width and `content` (Flex)
the rest; `header`/`footer` are `Exact` at a library-owned row-height constant (one
row tall today). These extents are named constants held in code, not literals in
this spec. **The
canonical tree uses only `Exact` and `Flex`; it does not use `Auto`.** Whether the
generic solver ever resolves a container `Auto` (recursive intrinsic measurement)
is therefore out of scope for phase 7 — the solver may keep rejecting `Auto`, and
this spec introduces no `Auto` node and adds no intrinsic-measurement requirement.

**Presence is not fit — overflow is client-local, `Exact` unchanged.** Presence (a
library semantic: is this node shown at all) is distinct from fit (a client-local
question: does the shown tree fit the actual viewport). This spec does NOT
redefine `Size::Exact`: it keeps its existing contract (reserve exactly that
extent). When the shown tree's main-axis total exceeds the viewport, the client
resolves it **in its native medium, never by changing presence and never by a
round trip**: a present node that cannot fit is clipped or scrolled the way that
medium already handles an over-tall document or an over-wide row (a terminal
truncates, a browser scrolls). Overflow presentation is deliberately NOT a
cross-client pixel-parity contract — it is presentation, which the project's global
rules assign to each client — so there is no id-specific "shrink this before that"
rule in the model and no shrinkable size kind. The only cross-client contract here
is the invariant that overflow changes nothing the library publishes: presence,
the tree, and every node's `Size` are identical whether or not the viewport can fit
them. A parity oracle pins *that* (a narrow viewport yields the same published
schema + presence as a wide one), not the medium-specific clip/scroll pixels.

**The grid client needs a non-rejecting logical layout to clip.** Today's grid
solver returns no layout when `Exact` children exceed the bounds — it rejects
rather than truncating. The fit-vs-presence separation requires that the TUI can
still show an over-bounds tree, so 7A adds a **logical layout pass** with a defined
nonnegative allocation rule. Along a container's main axis, computed within the
container's **post-inset content extent** (the frame minus its inset; if the inset
exceeds the frame on either axis, that content extent clamps to zero, never
negative): each `Exact` child gets its full extent; the remainder for `Flex`
children is `max(0, content extent − sum(Exact))` — clamped at zero, never negative.
`Size::flex()` carries no weight and the model adds none, so the remainder is split
**equally among the present `Flex` children** (matching the current grid solver; the
web must divide equally too, not treat a `Flex` extent as a grow weight). In the
canonical tree at most one `Flex` sibling is ever present per container, so equal
division is unambiguous there. Child coordinates are the running cumulative offset of
preceding children's extents, so a child's start can exceed the viewport; the
renderer then **clips** the logical layout to the terminal rect. Nothing is scaled
and no `Exact` extent is reduced — over-bounds coordinates are simply clipped.
Presence and the schema are untouched. (Placed in build step 2, with the canonical
tree.)

Every arrangement this tree needs is expressible with axis + size (`Flex` fills,
`Exact` for the row heights and the sidebar width) + the existing flex-spacer idiom
the header/footer already use for left/center/right packing. No node requires a
child positioned at a cross-axis start/center/end a spacer cannot produce.
Therefore **the alignment primitive is conclusively omitted**: `UiContainer` keeps
axis, inset, gap, and gains no alignment field, and the parent spec's
"start/center/end" promise is dropped. A future need is a separate, spec'd addition
with its own parity oracle — not carried speculatively now.

### 7A build order (each step: build, focused suite + web oracle, contracts true, one review; visual signoff before any layout-changing commit)

1. Add the view leaf kind + closed surface-id enum + conformance rejection, the
   view-leaf sizing rule (a view leaf's `Size` must be `Exact` or `Flex`, never
   `Auto`; `validateUiSchema` rejects an `Auto` view leaf), AND the per-surface
   client-profile declaration + unsupported-surface rejection (the profile
   enumerates the surface ids the client implements; a schema naming an unsupported
   surface is refused like an unsupported widget kind). No canonical-tree change
   yet — this is the additive primitive the canonical tree (step 2) needs to exist
   first. Map each surface id to its authoritative backing section, enforced by a
   test (`tests/test_ui_view_surface.cpp`), and pin unsupported-surface rejection on
   both clients. Files: `include/ssg/UiWidget.h` (WidgetKind + the surface-id enum),
   `include/ssg/UiTree.h` + `src/UiTree.cpp` (the view-leaf validation rule),
   both clients. Files: `include/ssg/UiWidget.h` (WidgetKind + the surface-id enum),
   `include/ssg/UiTree.h` + `src/UiTree.cpp` (the view-leaf validation rule),
   `include/ssg/UiProfile.h` (`ClientUiProfile` gains a per-surface set) +
   `tests/test_ui_profile.cpp`, `apps/web/reconcile.mjs`'s `WEB_UI_PROFILE` +
   `firstUnsupportedPrimitive`, `include/ssg/UiTreeProtocol.h` +
   `src/UiTreeProtocol.cpp`, `apps/web/client.mjs` (render).
2. Collapse `UiSchema` to a single root `UiNode`; delete `RegionRole`/`UiRegion`;
   construct and validate the concrete canonical tree (7A.4) — now constructible
   because the view leaves exist — with typed well-known identities +
   existence/kind/ancestry validation in `validateUiSchema`. Header/footer
   differential oracle stays byte-identical. Also add the non-rejecting logical
   layout pass + clip (the fit-vs-presence overflow rule above): `src/Layout.cpp`
   (nonnegative logical solve, `Flex` remainder clamped at zero), `src/Renderer.cpp`
   (clip logical layout to the viewport), with an undersized-viewport oracle
   (`tests/test_layout_overflow.cpp`) proving a viewport smaller than the summed
   fixed extents lays out logically and clips without dropping a present node.
   Files: `include/ssg/UiTree.h`
   (UiSchema → root node; UiRegion removed), `include/ssg/RegionRoot.h` (deleted),
   `include/ssg/UiTreeProtocol.h` + `src/UiTreeProtocol.cpp`, `src/ChromeLowering.cpp`,
   `src/Layout.cpp`, `src/Renderer.cpp`,
   `src/ChromeDecode.cpp`, `apps/web/reconcile.mjs`, `validateUiSchema`
   (in `src/UiTree.cpp` — the `UiSchemaValidation`/`ValidatedSchema` path), and
   `tests/test_ui_tree.cpp` / the chrome-lowering differential oracle.
3. Publish presence as its own section derived from `PresenceConfig`; remove
   `present` from `UiNodeState`. Both clients lay out only present nodes. Oracle:
   wire round-trip AND schema-correspondence (one record per node, generation and
   id-set agreement, rejection of missing/duplicate/foreign records),
   `tests/test_presence_protocol.cpp`. Files: new
   `include/ssg/PresenceProtocol.h` + `src/PresenceProtocol.cpp`,
   `include/ssg/UiNodeState.h` (drop `present`), `src/UiStateProtocol.cpp`,
   `include/ssg/session_snapshot.h` + `src/runtime/snapshot.cpp` (add the section),
   `apps/web/reconcile.mjs` + `apps/web/client.mjs`.
4. Populate `panel`(filetree⊕gitstatus) and `content`(tabview⊕findresults); back the
   view leaves with the existing bespoke renderers; carry the "last-active panel
   provider" hint in the runtime; prove the TUI lays panel/content out where it does
   today. Oracles: TUI↔web view-surface placement parity
   (`tests/test_view_placement.cpp`) and the retained hide/show/provider-switch
   sequence pinned against the hint (`tests/test_panel_presence_sequences.cpp`). For
   `findresults`, publish the `PaletteViewState` candidate universe AND the typed
   matcher-parameter fields on its channel and have the client match/order locally
   against the SHARED reference contract; pin the matching/ordering with a
   cross-client corpus oracle (`tests/test_fuzzy_corpus.cpp` + a mirrored
   `tests/web/test_fuzzy_corpus.mjs`) so the library reference and the web matcher
   produce identical match sets and orderings on a fixed corpus, and pin the
   parameter-field codec round-trip + malformed-field rejection with
   `tests/test_palette_protocol.cpp`.
   Files: `include/ssg/ShellState.h` + `src/ShellState.cpp` (provider index →
   runtime hint), `src/ChromeLowering.cpp` (general lowering),
   `include/ssg/PaletteSearcher.h` + `src/PaletteSearcher.cpp` (expose the pure
   reference matcher + the named parameter constants as typed `PaletteViewState`
   fields), `src/Protocol.cpp` (encode/decode the parameter fields in the `palette`
   section), `src/runtime/snapshot.cpp`, `src/runtime/presentation.cpp`,
   `apps/web/reconcile.mjs` + `apps/web/client.mjs`.
5. Route today's presence-changing commands through a single atomic server-side
   **command transition** and republish presence — still round-tripping, no
   prediction. `UiInteractionState::apply` reconciles focus captures but does NOT
   perform `panel.toggle`'s `BaseFocus` move/restore, and finder-open needs prompt
   capture + picker identity + candidate initialization + presence to move together.
   So 7A adds a runtime `CommandTransition` type (new `include/ssg/CommandTransition.h`
   + `src/CommandTransition.cpp`). It is NOT a bag of optional effects — that would
   admit invalid combinations (e.g. a capture push with no prompt seed). It is a
   closed set of **named, validly-constructed transitions** — `togglePanel`,
   `showPanelProvider(provider)`, `openFinder`, `closeFinder` — each a factory that
   yields a well-formed transition or nothing, so an invalid combination is
   unconstructable. Application is **two-phase**: a `preflight` computes the entire
   prospective post-state (presence via `applyMutationPatch`, the `BaseFocus`/
   capture change, the picker/prompt seed) and returns either a committed bundle or a
   rejection, mutating NO owned subsystem; `commit` swaps the validated bundle in as
   one operation. **Commit-boundary ownership:** these pieces are not swapped as
   several loose runtime members. The runtime holds ONE aggregate interaction owner —
   the `UiInteractionState` (presence + focus), the panel hint, and the picker/prompt
   state together — and `preflight` produces a fully-formed replacement of that
   aggregate. `commit` is a single move of the new aggregate into the runtime under
   its existing single-threaded update boundary, and is **infallible after preflight**
   (all validation happened there); no intermediate state is observable. Because
   preflight owns validation and touches nothing, a rejected transition leaves
   presence, focus (`BaseFocus` + capture stack), picker identity, candidate state,
   prompt state, and the panel hint exactly as before — no field is half-applied. It
   composes `UiInteractionState::apply` (presence+capture) with the base-focus and
   picker seeds inside that aggregate; it is the successor to
   `ShellState::togglePanel`/`showPanelProvider` and the finder open/close. Oracles:
   a focus/prompt parity test (`tests/test_command_transition_focus.cpp`) proving
   `panel.toggle` restores prior `BaseFocus` and finder open/close leaves focus and
   presence exactly as today, AND a rejection test proving a refused transition
   mutates nothing. Files: `include/ssg/CommandTransition.h` +
   `src/CommandTransition.cpp`, `src/runtime/presentation.cpp` (command dispatch),
   `include/ssg/ShellState.h` + `src/ShellState.cpp`, `src/runtime/snapshot.cpp`.

## Subphase 7B: command-carried optimistic mutation (prediction + reconciliation)

7B adds the client-side fast path, and only after 7A ships. It is the higher-risk
design and gets its own review with the ownership and acknowledgment protocol
resolved. Its non-negotiables, from the built model and the review:

### 7B.1 Command classification is a sum type, not synchronized metadata

A command's outcome must make **client-applicable mutation XOR server dispatch**
unrepresentable-when-mixed, by construction — not two booleans a maintainer keeps
in sync. A command registration/outcome is a sum type: either it yields a
`MutationPatch` a client may predict, or it dispatches to the server, never a
silent both. A command whose full effect is not exactly a presence patch cannot be
a predicted mutation.

### 7B.2 A predicted command must model its WHOLE effect or round-trip

`panel.toggle` does not only change presence: it moves and restores `BaseFocus`
(`src/ShellState.cpp` remembers focus-before-panel). Opening the finder pushes a
prompt-backed focus capture and initializes prompt and candidate state. Neither is
"mere visibility." So a command qualifies for client prediction only if its entire
effect is expressible as a `MutationPatch` (whose apply, via `UiInteractionState`,
already reconciles focus) — and the focus/prompt consequences must be part of that
modeled, atomic effect. Any effect beyond what `UiInteractionState.apply` produces
from a patch (e.g. seeding finder candidate state) forces a round-trip. This is the
crux 7B must get right; the finder in particular likely round-trips its open.

### 7B.3 Reconciliation protocol, not fire-and-forget

A client applies a predicted `MutationPatch` against its current basis, tagging it
with an `ApplicationId`, and sends it. The library applies authoritatively and
returns an acknowledgment (confirm or reject) naming the `ApplicationId`. The spec
must define, and pin with an independent oracle: rejection recovery (the client
rolls back to authoritative presence), generation-change handling (a schema replace
invalidates in-flight predictions), ordering of multiple in-flight predictions
(basis chaining), and a stale prediction rejected by basis. The built
`PresenceBasis`/`ApplicationId` types exist for exactly this; 7B wires the protocol
around them.

## Resolved by the user (folded above)

- `document` is subsumed by `tabview` (tab-list + document body + scrollbar); content
  peers are exactly `tabview` ⊕ `findresults`.
- Presence is its own basis-stamped section; `UiNodeState.present` is removed — one
  authority.
- The remembered panel provider is a runtime "last-active" hint (not in the tree,
  not in presence); it exists for client responsiveness on re-show.
- `findresults`: the library owns the node placement and the FULL candidate universe
  (`PaletteViewState`, published as `palette` in the session snapshot) on a data
  channel; the client owns the fuzzy-find (local narrowing by user input) and what is
  visible/clickable. `PaletteReport` is the host-side filtered result, not the channel.
- Alignment is omitted: the concrete canonical tree (7A.4) expresses every
  arrangement with axis + size + the flex-spacer idiom; `UiContainer` gains no
  alignment field.

## Open questions still for review

- 7B.2: which of today's presence-changing commands can be pure predicted patches,
  and which (finder open, with candidate seeding) must round-trip? Enumerate them.
- 7B: is the client→library predicted-patch send acknowledged synchronously enough
  that a single-flight (one prediction outstanding) model suffices, avoiding
  multi-prediction basis chaining in the first cut?
