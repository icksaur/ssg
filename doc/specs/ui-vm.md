# The UI-VM: a library-owned, medium-agnostic UI model

Status: in-flight design. Delete on merge. The durable residue lands in the
library's UI-model types, the snapshot channel, the conformance tests, and any
CONTRACT line — never in this file.

## Why this spec exists

Building the web client as the second host surfaced the real question: how much
of the UI is product, owned by the library, and how much is presentation, owned
by each client. Today the answer is split awkwardly. The library owns the
*semantic* model (document, selection, commands, keymap, theme roles, prompt
state, palette) and a *grid-layout service* (a closed widget vocabulary, a box
solver, chrome lowering to cells). Every grid client — so far only the TUI —
consumes both. The web client, which lays out DOM natively, cannot consume the
grid service, so the header and footer it draws are *reinvented in JavaScript*:
their contents, arrangement, and click targets are re-decided in the browser
rather than described by the library. A third client (a native KDE app, a Vulkan
surface) would reinvent them a third time.

That reinvention is the divergence the project's first global rule forbids: a
host must present the library's model in its native idiom, never redefine it.
The header's layout and contents, the footer's hints, which widget a click
dispatches, how focus moves between the editor and a prompt — these are product
decisions. They belong to the library, expressed in a form no client has to
re-derive and no client renders as cells unless cells are its native idiom.

This spec proposes lifting header/footer **layout and contents**, plus the
**focus/prompt state machine** and the **click-to-command bindings**, out of the
grid-only service into a medium-agnostic model that every client interprets — a
**generic layout tree**, not a header/footer-shaped record. The nickname is the
*UI-VM*, after Another World: the game shipped identically to a
dozen platforms because each platform ported a small interpreter that ran the
same content, and the interpreter drew polygons but never invented game
behavior. The library is that content and behavior; each client is that
interpreter.

This is a real design choice with divergent consequences — a medium-agnostic UI
channel versus continuing to grow the grid service and accept per-client
reinvention — and it renegotiates who owns header/footer layout. Both triggers
for a spec are met.

## What this is not: no versioning, no public contract

The library is a tool this project uses to build one editor across a terminal, a
browser, and a desktop surface. It is not a platform shipped to third parties, and
the clients are built and released from this same repository together. So this
spec deliberately rejects the machinery a public UI platform would need:

- **No wire versioning, no version negotiation, no min-version handshake.** All
  clients are built from the library they target. A widget the library composes
  is a widget its clients are expected to render; a mismatch is a bug in this
  repository, caught at build or startup, not a runtime version drift to be
  gracefully tolerated forever.
- **No open-enum / unknown-id tolerance as a permanent wire law.** The widget
  vocabulary is closed and finite, and its meaning is fixed by the code, not by a
  numbered standard evolved across releases.

What the library *does* still need, and this spec keeps, is a way for the **host to
know, from the client implementation it is serving, which widgets that client
renders** so a composition that uses a widget the client lacks fails loudly and
early rather than silently drawing nothing. That is
conformance checking against a static capability set, not version negotiation.
The distinction matters: we adopt Server-Driven UI's *shape* and its
missing-widget *detection*, but not its forward-compatibility apparatus, because
we control every client.

## The model

The library already contains most of the parts; they are split across the
grid-coupled layer and the semantic layer, and the piece that would carry the UI
is authored but not yet on the wire. The model below names what each part becomes.

### One closed widget vocabulary, medium-agnostic

`Widget.h`'s `WidgetKind` is the closed vocabulary — Container, Label, Field,
Checkbox, TextInput, Spacer — and its own comment already states the intent that
composition builds trees of these kinds and never adds new ones. That enum is the
"opcode set." Everything in `Widget.h` measured in cells (the fit engine, the
pack machinery, `WidgetStack`, `layoutTextInput`) is a *grid client's solving* of
the vocabulary, not the vocabulary itself, and stays behind the grid service.

The medium-agnostic widget record already exists as `ChromeComposition.h`'s
`WidgetDescriptor`: a kind, an id, a value source (a literal or a named live
provider), a role drawn from the theme's `SemanticRole` set, a click command, and
ordering/overflow hints — and, notably, **no cells**. That record is reused nearly
verbatim as the UI-VM's leaf.

`WidgetKind` must earn the discipline the theme roles already have. `SemanticRole`
is a closed `std::uint8_t` enum with a count constant and a mirrored array bound by
`static_assert`, and it is the standing proof that a closed-ordinal, medium-agnostic
contract works across the TUI and the web today: each client maps a role to its
medium and none invents one outside the set. `WidgetKind` should adopt the same
shape so a client can enumerate exactly the kinds it must handle.

### One generic layout tree, not a header/footer shape

The library's current composition type overcommits: `ChromeComposition` has named
`header` and `footer` slots and `RowDescriptor` bakes in a left/center/right row,
so "two rows of chrome" is welded into the API's *shape*. That is the grid-chrome
assumption the UI-VM must shed. Header and footer are not the vocabulary; they are
two conventional places a client happens to put UI in a text editor. A native app
with a sidebar, a palette overlay, or a status popover has neither a "header slot"
nor a "footer row," and should not have to pretend to.

