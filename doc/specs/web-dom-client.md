# Web DOM client — render the semantic snapshot, then deltas + local echo (M2/M3)

Status: in-flight. Delete on merge; the durable residue lands in the served
client asset, the host's delta loop, types, tests, and any CONTRACT line.

## Why one spec for two milestones

M2 (render the semantic snapshot in DOM/CSS) and M3 (apply deltas with local
echo) are entangled: the reconciliation rule that lets a keystroke appear before
its server acknowledgement is the same rule that decides how a delta merges into
what the client already shows. Designing the render model without the delta model
would fix a DOM shape that the delta path then has to fight. So the design is one
document; the plan ships it in two reviewable phases.

M1 already serves a stub page over `ssg --http PORT`, drives the `EditorRuntime`
directly (attach/dispatch/dimensionless `snapshot(client)`), refuses a second
concurrent attach, and delivers a whole semantic snapshot as one binary frame.
M2/M3 grow that stub into a real client and that whole-snapshot loop into a
delta loop, without touching the TUI path.

## The design choice this spec exists to settle

Three decisions have real alternatives whose consequences differ; reading the
code cannot settle them because the web client does not exist yet.

### 1. Where the delta stream comes from

The shipped `HttpEditorServer` already derives per-connection deltas, replays a
bounded history on reconnect, and marshals onto the command thread — but it holds
an `EditorSession&` and calls it directly, bypassing the `EditorRuntime` seam
(the documented I2 gap: no per-command reconciliation, no follow pause, no drain
of the deferred queue). The M1 host instead drives the runtime and ships whole
snapshots.

- Option A — route the web host through `HttpEditorServer`. Gets delta/replay for
  free, but re-enters the runtime-bypass we are trying to leave, and forces the
  `EditorSession&`→`EditorRuntime&` constructor change (a wide-blast-radius API
  change to a shipped type with many test call sites).
- Option B — the runtime-driven M1 host keeps the last semantic snapshot per
  connection and derives its own delta with `SessionSnapshotCodec::deriveDelta`,
  encoding it with `ProtocolCodec::encodeSessionDelta`. No API change, stays on
  the runtime seam, reuses the same codec `HttpEditorServer` uses. It must
  re-implement the small reconnect-replay buffer the shipped server already has.

**Decision: Option B.** The settled project direction is to drive the server
behind `EditorRuntime&`; adopting `HttpEditorServer` now would re-introduce the
bypass and pay for a shipped-API change before the web client has earned it. The
host keeps a per-connection `std::optional<SessionSnapshot>` (already present for
M1), and on each change derives and ships a `SessionDelta`; the first frame after
attach stays a whole snapshot. A bounded reconnect-replay ring is deferred to M3
only if a reconnect path is built; a fresh attach re-sends a whole snapshot,
which is always correct.

### 2. How input reaches the library

The keyboard-first global rule requires every user-visible action to be operable
through browser-deliverable keyboard input **via the authoritative keymap**. M1's
`TYPE:<char>` frame that hardcodes `text.insert` is a stopgap that satisfies
neither keymap routing nor any non-text action, and — the correction from spec
review — "the keymap's designated insertion command" is not a real contract. The
TUI does not resolve printable text through the keymap at all; it runs the same
two-part model the web host must replicate exactly, so both clients share one
behavior path through the library's own seams:

- Named keys go through `CompiledKeymap::resolve(pending, focus)`, which the TUI
  drives with a per-client **pending-stroke buffer** and the current
  `FocusTarget`. Resolution is three-valued: a match dispatches a command and
  clears pending; a strict prefix stays pending (a multi-stroke binding in
  progress); no match clears pending. The web host therefore owns a per-connection
  pending-stroke buffer with the same reset rules, plus a prefix timeout and a
  reset on focus change, so multi-key bindings follow the authoritative keymap.
