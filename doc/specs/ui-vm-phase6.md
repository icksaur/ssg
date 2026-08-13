# UI-VM phase 6: the web consumes the published model

Scaffolding for phase 6 of `doc/specs/ui-vm.md`. Deleted when phase 6 merges; its
plan and file list live on in git, its durable residue in types, tests, and any
CONTRACT line. Read `ui-vm.md` first — this refines only phase 6.

## Goal

Make the web client render the header/footer chrome from the library-published
model instead of not rendering it at all, and land the dynamic-node-state half of
the interaction-state wire publication phase 4 deferred (the resolved values a
second client needs; the mutation-patch wire and runtime interaction ownership stay
deferred to phase 7 — see the boundary decision below). The web becomes a
first-class client of the same schema the TUI lowers: it interprets the region tree
into DOM, colors it from the theme roles, shows the resolved widget values, and
dispatches a widget's command on click.

## What forces a design here

The published `ui` schema is immutable structure carrying value **sources**
(`ValueSource`: a literal or a provider id), never resolved values. The TUI
resolves those sources server-side at lowering time, through a resolver built from
the live status-field registry, and paints cells. The web has no status registry
and cannot resolve a provider id like `path` into `~/proj`. So the schema alone is
not renderable by a second client: the missing half is the generation-scoped
**dynamic node state** — the resolved value, accessible label, and click command
per node, plus each node's presence — which is precisely the state phase 4 named
and deferred. Phase 6 has the first real consumer of it, so phase 6 publishes it.

This is the crux: the dynamic-state publication is not speculative rail. It is the
data without which the web cannot draw a single composed field.

## Design decisions

### Dynamic node state is a new semantic section, resolved server-side

Add a semantic section carrying, per schema node id at a generation, the state a
client needs to present that node without any registry of its own. Presence and
renderability are distinct and both are needed, so the per-node record separates
them:

- Every node — container and leaf alike — carries an authoritative `present`
  flag. A container has no value; its presence alone governs whether its subtree
  is drawn.
- A leaf additionally carries optional semantic leaf state: the resolved `value`
  text, the accessible `label`, an optional click `command`, and — for a checkbox
  — a `checked` boolean carried as its own field, never folded into `value`. The
  published content is SEMANTIC: the checkbox's checked state, not a TUI-formatted
  glyph; the resolved text, not a cell run. A native client renders these in its
  own idiom.

Concretely: `UiNodeState { UiNodeId id; bool present; std::optional<UiLeafState>
leaf; }` where `UiLeafState { std::string value; std::string label;
std::optional<std::string> command; std::optional<bool> checked; }`, and the
section `UiStateSection { Generation generation; std::vector<UiNodeState> nodes; }`.