So the layout API is a **single generic tree**, not a fixed set of named regions.
A node is either a container (an axis, a sizing rule, insets, children) or a
widget leaf (a `WidgetDescriptor`). This is the same shape `Layout.h`'s
`LayoutNode` already has for the grid — the UI-VM promotes that generic recursive
structure to be *the* medium-agnostic payload, rather than shipping a
two-slot chrome record on top of it.

Where "header" and "footer" survive at all, they survive as **region roots** — a
small, closed set of well-known placement roots each carrying one generic layout
tree, so a client knows where in its native frame to attach each tree without the
tree itself being shaped like a header or a row. The set must cover the placements
real clients need — a top and bottom edge, an overlay layer, and leading/trailing
edges for a native client's sidebar — enumerated as placement *roles* rather than
named "header"/"footer" slots; the set stays closed and medium-agnostic for the
same reason the widget vocabulary is: a client must be able to enumerate the
places it may be asked to render. But the *contents* of every region are the one
generic tree type, and a region imposes no left/center/right or single-row
structure — that arrangement, where a grid client wants it, is expressed by how
the tree is built (a Container on the row axis with a Spacer), not by a bespoke
record.

The design choice this settles: generalize `RowDescriptor`/`ChromeComposition`'s
explicit slots into the generic tree plus a closed region-root set, rather than
add more named slots as new UI appears. The migration reuses `WidgetDescriptor`
as the leaf and `decodeChromeComposition`'s validation logic, but replaces the
two-slot container with the generic node tree.

### The box model is that same tree, solved to cells only by clients that use cells

`Layout.h`'s `LayoutNode` is already the generic constraint tree — axis,
exact-or-flex sizing, insets, children — and its comment already notes that richer
concepts are expressed by how the tree is built, not by the solver. The structure
is medium-agnostic; only its unit (cells) and its `Rect`/`ShellNodeKind` welding to
the grid are terminal-specific. The layout tree above and this box model are the
**same tree**: a container node carries the axis/sizing/inset constraints, a leaf
carries a widget. Generalizing means surfacing that constraint tree without its
grid welding and letting a client map it to its idiom — a DOM client maps axis to
flex direction, flex sizing to `flex: 1`, an exact size to a chosen unit, insets
to padding — while only the grid client calls the solver that produces cells.

### Lowering to cells moves from the library to the grid client

Today the library owns not just the descriptor tree but its lowering to grid
cells (`ChromeLowering.h`'s `lowerChromeRow` builds a `WidgetStack`, resolves it
against a rect, and emits accessibility nodes). Lowering lives in the library only
because there has so far been one geometry and both hosts share it. Under the
UI-VM, lowering-to-cells becomes the **grid client's** job; the library publishes
the schema (structure + value sources) *before* lowering, and the resolved live
values ride the generation-scoped dynamic-state deltas beside it, not baked into
the schema. Live value *resolution* stays in the library, because a provider's
value (a file path, a dirty flag, a branch name) is semantic — but a resolved value
is dynamic state, not schema, so it never makes the "immutable within a generation"
schema mutable. The grid path itself is not removed — the `WidgetStack` fit engine
and `lowerChromeRow` stay as the terminal's private way of interpreting the tree.
The UI-VM adds a pre-lowering channel beside the existing lowering, rather than
replacing it.

### Three layers: an immutable tree schema, generation-scoped node state, and an atomic mutation patch

The library does not send "the tree once." It sends three distinct things, and
conflating them is the trap the first draft fell into:

1. **A tree schema** — the superset structure: every node that could ever be
   shown, its parent/child relationships, its `WidgetKind`, its trigger ids, and
   the **source/identity** of each value it displays (a literal, or the id of a
   live provider) — but **not** any resolved value. This is immutable *within a
   generation*: it changes only when the composition itself changes (init.lua
   reloads, a host sets a new chrome), and each such change is a **full-tree
   replacement stamped with a new generation number**. A client can cache and lower
   a schema for as long as its generation is current. Every node carries a strong
   **`UiNodeId` unique within its generation**; the schema is rejected at
   validation if two nodes share an id, if a trigger references a missing node, or
   if any reference crosses a generation. Patches, dynamic-state deltas, triggers,
   and focus entries all address nodes by this id, so uniqueness is a precondition
   for every other layer, not an afterthought — the existing widget/layout ids are
   not guaranteed unique and cannot be used directly.
2. **Dynamic node state** — the per-node data that legitimately changes frame to
   frame and that the schema deliberately omits: **resolved provider values,
   accessibility labels, inherited commands, and each node's server-authoritative
   present flag**. This rides **generation-scoped deltas** against the current
   schema; it is never "sent once," because a path, a dirty flag, or a branch name
   changes while the structure does not. Keeping resolved values here and out of
   the schema is what makes the schema genuinely immutable within a generation.
3. **A mutation patch** — how server-authoritative presence changes; a command a
   trigger dispatches produces it (below).