- Printable content goes through `SemanticInputRouter::textRouting(focusName)`,
  exactly as `apps/ssg_main.cpp`'s `routeText` does: `Insert` dispatches
  `text.insert`; `PromptQuery` routes to prompt editing; `Ignore` drops it. The
  `PromptQuery` sub-decision — whether a printable character appends to the picker
  query or dispatches `find.update_query` / `replace.update_replacement` /
  `prompt.update_value` — is today duplicated in the TUI app's `routeText`.
  Duplicating it again in the web client would be a second copy of a behavior
  decision, so this spec **moves that sub-routing behind a library seam**. The
  seam takes `(focus, active prompt state, text)` and returns a closed result: a
  `Dispatch{command name, args}` for a library command, an `AppendPaletteQuery`
  for the one client-owned derived view, or an `Ignore`. Query *replacement* (not
  just append — e.g. backspace or paste) is expressed by the seam returning the
  command with the full new query value, matching how `find.update_query` and
  `prompt.update_value` already take the whole value. Both the TUI app and the web
  client call the one seam; the TUI app's `routeText` is refactored to adopt it so
  exactly one implementation exists. Only the `AppendPaletteQuery` result is
  acted on by the client itself; every other result is a library command the host
  dispatches. The palette query is the single client-owned derived view under the
  established prompt-fulfilment boundary (INV-derived-view-bounded), not a new
  out-of-band path.

- Option rejected — the browser interprets keys itself and sends named commands.
  This re-invents keymap resolution and text routing in JS, splitting the one
  behavior path into two.

**Decision: replicate the TUI's `CompiledKeymap::resolve` + `SemanticInputRouter`
seam in the web host.** The browser sends raw key/modifier events and printable
text; the host resolves keys against the keymap (with a per-connection pending
buffer, prefix timeout, and focus-change reset) and routes text through
`SemanticInputRouter::textRouting` plus the library prompt-routing seam,
dispatching every resulting library command through the runtime. Only the seam's
`AppendPaletteQuery` result is acted on by the client; all other prompt routing
is a library command. There is no web-only input path and no second keymap.

### 3. What the DOM is made of

The library no longer imposes a grid on the web client; the client lays out
natively. The open question is the document surface shape.

- Option A — one text container with syntax runs as inline spans, a caret element,
  and a selection overlay drawn from the semantic selection set; tabs as a strip,
  the tree as an optional list. Native flow, wraps by CSS.
- Option B — a line-per-element model mirroring the grid rows. Closer to the TUI
  but re-imposes row geometry the reframe removed.

**Decision: Option A.** The web client is a first-class client, not a grid mirror;
it renders the semantic model in the browser's idiom. Wrapping, scrolling, and
selection geometry are the client's own, computed from semantic offsets, never
from `PresentationSnapshot` (which the host does not even send — `snapshot(client)`
returns `presentation()==nullopt`).

### Offsets: UTF-8 bytes vs UTF-16 code units

SSG document positions — caret, selection endpoints, insertion points — are
UTF-8 `ByteOffset`s; a JavaScript string indexes UTF-16 code units. The client
never uses a raw JS string index as a document position. It keeps the document as
bytes (or maintains an explicit byte↔code-unit map) and converts at the boundary,
so caret, selection, and insertion are correct for non-ASCII and astral-plane
content. A test inserts and selects across multi-byte and surrogate-pair text and
asserts the dispatched offsets are the intended `ByteOffset`s.

## Theme is roles, not colors

The client maps `ThemeSnapshot` roles to CSS custom properties (one property per
role) and styles every element through those properties. The client invents no
color outside the role set (global rule); the built-in theme's values arrive in
the snapshot and change live when a delta carries a theme change. This is the web
mapping of the same role set the TUI maps to a terminal palette.

## Client ownership boundary