The runtime builds it by walking the current schema: every node is marked present
(see the focus/ownership decision below), and each leaf's `ValueSource` is resolved
through the same status-field-derived resolver the TUI lowering uses, per the
per-kind rules below — a Label/Field leaf whose resolution is empty gets no leaf
state (matching the built-in and TUI drop), while a checkbox always gets leaf state.
It emits resolved tuples as semantic data, not grid cells, and the resolution is
GEOMETRY-INDEPENDENT: it applies no width-based rank collapse or fit (those are a
grid client's presentation concern), so the same state serves every client size.

The section names the `Generation` of the `ui` schema it resolves, and stands in
exact correspondence with that schema: exactly one `UiNodeState` per schema node,
no record for an id absent from the schema, and record shape agreeing with node
kind — a container node's record carries no leaf state, a leaf node's record carries
leaf state or `nullopt` per the resolution rules below. A client validates this
correspondence (generation equal, ids one-to-one, shape agreeing) before
interpreting; a mismatch means the state and schema are from different frames, so
the client waits for a consistent pair rather than binding values to the wrong or a
missing node. The codec rejects a section with no generation, a duplicate node id,
or a malformed optional-field shape.

Per-kind resolution (the rules the server applies and the oracle pins):

- **Label, Field**: leaf state iff BOTH the resolved value (literal or provider) and
  the accessible label are non-empty; an empty value OR an empty label yields `leaf =
  nullopt` — the semantic form of the TUI/built-in drop, which drops on
  `label.empty() || value.empty()`. `label` and optional `command` are what
  `resolveWidget` produces (the accessible label and the click command), with an
  explicit descriptor `command` overriding a provider-inherited one.
- **Checkbox**: always leaf state (it renders its box regardless of caption); the
  `checked` boolean is resolved from the checkbox's checked source, and `value` is the
  optional caption (possibly empty). Its semantic `label` and `command` are NOT what
  the current `resolveWidget` yields for a checkbox: today a literal-caption checkbox's
  label is the TUI-composed glyph string (`r.label = r.content = checkboxText(...)`).
  Phase 6 factors a semantic checkbox resolution ahead of that glyph composition — a
  literal caption's `label` equals its caption text, a provider caption's `label` is
  the caption provider's accessible label, `value` is the caption, and `command`
  follows the same descriptor-overrides-inherited rule — so the published fields are
  semantic and the TUI's glyph string is never published. A checkbox is never dropped
  on empty value.
- **Spacer, Container**: `leaf = nullopt` always — a spacer is a gap the client
  draws from the schema's `Spacer` kind, a container is structure; neither resolves
  a value.

`present` is server-authoritative presence, distinct from this renderability: in
phase 6 every node is present (see the ownership decision below), and the empty-value
drop is expressed as `leaf = nullopt` on a still-present node, not as `present =
false`.

The section is SEMANTIC (a `SessionSnapshotSections` member, never a presentation
projection), so a dimensionless or native client receives it, preserving the
presentation-optional snapshot contract. It is published dual-path (whole-value
delta, `UiStateSectionDelta` mirroring `ThemeSectionDelta`), with a wire round-trip
oracle.

The schema section keeps carrying sources, not resolved values: structure is
immutable within a generation, and the sources are what make it so. Resolved values
change every frame (a branch name, a cursor position) and belong in the mutable
dynamic-state section, not the structural schema. Keeping them separate is what lets
the schema be a stable, cacheable, generation-stamped structure while values stream.

### Presence is derived from the validated schema; UiInteractionState ownership defers to phase 7

Phase 6 does NOT instantiate a runtime `UiInteractionState`. Two reasons. First,
today's schema contains only composed chrome nodes, while `UiInteractionState`'s
focus capture requires a schema node for the focused surface — and today's prompt
focus has no schema node (prompts become ephemeral schema nodes only in phase 7).
Owning the state now would model a focus the schema cannot represent. Second, for
the static init.lua chrome every node is present, so presence needs no mutation
authority yet.

So phase 6 derives the published presence directly from the validated schema: every
node present. Effective focus stays exactly as it is published today (the
`FocusTarget` field), with its current authority; phase 6 does not route focus
through a canonical owner and does not widen the focus wire. The `UiInteractionState`
runtime ownership, the capture stack over ephemeral prompt nodes, and the presence
mutation it authorizes all land in phase 7, where ephemeral nodes exist to carry
them.

### Canonical focus stays the existing effective-focus field

Effective focus (`FocusTarget`) is already a published semantic field the web reads
to gate local echo. Phase 6 leaves it unchanged in wire, meaning, and authority.

### The mutation-patch wire and interaction ownership defer to phase 7 — the amended boundary

`applyMutationPatch` (the reference interpreter) exists and is pinned, but no
command in the runtime PRODUCES a `MutationPatch` and nothing consumes one over the
wire. Publishing the reconciled optimistic patch protocol now — application ids,
acknowledgments, client-side prediction — would be rail with no traffic, the
speculative wire the phase-4 boundary review rejected. Phase 6 therefore publishes
server-authoritative presence as part of the dynamic-state delta (the client
applies what the server resolved, no prediction), and defers BOTH the mutation-patch
wire AND runtime `UiInteractionState` ownership to phase 7, where ephemeral ownership
introduces the commands that emit patches, the ephemeral schema nodes that carry
prompt focus, and the client-side reconciliation that consumes them.

This narrows what `ui-vm.md`'s phase 6 text promises (it currently lists mutation
vocabulary publication and runtime interaction ownership under phase 6). That parent
boundary is amended in the same change: phase 6 publishes the resolved dynamic node
state and the web interpreter; the mutation vocabulary wire, the `UiInteractionState`
runtime ownership, and canonical-focus routing move to phase 7. The amendment lands
in `ui-vm.md` before phase 6 implementation begins, so the parent spec and this one
agree.