Every mutation and every dynamic-state delta **names the generation it targets**,
so a client that is still on an old schema rejects or refetches rather than
applying a patch to the wrong tree. A generation bump is a full replacement, not a
relative patch, precisely because a structural change has no meaningful "relative"
form.

This separation is what lets the schema be lowered to cells or DOM once and reused:
the expensive structural lowering is amortized across a generation, while the cheap
per-node state and presence flips flow as deltas.

### The mutation patch is one atomic, validated, ordered model

A trigger dispatches a command, and the command produces a **mutation patch**: a
single, atomic, ordered set of presence operations over named nodes, validated by
the library and pinned by the reference interpreter before any client consumes it.
It is *not* an unordered bag of
`toggle`/`show`/`hide` whose result depends on evaluation order. The patch model
must define, and the reference interpreter must enforce:

- **Atomicity and pre-state.** A patch applies against a known pre-state (the
  current server-authoritative presence configuration at a generation) and either
  applies whole or not at all; a client never observes a half-applied patch.
- **Determinism and conflict.** Contradictory operations in one patch (show and
  hide the same node) are rejected at validation, not silently order-resolved, so
  every client computes the identical post-state.
- **Parent/child semantics (one decided rule).** Hiding a container hides its
  subtree. Showing a node whose ancestor is hidden **implies showing the
  ancestors on its path** as part of the same atomic patch (it does not silently
  no-op), and a patch that hides an ancestor while also naming a descendant show
  is a contradiction rejected at validation. One rule, not a client choice — the
  patch algebra is not "deterministic" until this is fixed rather than left as
  "either rejected or implies showing."
- **Focus order.** A patch that changes a focus-bearing node's presence carries a
  defined effect on the focus model (below), applied as part of the same atomic
  patch, so focus never lags presence by a frame.
- **Replay.** Applying the same patch sequence from the same pre-state on any
  client yields the same post-state — the property the conformance suite asserts.
- **Causality for reconciled patches.** A reconciled patch (one a client applies
  optimistically before the library confirms) carries an **application id** and the
  **authoritative basis it was predicted against**; the library's acknowledgment
  names that id and basis. This is what a schema generation cannot supply — a
  generation identifies structure, not which predicted patch or presence pre-state
  is being confirmed. Without it, a delayed confirmation could roll back input that
  is already newer, or apply against the wrong local pre-state.

`toggle`, `show`, `hide` are the vocabulary of operations *inside* a patch; the
patch, not the loose operation, is the unit the wire carries and the interpreter
validates.

### The library declares mutations as commands; the client projects them optimistically

Every UI-VM mutation is a **command**. A trigger (a click id or a keybind) fires a
registered command whose effect is a validated patch on server-authoritative
presence; the library owns that presence, focus, and every semantic tie. What the
client does *not* do is invent product state — it **projects** the command's
presentational effect optimistically: the moment a trigger fires, the client
applies the patch to its local copy of presence and re-solves layout for
zero-latency feel, while the command travels to the library and the confirmation
(carrying the application-id/basis above) reconciles. If the library's confirmed
presence differs, the client's projection yields to it. So there *is* a round-trip
for the authority — the command — but not for the *pixels*; the visible change is
immediate and the authoritative change follows. This is the "retained-mode server,
immediate-mode client" split applied to presence: the library retains the
authoritative configuration and the command that changes it; the client renders the
predicted configuration immediately and reconciles.

Re-solving layout is still entirely the client's, because a confirmed patch changes
only presence, not geometry: the client re-evaluates its own layout over the current
schema with updated flags. Axes are *orthogonal by default* — each node's presence
is its own state, and there is deliberately no fully-qualified state vector like
`{tabbed visible, palette hidden, bar shown}`, which is the combinatorial explosion
the statechart literature warns against and which invents coupling that is not real.

Clients project and re-solve differently, and the model is designed so they can:

- **The TUI** applies the confirmed (or optimistically projected) patch by flipping
  the affected nodes' present flags and re-runs its own grid solver (`solveLayout` /
  `WidgetStack`) over the schema to produce new cells.
- **The web client** will most likely lower the schema **once per generation to
  DOM + JS**, where applying a patch is a class/`display` flip (or a reparent) and
  the **browser** re-solves layout natively. The application code runs no solver — it
  hands structure to the browser and lets CSS reconcile.

So the concerns stay cleanly separated, and this is what keeps the solver simple:

- **the tree schema** — pure structure, the solver's only input, immutable within a
  generation;
- **each node's server-authoritative present flag** — the state a command's patch
  changes, owned by the library and projected optimistically by the client;
- **the mutation vocabulary** — the atomic command-backed patches the library
  declares as available, keyed by trigger id.

No mutation is embedded in a widget (which would confuse the solver, as it should
only ever see structure and current flags), and no patch carries a whole-state
snapshot. A widget references a *trigger* (a click id or a keybind) by id; the
trigger dispatches a command; the client optimistically projects the command's
patch and re-solves.

### All UI-VM presence is server-authoritative; native affordances live outside the vocabulary