The served asset is the web *presentation*: DOM structure, CSS, wrapping,
scrolling, caret rendering, local-echo prediction, and key-event capture. It owns
no product behavior — every edit is a `ClientCommand` the host dispatches through
the runtime, and the authoritative state always arrives as a snapshot or delta.
The prompt/text routing decision is a library seam both clients call, not client
logic; the one client-owned exception is the palette query, a client-owned derived
view under the established prompt-fulfilment boundary. The host owns identity and
capabilities (the attach principal), the runtime seam, and the delta derivation.
Nothing in the client reconstructs feature state or reads a capability from a
payload.

## Local echo and the reconciliation contract (M3)

The load-bearing rule, the reason M2 and M3 share a spec:

> The client shows the authoritative document at the last applied revision plus
> its own not-yet-acknowledged local edits. When an authoritative update carries
> an acknowledgement, the client drops exactly the predicted edits that
> acknowledgement covers and keeps the still-unacknowledged remainder.

A revision number and the resulting document alone cannot tell the client *which*
predicted edits an update includes — coalesced, identical, or interleaved edits
are indistinguishable by content, and a background change (deferred enrichment,
diff refresh) advances the revision with no predicted edit behind it. So the
acknowledgement must be explicit and ordered:

- The client assigns each predicted edit a monotonic **client edit id** and sends
  it on the dispatched command frame.
- The web host tracks the highest client edit id it has **settled** — resolved as
  either applied or rejected — for a connection, and stamps `settledClientEditId`
  onto every outbound frame (snapshot or delta) as an envelope field, not inside
  `SessionDelta`, so the library wire types are untouched.
- The host emits a settlement for **every** predicted command, including one that
  changed nothing (a rejected or no-op command that advances no revision), as a
  header-only frame carrying the advanced `settledClientEditId`, so the client
  always receives closure and never retains a prediction indefinitely.
- The client drops predictions through `settledClientEditId` and re-bases the
  remainder onto the authoritative document. Applied and rejected settle
  identically because authoritative wins: a rejected edit is simply absent from
  the authoritative document, so re-basing drops it. If the predicted base and the
  authoritative document disagree, the client discards its predictions and adopts
  the authoritative document.

### The host envelope schema

The M2/M3 input and output protocol is host-defined (M1 already uses ad-hoc
`SSG1 ATTACH -` / `TYPE:` frames), so the settlement rides a thin host envelope
around the library-encoded bytes, leaving `ProtocolCodec` untouched:

- Inbound command frame: a small header carrying an optional `clientEditId`
  (absent for input the client does not predict, e.g. a bound key or a non-text
  command) followed by the key/text/command payload the host routes.
- Outbound frame: a header carrying `settledClientEditId` (a sentinel means
  "nothing settled yet") followed by an **optional** library-encoded body — a
  snapshot or a delta when state changed, and **no body** for a pure settlement of
  a rejected or no-op command that advanced no revision. A `SessionDelta` requires
  a revision advance (`SessionSnapshotCodec::deriveDelta`), so a no-change
  settlement is header-only rather than a fabricated empty delta; the client reads
  the settled id and re-bases onto its existing authoritative document.
- `settledClientEditId` advances when a predicted command is **resolved** —
  applied (its revision reached) or rejected — never before; ids are monotonic per
  connection and settle in the connection's message order. A rejected command
  advances the settled id without appearing in the authoritative document, which
  is exactly what drops its prediction.

M3's prediction scope is caret-anchored text insertion (the common typing case),
where re-basing the remainder is a string splice. Edits the client cannot predict
safely (structural commands, multi-caret, replace) carry no prediction and show
on the authoritative round-trip — correct, just not instant. The contract above,
plus the client-edit-id acknowledgement, is what the test pins; the prediction set
is an implementation choice the test does not freeze.

## Plan