### The web DOM interpreter

Replace the web client's absent/ad-hoc chrome with a DOM interpreter over the two
sections. For each region in the schema it builds a container positioned by the
region role, and for each leaf a DOM element chosen by widget kind, textual content
from the dynamic state's resolved value, color from the theme role mapped to a CSS
custom property, `display` gated by the node's `present` flag, and — when the
dynamic state gives the node a command — a click handler that dispatches that
command over the existing input channel. Geometry is the browser's (the web is a
native-layout client, not a grid client); only the semantic structure, values,
roles, and triggers are the library's. The interpreter reconciles against node id,
so a delta that changes one node's value or presence touches only that node.

### Missing-widget detection is a client-profile rejection

The web client build declares the `ClientUiProfile` it implements (the widget kinds
and region roles its interpreter can draw). Before interpreting a schema it checks
every widget kind and region role against that profile; a primitive the build does
not implement is a loud, tested rejection naming the gap, not a silently dropped
element. This is the phase-2 profile earning its keep at its first real client.
Whether the profile is additionally enforced host-side at attach (the host holding
the served build's profile) is the attach-seam concern ui-vm.md assigns to a later
phase; phase 6 lands the client-side interpreter check and its test.

## Plan

Each sub-step is independently gated and reviewed; the wire and behavior changes are
each pinned by an oracle before a client consumes them.

1. **Resolve the dynamic node state server-side, pinned to the TUI first.** Before
   factoring anything, write the parity oracle against the CURRENT TUI lowering, per
   the per-kind rules above, at a NON-COLLAPSING width. The semantic state is
   geometry-independent, but the TUI's emit/drop at narrow widths also reflects
   rank-collapse and fit; so the oracle compares at a width wide enough that no widget
   collapses (or against the TUI's pre-fit resolution), isolating resolution from
   layout. For a corpus of composed regions at such a width, a **Label or Field**
   entry must agree with what `lowerUiChromeRegion` emits as an `AccessibilityNode` —
   same `value`, `label`, `command`, and the same present-leaf-vs-`nullopt` decision
   as the TUI's emit-vs-drop (which drops on empty value or empty label). A
   **Checkbox** cannot be compared field-for-field against a TUI node: the
   `AccessibilityNode` exposes    only the composed glyph string, not the semantic `checked` bool, the bare caption,
   or the semantic label. So for a checkbox the oracle asserts only that a node is
   emitted exactly when a leaf state is produced (never dropped), and asserts the
   semantic `checked`, `value`, `label`, and `command` (including the
   descriptor-overrides-inherited precedence) against an INDEPENDENT expectation
   derived from the checkbox's sources, not from the TUI node. Writing the comparison first keeps it
   independent of the resolution code it will later share: the two paths share
   `resolveWidget` but assemble differently (TUI → grid nodes, this → semantic
   `UiNodeState`), and the oracle asserts they still agree on what `resolveWidget`
   cannot by itself decide — the walk, the per-kind drop, and the checkbox derivation.
   Then factor the resolver so the runtime can produce a `std::vector<UiNodeState>`
   from the schema and the status-field resolver. For a checkbox this factoring is not
   a pure reuse: `resolveWidget` today folds the composed glyph into the checkbox label
   (`r.label = r.content`), so the factoring must surface the semantic caption, checked
   bool, and semantic label AHEAD of glyph composition, with the TUI's glyph path built
   on top of the same semantic pieces so the two do not diverge. Types land in
   `include/ssg/UiNodeState.h` (`UiLeafState`, `UiNodeState`, `UiStateSection`);
   resolution in the runtime snapshot path and/or `ChromeLowering`. No wire, no client
   yet.

2. **Publish the dynamic-state section on the channel.** Add `UiStateSection uiState`
   to `SessionSnapshotSections` and `UiStateSectionDelta` (whole-value replacement,
   mirroring `ThemeSectionDelta`); thread it through `operator==`, `SessionDelta`
   ctor/member/accessor, `deriveDelta`, `replay`, `decodeWire`, and the `ProtocolCodec`
   snapshot and delta encode/decode in `Protocol.cpp`; add the wire codec
   (`encodeUiState`/`decodeUiState`, alongside `UiTreeProtocol` or a sibling) with a
   round-trip oracle asserting a corpus survives encode→decode, and a rejection oracle
   over the malformed cases the codec OWNS (checkable from the section alone, without
   the schema): a section with no generation, a duplicate node id, and a leaf record
   whose field TYPES are malformed (a `checked` that is not a boolean, a `command` or
   `value` that is not a string). The schema-relative checks — node-id set matching the
   schema and container/leaf shape agreement — are NOT codec-ownable (a `UiNodeState`
   carries no container/leaf discriminator; agreement is defined only against the
   schema), so they are client-side reconciliation checks tested in the web node tests,
   not codec rejections. Populate `sections.uiState` in the runtime alongside the `ui`
   schema, stamped with the same `chromeGeneration`. Files: `include/ssg/UiNodeState.h`,
   the new codec `.h/.cpp`, `session_snapshot.{h,cpp}`, `Protocol.cpp`,
   `src/runtime/snapshot.cpp`, `tests/test_protocol.cpp`, the codec oracle.

3. **Reject an unrenderable primitive in the web client, before any interpretation.**
   Give the web build its `ClientUiProfile`-equivalent (the widget kinds and region
   roles the interpreter implements, in `reconcile.mjs`) and a pure check that returns
   the first unsupported primitive of a schema. A node test asserts an unsupported
   widget kind or region role is a named rejection. This lands before the interpreter
   so the interpreter never runs on a schema it cannot fully draw.

4. **Interpret the model in the web client.** Build the DOM interpreter over the
   schema plus dynamic state, gated by the step-3 profile check. Renderability is
   per-kind, and presence alone is not sufficient — a node is drawn only when
   `present` AND its kind's content requirement is met: a **Label/Field** draws only
   when `present && leaf.has_value()` (a present-but-`nullopt` leaf is the resolved
   drop and draws nothing); a **Checkbox** draws whenever `present` (its leaf state
   always exists), showing `checked` and the optional caption `value`; a **Spacer**
   draws its structural gap whenever `present`, from the schema's `Spacer` kind, even
   though `leaf` is `nullopt`; a **Container** lays out its children whenever
   `present`. For a drawn leaf: element by widget kind, text from `value`, color from
   the theme role as a CSS custom property, and a click handler dispatching `command`
   when present, over the existing input channel; regions are placed by region role.
   A `uiState` whose generation or node-id set disagrees with the `ui` schema is not
   interpreted (wait for a consistent frame). Reconcile by node id. The pure logic
   lives in `reconcile.mjs`; a node test asserts a schema+state pair yields the
   expected element/text/role/command/checked set, that the per-kind render gate holds
   (a `nullopt` Label draws nothing, a present Spacer draws a gap), and that a delta
   touches only the changed node. `client.mjs`/`index.html` gain the region containers.

## Test kinds

The dynamic-state resolution parity (step 1) is an algorithm oracle against the TUI
lowering, written before the shared resolver so it pins behavior. The wire round-trip
and malformed-input rejection (step 2) are contract oracles. The profile rejection
(step 3) and web interpretation (step 4) are node tests over the pure logic in
`reconcile.mjs`. No golden of presentation taste; assert resolved values, wire bytes,
and rejection.

## Residue

What survives phase 6: the dynamic-state section types (`UiLeafState`, `UiNodeState`,
`UiStateSection`) and their codec; the runtime's schema-derived dynamic-state
resolution; the web interpreter's pure logic in `reconcile.mjs` and its node tests;
the client-profile rejection and its test; any CONTRACT line on the library/host
seam the dynamic-state publication introduces (what a client may assume the section
resolves, what it must not resolve itself). The runtime `UiInteractionState`
ownership, the mutation-patch wire, and canonical-focus routing are NOT phase-6
residue — they are phase 7. The plan, the file list, and this exposition do not
survive — git keeps them.