The first draft's "most mutations are purely local" was wrong in a way that
violated the global rules, and this is the correction. A library-declared,
keybound trigger that changes visible *product* UI — opening the palette, toggling
a product feature's panel — is a **user-visible action**, and the global rules
require every user-visible action to enter through the typed client API, be
registered in the command registry, be callable through the Lua API, and travel
one behavior path. Applying such a change entirely client-side would bypass all of
that and create a second behavior path.

So **every node the UI-VM describes has server-authoritative presence**, changed
only by a command, flowing to every client as generation-scoped dynamic state, and
projected optimistically by the client as above. There is no second class of
UI-VM presence and no per-trigger "is this local or product" classification for the
library to declare — every UI-VM trigger resolves to a command, full stop. That
uniformity is what keeps one behavior path: a client cannot decide a UI-VM node is
"just cosmetic" and mutate it privately.

A **native presentation affordance the client itself invents** — a native
scrollbar's visibility, a touch-reveal, a platform disclosure the library knows
nothing about — is simply **not part of the UI-VM at all**. It is not a UI-VM node,
carries no trigger the library declares, is absent from the mutation vocabulary and
the conformance profile, never enters semantic deltas, and never touches focus. The
client owns it entirely, the way a browser owns its own scrollbar chrome. Because
the library never models it, it cannot be confused with product presence and needs
no library classification: the boundary is not "which class is this trigger" but
"is this a UI-VM node at all." Anything the library describes is product presence
changed by a command; anything the client invents natively is outside the model.

Server-authoritative presence has a stated lifetime: it is owned by the library,
published with the schema's defaults, and a client's optimistic projection **resets
to the confirmed authoritative presence on any full-tree replacement (a new
generation), on reconnect, and on a profile or session/workspace change**. A client
never persists an optimistic flip across a generation bump — the
new schema's authoritative defaults win — so the library remains the source of
truth for the starting configuration.

Coupling, where it is real, lives *inside a mutation*, not in a global machine.
Two overlays that genuinely cannot coexist (a palette and a file finder competing
for the same region) are expressed by having the command's patch `show(palette)`
also `hide(finder)` — mutual exclusion written into the patch, declared and
testable, rather than enforced by a state vector the library maintains. Orthogonal
by default; coupling only where a patch names it.

### Keyboard focus: a preserved base context plus a transient capture stack

Focus has two kinds of change, and one structure cannot model both. Focus moving
between the **editor and a panel** happens while *both* nodes stay present — it is
not a push or a pop, it is a change of which persistent surface is active. Focus
moving onto a **transient overlay** (palette, file finder, a footer prompt) is a
capture that must be *returned* when the overlay closes. Modeling the first as
show-push/hide-pop would leave stale or duplicate stack entries for nodes that
never left. So focus is described as two layers:

- **A closed base focus context** — the persistent surfaces that are always
  present (Editor, Panel). This is `FocusTarget` **minus its transient `Prompt`
  member**: `Prompt` is not a persistent base, it is what the *capture stack*
  expresses, so keeping `Prompt` in the base type would give prompt focus two
  authorities (a base value and a stack entry) that could disagree. Moving base
  focus between Editor and Panel is an explicit focus operation, not a presence
  mutation.
- **A LIFO focus-capture stack layered on top**, holding only *transient* surfaces
  that captured focus. `show(palette)` pushes a capture entry; `hide(palette)`
  pops it; when the stack empties, focus returns to the base context beneath.
  Nested transient surfaces fall out of LIFO order. The base context is never a
  stack entry, so a present editor/panel is never duplicated onto the stack.

The **effective focus** — the single value keymap routing needs, including today's
`FocusTarget::Prompt` context — is *derived*, not stored twice: it is the top
capture entry's context if the stack is non-empty, otherwise the base context. So
`keymapContexts()` continues to produce the same closed set of contexts it does
today (including the prompt context), but the prompt context now comes from the top
capture rather than from a `Prompt` member of the base type. One authority for
prompt focus (the stack), one derivation to the effective context keymap routing
consumes.

The one-active-prompt contract is preserved explicitly, not incidentally:
`promptFocusRegion` and the single `PromptSurface` request mean **at most one
capture-stack entry is prompt-backed at a time**. The earlier "a prompt already
open beneath the palette" framing was wrong and is dropped — it contradicted the
one-prompt model. A palette or finder capture may sit above the base context, but
two prompt-backed captures cannot coexist; the stack's prompt portion stays bounded
to one by the same `PromptSurface` construction that guarantees it today.

The invariant worth enforcing is that **focus never references a hidden node**:
after any patch that hides a node, focus resolves to a present node — popping any
capture entry the patch hid (including when the patch hides an *ancestor* of a
captured node), and falling back to the base context. This cannot be made valid
"by construction" across a library-side stack and independently client-mutated
presence flags, because a local presence flip on one client could otherwise strand
focus. So the honest scope is: **one canonical library-side type owns both the
focus-affecting presence and the capture stack together**, applying a hide and its
induced pop as one atomic operation so no intermediate stranded state exists in the
authority. Clients may *predict* the pop-on-hide optimistically for latency, but
cross-process agreement is reached by reconciliation to that library type, and a
**seam test** asserts that after any hide the reconciled focus references a present
node. This is a seam test, not a CONTRACT line, because the property has a knowable
oracle; the atomic library type is what makes it hold.