Files: the served client asset (grown from `apps/http_serve.cpp`'s `kPage`; moved
to a build-embedded file once it outgrows a readable string constant), the host
loop in `apps/http_serve.cpp`, and a client-behavior test target for the
reconciliation contract. Independent oracles precede implementation where the
answer is knowable independently: a wire-shape oracle for the delta the host
emits, a UTF-8↔UTF-16 offset oracle, and the reconciliation property oracle.

### Phase M2 — render the semantic snapshot, keymap-routed input, blocking echo
- Grow the served client from the M1 stub into a real renderer: document surface
  with syntax spans, caret and selection from the semantic selection set, tab
  strip, optional tree list, all styled through theme-role CSS custom properties.
- Replace the `TYPE:` stopgap with the two-part input model: raw key/modifier and
  text frames; the host resolves keys through `CompiledKeymap::resolve` (per
  connection pending buffer, prefix timeout, focus reset) and routes text through
  `SemanticInputRouter::textRouting` plus the new library prompt-routing seam.
  Only the seam's `AppendPaletteQuery` result is handled by the web client; every
  other result is a library command dispatched through the runtime. The TUI app
  adopts the same seam.
- Echo is blocking in M2: the edit appears when its snapshot returns. No delta yet
  — the host may still ship whole snapshots this phase; keep snapshot and delta
  paths separable.
- Where the client asset lives: decide inline C++ constant vs a build-embedded
  file once the asset outgrows a single readable string. Not a product decision;
  fold into whichever step makes the string unwieldy.
- Oracle: the offset conversion (UTF-8 `ByteOffset` ↔ UTF-16) tested against
  multi-byte and surrogate-pair text before the renderer relies on it.
- Oracle: an input-routing parity test asserting the web host's key/text handling
  (the `CompiledKeymap::resolve` pending/prefix-timeout/focus-reset buffer plus
  the `SemanticInputRouter` + prompt-routing seam) produces the same dispatched
  commands as the TUI app for the same input and focus, so the seam cannot drift
  into a second behavior path.

### Phase M3 — deltas + local echo
- Host: per-connection `deriveDelta` + `encodeSessionDelta`, emitted on **every
  observed runtime revision change** for the connection — not only after a frame
  the client's own message caused — so background changes (deferred enrichment,
  diff refresh) reach the client. Whole snapshot only on first attach (Option B).
  Each outbound frame stamps the highest applied client edit id. Reconnect-replay
  deferred unless a reconnect path is built.
- Client: a delta applier that mirrors `SessionSnapshotCodec::replay` for the
  sections it renders, advancing its revision. This mirror is a boundary risk (two
  implementations of one replay); the wire-shape oracle and a round-trip check
  (apply the host's own delta and compare to the host's next snapshot) guard the
  drift.
- Client: local-echo prediction for caret-anchored insertion, dropping predictions
  through the acknowledged client edit id and re-basing the remainder per the
  reconciliation contract, authoritative-wins on divergence.
- Oracle: the reconciliation property test drives an interleaving of local
  keystrokes and host frames (with acknowledgements, coalescing, and a background
  revision bump) and asserts authoritative-plus-unacked equals the re-based
  result.

## What survives this spec

- The reconciliation contract → a client-level test, and a CONTRACT line only if
  no test name can carry the re-basing rule.
- The prompt/text routing seam → a library type both clients call, plus the
  input-routing parity test that keeps them one path; the TUI app's `routeText`
  duplication is retired into it.
- The role→CSS-property mapping and the no-invented-color rule → already covered
  by the color-authority scanner extended to the served asset if it grows CSS.
- The runtime-seam / no-bypass choice → the host stays on `EditorRuntime`; the
  I2-bypass note on `HttpEditorServer` is unchanged and still names the gap.
- Everything else — the phase list, the option rejections — is git history.

## Explicitly out of scope

Pointer/touch/IME/a11y affordances (M4, driven by real use), multi-client, the
`EditorSession&`→`EditorRuntime&` change to the shipped `HttpEditorServer`,
reconnect-replay unless a reconnect path is actually built.