Focus is authoritative library state; a client never invents a base-focus change
or a stack push/pop, it reflects the confirmed focus and predicts only the obvious
pop-on-hide. A patch that changes a focus-bearing node's presence is therefore a
*reconciled* command-projected change whose declared semantic tie is exactly the
base-focus change or the capture push/pop; a command whose patch touches no
focus-bearing node carries no focus effect, and a native affordance outside the
UI-VM is not a patch at all.

The library already holds the semantic core of this in pieces: `FocusTarget` is
today's closed focus axis (a non-geometric snapshot section, valid keymap contexts
derived from it so they cannot drift); its persistent members (Editor, Panel)
become the base layer and its transient `Prompt` context becomes a derivation from
the top capture entry; `ActivePrompt` and `PromptSurface` keep the one-active-prompt
invariant by construction and become the bounded prompt portion of the capture
stack;
`CompiledKeymap` answers keystrokes per focus context; and `PromptTextRouter` is
the single decision mapping printable text plus focus plus active prompt to a
command, a query append, or nothing, called by both clients today. The UI-VM keeps
all of that as the authoritative half and adds the mutation vocabulary and
client-side re-solve as the presentational half.

Click-to-command is already declarative: every accessibility node and widget
descriptor carries the command a click dispatches. A native client reads the
trigger off the widget and dispatches its command, optimistically projecting the
resulting patch; it needs no host-side pointer table for
chrome. The host pointer router stays only for document, tree, and tab hit
resolution, which is genuine geometry.

One behavior the library must still pin so clients do not diverge is the order in
which a reconciled mutation's semantic effect applies relative to its
presentational flip. Because the presentational half is client-applied and the
semantic half is library-confirmed, a client that reorders them can show focus in
the wrong place for a frame. This is a narrow client-prediction/reconciliation
ordering rule, not a general state-machine semantics question, and it has a
knowable oracle, so it is pinned by a named conformance test rather than a
CONTRACT line that would only duplicate a testable behavior.

### The channel

The transport spine exists: the sectioned envelope in `apps/http_serve.cpp`
carries tagged sections, and `EditorRuntime::snapshot` has two overloads bound by
a CONTRACT line — the dimension-less overload returns the identical semantic
sections with presentation absent, so a native client already receives the whole
semantic model without any grid geometry. The UI-VM's tree-and-mutations
description is a **new medium-agnostic section on this channel**, carrying the
tree schema (per generation), the generation-scoped dynamic node-state deltas, the
mutation vocabulary, and the focus (base context + capture stack) state. It must
live in the
semantic (medium-agnostic) channel and never inside the presentation snapshot, or
a native client cannot receive it — this is exactly what the presentation-optional
CONTRACT protects, and the UI-VM leans on and extends it.

## The two hard parts

### Missing-widget detection (not versioning)

Even with a closed vocabulary built in one repository, a client may not yet
implement every kind the library can compose — the web client, mid-migration,
renders a subset. The library must not silently compose a widget a client cannot
draw. The scaffolding to model *host-authoritative facts about a client* exists:
the attach handshake already carries a host-declared client state, and the
`HttpEditorSessionHost` CONTRACT establishes the discipline that such facts are
the host's to declare, never the client's to assert. The UI profile below follows
that discipline without reusing the authorization capability set itself.

The design choice is *when* a mismatch is caught. Because there is no versioning,
the honest answer is **as early as possible**: the host holds each client
implementation's profile of renderable widget kinds, and a composition that uses a
kind outside that set is a conformance
failure surfaced at composition time, not a runtime fallback the library carries
forever. This keeps the vocabulary's meaning fixed in code and turns "the web
client doesn't do checkboxes yet" into a loud, testable gap rather than a blank
region.

The one thing this must *not* do is overload the authorization seam. The attach
handshake's capability set is host *policy authorization* — what a principal is
permitted to do — and the `HttpEditorSessionHost` CONTRACT makes it
host-authoritative and never client-declared for exactly that reason. Widget
support is a different axis: it is *rendering conformance*, a fact about a
client's implementation, not a permission a policy grants. Conflating the two
would let a rendering gap masquerade as an authorization decision and vice versa.
So the missing-widget mechanism is a **distinct, non-authorizing client UI
profile**: the closed set of widget kinds a given client implementation renders,
selected by the host from the client build it is serving rather than by principal
policy. The library composes against the active profile; a composition that
exceeds it takes a **defined rejection path** — the composition is refused with a
named error identifying the unsupported element, never silently dropped — and that
refusal is pinned by a seam test. The authorization capability set is left
untouched and keeps its CONTRACT.

The profile's **scope and lifecycle must be defined, not assumed**. It covers not
only `WidgetKind` but the whole closed vocabulary a client must interpret: the
**region-root placement roles**, the container/layout constructs, and the
**mutation-patch operations** — a client that cannot render an overlay region or
cannot apply a `hide`-with-focus-pop is as non-conformant as one missing a
checkbox, and detection must reject any of these, not widgets alone. The profile is
a per-client fact (each attached client has its own), fixed for that client's
connection: it is established **at attach** and does not change mid-connection, so
"the active profile" is always a specific client's, never ambiguous across a
multi-client session. **Profile provenance is host-authoritative.** A profile is
derived from a client *implementation* — ideally build-generated from the set of
widget/region/operation handlers that implementation actually registers, so a
stale declaration cannot agree with a stale implementation — but the **host
selects which such profile applies**, and **no wire field supplied by the remote
client may grant or widen its own profile**. This preserves the attach-seam
contract: a client declaring "I support checkboxes" over the wire is not trusted;
the host knows, from the client build it is serving, what that client can render.
The rejection point is likewise defined: a composition (a new
generation) that exceeds a currently-attached client's profile is refused **at
composition time** with the named error, so the gap surfaces when the unsupported
element would first be published, not at some later render. **Rejection is
transactional and all-or-nothing across all attached profiles:** when a composition
would exceed any attached client's profile, the current generation, its dynamic
state, and the session revision are left unchanged and the host-only
composition seam (`setComposedChrome`) receives the named failure — no client
observes a new generation and there is no silent seam failure. A client attaching
late is checked against the current generation at attach; a client detaching removes
only its own profile from consideration.

The residue this leaves: `WidgetKind` gains the closed-enum discipline (count
constant, mirrored array, `static_assert`) so a client's UI profile and the
library's composed set are checkable against the same enumeration; the region-root
role set and the mutation-operation set gain the same discipline for the same
reason; the UI profile is a distinct type from the authorization capability set;
and a conformance test asserts both that a composition exceeding a profile is
rejected on the defined path and that every client's declared profile renders every
element it claims.

### Client-authoritative ephemeral state as a declared property

Some state must be owned by the client for latency: a picker's query text and
selection index cannot round-trip on every keystroke without feeling laggy. This
already works end-to-end, but as a hand-wired special case spread across three
mechanisms — an empty scroll command meaning "keep this local," a dedicated
palette-query append branch in the shared router, and a separate report section
echoing a request id so stale results drop. The library ranks; the client owns the
query text and the selection/scroll window, editing them without waiting, and the
newly ranked rows arrive from the library.

The design choice is how to generalize this from one bespoke surface into a
property any widget can carry. The established framing is netcode's *declared
ownership*: authority over each piece of state is split and stated, and the client
is granted its ephemeral fields for latency. Here the split is clean rather than a
conflict to reconcile: the client owns the ephemeral *input* (query, scroll offset,
selection index) outright, and the library owns the *derived result* (the ranking).
There is no committed query state on the library side to roll back toward — the
input never becomes authoritative semantic state, so there is nothing to reconcile,
only results to correlate. Under the UI-VM, a widget or region declares that its
transient state is client-authoritative:
the client **owns it locally and edits it without waiting** — a keystroke updates
the query text and the selection immediately, with no blocking round-trip on the
*input*. The **ranked rows**, though, are the library's: they change only when a
correlated library result arrives, because the browser must never rank (that would
be a second behavior path). So the query is **transmitted as request
input, yet never becomes authoritative semantic state**: the library ranks it and
returns derived rows tagged with the request identity, the client applies whichever
derived result matches its current local state and drops stale ones, and the local
query/selection is the client's to edit freely in the meantime. The distinction the
model must hold is "client-authoritative ephemeral input the library consumes to
produce a derived view" versus "committed semantic state" — the query is the former,
never the latter.

The risk to confront, and the part of this migration most in need of its own
review before code: today `PromptTextRouter` receives only `PromptRoutingState`
and hard-codes the `ActivePrompt::Palette` branch to decide "append to the palette
query." A generic "this widget's transient state is client-owned" property is not
available at that chokepoint, and arbitrary client-local state cannot be inferred
from the chrome descriptors the router never sees. Generalizing therefore is not a
matter of moving a flag onto a widget; it requires defining, before phase 7, an
explicit typed model the router consumes:

- **What the router reads.** A typed *semantic interaction metadata* input the
  router takes alongside the routing state, describing which of the currently
  focused surface's fields are client-authoritative and where their text is
  destined — replacing the hardcoded palette branch with a lookup over declared
  data.
- **Who owns it and for how long.** Each client-authoritative field has a stated
  owner (the client) and lifetime (transient); the library owns only the derived
  result it computes from that input, never a committed copy of the input itself,
  so there is no authoritative query state to reconcile toward — only correlation
  of results to the input that produced them.
- **How stale results are correlated.** The stale-drop key that today rides the
  palette report as a request id becomes a general correlation contract: a
  library-supplied derived result carries the identity of the input it answers, so
  a client drops a result that no longer matches its current local state.

Until those three are typed, the promised replacement is hand-waving; the spec
commits phase 7 to defining them first and reviewing that definition before the
router change.

## What is kept true

Every existing CONTRACT line survives. The attach-seam contracts
(`HttpEditorSessionHost`) are preserved and left untouched: the authorization
capability set stays host-authoritative and is not overloaded with rendering
concerns. The widget UI profile is a *distinct*, non-authorizing, host-selected
fact about a client, following the same host-declares-not-client discipline
without touching the authorization seam. The
presentation-optional contract (`EditorRuntime::snapshot`) is leaned on and
extended — the UI-VM section is medium-agnostic and never gated on grid geometry.
The one-active-prompt contract (`PromptSurface`) is preserved explicitly: the base
focus context is `FocusTarget` without its transient `Prompt` member, the effective
prompt context is derived from the top capture entry so `keymapContexts()` still
produces the same closed set, and the focus-capture stack holds **at most one
prompt-backed entry**, so `promptFocusRegion` and the single `PromptSurface`
request continue to guarantee two prompts cannot be open at once. The capture stack
layers transient surfaces above the base context without redefining either. The
host-only chrome-composition seam (`EditorRuntime`) stays a host-only,
non-registered seam; whether publishing the UI-VM section is likewise host-only is
a question the migration answers, not a contract it breaks.

The UI-VM does not renegotiate the "geometry is not a cross-client contract" rule.
It is the direct fulfillment of the first global rule — that each client presents
the semantic model in its native idiom. It adds a second, non-geometric UI
contract *beside* the grid service; a native-layout client ignores the grid
service exactly as that rule already permits, and now has a library-owned generic
layout tree to render — attached at whichever region roots its native frame
provides — instead of reinventing a header and footer.

## Migration: nail the TUI first

The migration's guiding constraint is that it must never regress the one working
client while it generalizes the boundary. The TUI is both the reference client and
the risk: it is the only consumer of the grid path today, so the safe order lifts
the model out from under the TUI *without changing what the TUI draws*, proves the
web client can consume the lifted model, and only then removes the web client's
reinvented chrome.

The phases below are a roadmap, not a schedule; each is independently reviewable
and gated. Two ordering principles the review surfaced govern them: the
medium-agnostic constraint types are **lifted and typed before** anything is
published on them (so the published tree is genuinely medium-agnostic, not
grid-shaped data relabelled), and the conformance oracle **grows with each phase**
rather than arriving at the end — each phase lands the validation and differential
tests for the types it introduces, or the phases are not independently gateable.

1. **Inventory and pin the current contracts.** Enumerate the classes this
   migration touches and state, for each, what it owns and what promise it makes:
   the widget vocabulary (`WidgetKind`), the current two-slot composition
   (`WidgetDescriptor` leaf plus the `RowDescriptor`/`ChromeComposition` header/
   footer record and its decoder), the grid lowering (`WidgetStack`,
   `lowerChromeRow`), the box model (`LayoutNode`, the solver), the focus/prompt
   state pieces (`FocusTarget`, `ActivePrompt`, `PromptSurface`, `CompiledKeymap`,
   `PromptTextRouter`), the channel (`SessionSnapshotSections`, the sectioned
   envelope, the two-overload snapshot seam), and the ephemeral-ownership special
   case (the palette client-owned-window mechanisms). This inventory is the map the
   rest of the migration edits against; it is scratch work, not a tracked document.

2. **Give the closed vocabularies their discipline.** Add to `WidgetKind` the count
   constant, mirrored array, and `static_assert` that `SemanticRole` has, so it is
   enumerable and a client's declared UI profile is checkable against it. Land the
   profile type (distinct from the authorization capability set) and its rejection
   seam test **for `WidgetKind`** here. The region-root role set and the
   mutation-operation set gain the same discipline in phase 3 and phase 4
   respectively, when those types are introduced, and the rejection oracle grows to
   cover them there — this phase does not claim full-vocabulary rejection coverage
   before those types exist. No behavior changes; the TUI draws exactly as before.

3. **Lift and type the medium-agnostic constraints.** Split the container
   constraints (`Axis`, `SizeKind`, `Inset`, tree order) out of `Layout.h`'s grid
   welding so the constraint tree carries no `Rect`/`ShellNodeKind` and no cell
   unit; the grid solver consumes these lifted constraints and remains the grid
   client's private step. Introduce the closed **region-root role set** with the
   same enum discipline and grow the profile rejection oracle to cover it. This
   precedes publication so what is published is a truly medium-agnostic tree, not
   grid data with the labels changed. Land the tests that the lifted constraints
   solve, on the grid, to exactly today's geometry.

4. **Generalize the composition into the schema-plus-mutations model and publish
   it dual-path, TUI still lowering locally.** Replace the two-slot
   `ChromeComposition`/`RowDescriptor` record with the generic tree schema
   (container nodes over the lifted constraints + `WidgetDescriptor` leaves,
   addressed by generation-unique `UiNodeId`) at the region roots, plus the
   generation-scoped dynamic node state and the atomic mutation-patch model with its
   closed operation set and reconciled-patch causality. Land the mutation-patch
   validator and its reference-interpreter tests here — node-id uniqueness,
   parent/child rule, conflict rejection, and replay determinism are pinned *before*
   any client consumes them — and grow the profile rejection oracle to cover the
   patch operations. Because atomic patch validation depends on the focus model,
   **land the canonical focus owner here too**: the base-focus type (`FocusTarget`
   minus `Prompt`), the focus-capture stack coupled to server-authoritative presence
   in one library type, the `PromptSurface` integration bounding the stack to one
   prompt-backed entry, the effective-context derivation, and their seam tests
   (after any hide, focus references a present node; no stale or duplicate
   prompt-backed capture). `PromptSurface` alone cannot prevent a stale capture entry
   unless the canonical owner couples focus to presence, so this must precede either
   client consuming patches. Publish the **schema** on the channel **alongside** the
   existing path — a semantic `ui` section carrying the generation-scoped tree, whole-
   value replaced per generation — and prove faithfulness by a **differential oracle**:
   the TUI lowering the published schema through the existing `lowerChromeRow` path
   produces **byte-for-byte-unchanged** output versus the legacy composition, across a
   corpus of states and after each mutation. This phase changes nothing a user sees;
   the differential proof is its whole product.

   **Publication boundary.** Phase 4 *lands and pins* the interaction-state types —
   the mutation-patch validator/interpreter, the canonical focus owner, and the
   presence model — as library types with their oracles, because the schema and its
   faithfulness proof depend on them existing and being correct. It does **not** wire
   those types onto the channel: publishing generation-scoped node-state/presence
   deltas, the mutation vocabulary, and canonical focus over the wire requires the
   runtime to *own* a live `UiInteractionState`, and there is no runtime owner and no
   client consuming patches until the TUI switches (phase 5) and the web interpreter
   lands (phase 6). Interaction-state **wire publication** is therefore deferred to
   phase 6/7, where the router generalization introduces the runtime ownership and the
   first consumer that gives it meaning. Publishing focus/presence/mutation deltas
   before a consumer exists would be speculative wire reshaped once phase 6/7 gives it
   a real reader; the schema is publishable now because the TUI differential oracle
   consumes it immediately.

5. **Switch the TUI onto the published model and remove the legacy shape.** Once the
   differential oracle is green, make the published schema the TUI's only source and
   delete the two-slot `RowDescriptor`/`ChromeComposition` container. The
   byte-for-byte oracle guards the switch; legacy removal is a separate, revertible
   step from the switch itself.

6. **Make the web client consume the published model.** Replace the browser's
   reinvented/absent header/footer with a DOM interpreter over the published schema:
   mapping kinds to DOM and roles to CSS custom properties, showing the resolved
   widget values, gating `display` by node presence, and reading triggers off the
   widgets. The schema carries value SOURCES, not resolved values, so this is where
   the **resolved dynamic node state deferred from phase 4** lands: the runtime
   publishes a generation-scoped, per-node `(present + resolved value/label/command/
   checked)` section — built by the same resolver and empty-drop rule the TUI lowering
   uses — on the channel, pinned by a round-trip oracle before the web interpreter
   reads it. Presence is derived all-present from the validated schema; the runtime
   does NOT yet own a live `UiInteractionState`, and the mutation-patch wire and
   canonical-focus routing stay deferred to phase 7 (no command emits a patch and
   prompt focus has no schema node until ephemeral nodes exist). This is
   where missing-widget detection earns its keep — the web build declares the UI
   profile its interpreter implements, and any gap (a widget kind or a region role it
   lacks) is a loud, tested client-side rejection BEFORE interpretation; host-side
   attach enforcement of a served build's profile stays a later-phase concern.

7. **Generalize ephemeral ownership and land runtime interaction.** Turn the palette's
   hand-wired client-authoritative window into the typed per-field ownership model the
   router reads as data (the three definitions the hard part above commits to),
   retiring the three bespoke mechanisms. This is also where the interaction-state
   half deferred from phase 6 lands, because ephemeral ownership is what creates its
   preconditions: the runtime takes ownership of a live `UiInteractionState` over
   ephemeral prompt/palette schema nodes, routes canonical focus through it, and
   publishes the mutation vocabulary (the reconciled optimistic patch protocol) on the
   channel — the first commands that emit a `MutationPatch` and the first client that
   reconciles one appear here. This is the phase that touches the shared routing
   chokepoint and warrants its own review before code, per the hard part above.

8. **Complete the conformance suite.** Each prior phase has already landed its slice
   of the oracle — vocabulary/profile rejection (phase 2), constraint-solve parity
   (phase 3), mutation-patch semantics and the TUI differential (phase 4), the web
   interpreter's profile coverage (phase 6), ephemeral reconciliation (phase 7).
   This phase closes the remaining cross-client assertions into one suite against a
   reference interpreter: that applying the library's mutation patches yields the
   same presence configuration in every client, that a reconciled patch's
   presentational flip and its focus effect apply in the pinned order, that after
   any hide focus references a present node, and that a widget trigger dispatches the
   declared command whose patch the client then projects. The suite is the operational
   definition of "a conformant client," and it is what lets a future native app know
   it is in parity by passing, not by inspection.

The sequencing question left open: whether the deferred web mouse and clipboard
work (Phases B/C of the prompt/palette spec) proceeds on the current ad-hoc web
client or waits for phase 6 here, after which the web client renders a
library-owned tree and that work becomes simpler. The migration is the reason to
consider waiting, but this spec does not force the order.
