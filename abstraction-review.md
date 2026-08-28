# Web client abstraction review

## Finding

The project does not primarily lack a generic UI tree. It already has one:
`UiSchema` contains one `UiNode` tree with medium-neutral layout constraints,
stable node identity, widget descriptors, and validated well-known areas
(`include/ssg/UiTree.h:3-18`, `include/ssg/UiTree.h:67-112`). Separate
`UiStateSection` and `UiPresenceSection` authorities complete its published
render state.

The agility problem is that this model is not the single path to either client.
The web client uses the tree for only part of the screen, the terminal computes
most geometry from a second tree, the running HTTP host bypasses the library's
typed server, and features add independent ingress and delta conventions. New
work therefore extends several partially overlapping abstractions instead of one
small client contract.

The target should be a retained UI model with four explicit concerns:

1. one whole-screen `UiNode` tree carrying structure, semantic role, layout
   intent, presence, accessibility, activation, and content bindings;
2. separately versioned authoritative data streams for document and feature
   content;
3. client-owned interaction state for work a client can resolve completely from
   published data;
4. command-carried prediction and acknowledgment only when a client temporarily
   predicts server-authoritative state.

These concerns should share one typed transport and one atomic update boundary.
They should not be collapsed into a generic property bag or a universal
optimistic-state framework.

## Evidence of the current split

### The tree is present but not authoritative end to end

`UiNode` already gives every node an id, parent-relative size, and either
container or leaf content. Containers carry axis, inset, gap, children, and
scroll intent (`include/ssg/UiTree.h:67-112`). Resolved leaf text, labels,
commands, checked state, and semantic role are published in `UiStateSection`
(`include/ssg/UiNodeState.h:29-64`). Visibility is published separately in
`UiPresenceSection` (`include/ssg/UiPresence.h:24-49`).

The web interpreter joins all three sections, but `renderChrome` routes only
`header`, `footer`, and `body` through the resulting tree
(`apps/web/client.mjs:421-477`). The tree's `footer.prompt`, `notice`, and
`externalmod` nodes are skipped and rendered through persistent, feature-specific
DOM hosts and feature-specific functions (`apps/web/client.mjs:480-646`).
Consequently, a node can exist in the authoritative tree, declare a backing
surface, and still not determine placement or rendering.

The terminal does not use this tree as its whole-screen layout input either.
Grid presentation retains a parallel shell/layout model. That duplicates the
meaning of tabs, notices, prompts, external-modification actions, panes, and
scroll regions instead of making the terminal's optional grid solver consume
the same node tree.

### Dynamic node state has three synchronization protocols

The schema, resolved leaf state, and presence are three complete node-indexed
sections. Each carries the same generation and node correspondence, and each is
replaced wholesale by its own delta type
(`include/ssg/session_snapshot.h:233-252`). The browser must reject a frame unless
all generations and node sets agree.

The internal separation has a reason: structure, resolved values, and presence
change at different rates, and presence has a reconciliation basis. That does
not require three full wire snapshots with three client-side joins. A single
wire node encoding can carry the current typed fields while updates replace
changed nodes or changed fields atomically. Internal authoring and projection
types may remain separate if they improve correctness.

`MutationPatch`, `ApplicationId`, and `PresenceBasis` already define atomic,
reconcilable visibility mutations (`include/ssg/MutationPatch.h:3-24`,
`include/ssg/MutationPatch.h:36-90`). `PresenceBasis` is published and required
by the presence codec (`src/PresenceProtocol.cpp`), but the shipped browser
ignores its value and performs no reconciliation. `MutationPatch` and
`ApplicationId` remain library-internal with no wire representation or client
path. The project is paying the conceptual cost before receiving the wire or
latency benefit.

### Feature content and update shapes are ad hoc

`ViewSurfaceBacking` is a useful seed: it makes each opaque view surface name
the authoritative snapshot sections that back it
(`include/ssg/ViewSurfaceBacking.h:17-41`). The binding is not carried as data to
the client, however. The browser mirrors it with switches and special rendering
paths.

One session delta currently mixes several update shapes:

- document text splice plus separate caret;
- whole-section replacement;
- explicit changed flag plus optional replacement;
- keyed upsert/remove merge;
- ordered splice with base revision and snapshot fallback.

| Current delta type(s) | Payload shape today | Target payload shape | Envelope change required |
|---|---|---|---|
| `DocumentDelta` | Text splice with base/result revisions | Text splice | Keep the revision pair; use the common snapshot-required and stream envelope. |
| `TabDelta`, `ThemeSectionDelta`, `UiSectionDelta`, `UiStateSectionDelta`, `UiPresenceSectionDelta`, `PaletteSectionDelta`, `StyleSectionDelta`, `ShellSectionDelta` | Value replacement; a populated optional means changed | Value replacement | Add the common revision pair, explicit update presence, and snapshot-required recovery. |
| `PromptProjectionDelta`, `PromptViewSectionDelta`, `NoticeViewSectionDelta`, `TreeWindowsDelta`, `ClipboardDelta` | Value replacement; a separate `changed` flag distinguishes unchanged from an absent replacement | Value replacement | Replace the bespoke changed wrapper with the common update-presence and absence semantics; add the common revision pair and snapshot-required recovery. |
| `ExternalModificationDelta` | Keyed upsert/remove merge with base/result revisions | Keyed merge | Keep the revision pair; add common snapshot-required recovery. |
| `SettingsSectionDelta` | Keyed change list | Keyed merge | Add the common revision pair, update-presence semantics, and snapshot-required recovery. |
| `TreeDelta` / `TreeProviderDelta` | Ordered splice with base/result revisions and snapshot-required fallback | Ordered splice | Keep the payload and recovery behavior; conform to the common stream envelope. |

The two value-replacement rows differ only in envelope and absence semantics,
not payload shape. Today only some deltas carry base/result revisions and only
`TreeDelta` carries snapshot-required recovery. Those are the inconsistencies to
normalize; the different payload shapes remain.

The browser has separate application logic for these forms
(`apps/web/reconcile.mjs:258-307`) and feature-specific projection logic. For
example, external-modification action labels, command ids, and status glyphs are
re-derived in JavaScript (`apps/web/reconcile.mjs:386-437`) instead of arriving
as authoritative affordance data.

Different data structures can justify different patch payloads. They do not
justify different envelope rules, absence semantics, revision behavior, or
resynchronization behavior.

### The browser-facing host is a second protocol

The running web host parses `KEY`, `CMD`, `PFOC`, `PSUB`, `TSEL`, `EXMD`, and
`SNAP` frames separately (`apps/http_serve.cpp:365-551`). Each feature chooses
its own framing, parsing, validation, and escaping rule. The host also adds its
own settlement envelope (`apps/http_serve.cpp:230-264`).

This bypasses the typed ingress, replay, backpressure, and bounded protocol path
already present in the library server. It also puts product-aware composition in
the adapter: a tree click becomes select-then-activate, palette submit validates
and translates a candidate, and external-modification clicks use another
select-then-command sequence (`apps/http_serve.cpp:448-531`).

Those are commands or typed input events, not new transport concepts. A new
interactive feature should not require editing the WebSocket parser.

### `EditorSession` and `EditorRuntime` are not peer hosts

The names imply two possible editor hosts, but the implementations do not.

The current `EditorSession` is a small dispatch kernel. It owns the command
catalog, attached principals, session revision, topology, a mutex, and the
guards for capabilities, stale revisions, revision exhaustion, handler
exceptions, and nested dispatch (`src/EditorSession.cpp:53-68`,
`src/EditorSession.cpp:112-207`). It owns no document, workspace, selection,
interaction, feature, or snapshot state.

The current `EditorRuntime` is the editor aggregate. It owns the workspace,
documents, selection, history, settings, clipboard, tabs, prompts, interaction
authority, tree, syntax, LSP, diffs, external-modification flow, recovery,
autosave, workers, and presentation projections. It constructs and owns an
`EditorSession` as an implementation member
(`src/runtime/editor_runtime_internal.h:131-238`,
`src/EditorRuntime.cpp:2919-2960`).

Every shipping editor client already uses `EditorRuntime`: the terminal,
standalone web host, and Lua host. Direct `EditorSession` consumers are the
typed HTTP route, protocol/test slices, and stand-in test editors.

The distinction matters because `EditorRuntime::dispatch` is not a transparent
forwarder. Around `EditorSession::dispatch` it:

- snapshots the routing signature;
- reconciles stale find state and picker candidates;
- reconciles notice and external-modification presence;
- pauses follow-edits after local edits;
- drains requested follow-up commands in revision order;
- closes an accepted file picker;
- computes routing and geometry effects.

That behavior is in `src/EditorRuntime.cpp:3045-3164`. Calling
`EditorSession::dispatch` directly skips it all. The lower path nevertheless
returns the same `CommandResult` type, with default-false `DispatchEffects`
(`src/EditorSession.cpp:207`), making the incomplete path look correct.

Attachment has the same defect. `EditorRuntime::attach` registers the client
with both the dispatch kernel and `FollowEditsModel`; direct
`EditorSession::attach` registers only the principal
(`src/EditorRuntime.cpp:2966-2977`, `src/EditorSession.cpp:84-100`).

Snapshots settle the choice: only `EditorRuntime` can produce the authoritative
semantic and optional grid views (`src/EditorRuntime.cpp:3179-3222`).
`EditorSession` contributes only revision, topology, principal, and view id. The
typed HTTP route therefore has to combine direct session dispatch with a second
`HttpEditorSessionHost::snapshot` callback (`include/ssg/HttpEditorServer.h:68-85`).
That split structurally permits command execution and published state to come
from different owners.

The current layering also exposes three misuse traps:

1. `EditorSession::advanceRevision` is public even though its contract says it
   exists for runtime-owned out-of-band changes
   (`include/ssg/EditorSession.h:139-148`).
2. Both `EditorRuntime::snapshot` overloads are `const` but use `const_cast` to
   drain background work, which may mutate authoritative state and advance the
   revision (`src/EditorRuntime.cpp:3179-3182`,
   `src/EditorRuntime.cpp:3208-3212`).
3. `CommandServices::runTransaction` suggests rollback or atomic commit, but the
   runtime implementation simply calls the operation
   (`src/EditorRuntime.cpp:988-991`). The actual safety of a failed command
   depends on each handler staging its own mutations.

Grid snapshot generation is not currently a pure read for another reason:
`shellView` updates cached pane and panel dimensions that later reveal and
scroll commands consume (`src/runtime/editor_runtime_internal.h:333-348`).
Those behavior-bearing values are shared runtime fields rather than explicit
per-view state. A multi-client host can therefore let one client's dimensions
affect another client's later command.

### Low-latency behavior is several different ownership models

The current browser has at least these local mechanisms:

- predicted document insertion, settled by client edit id;
- palette query, filtering, selection, and scrolling;
- DOM scroll offsets retained by node id;
- prompt caret and focus planning;
- browser-native focus, accessibility, selection, and future IME composition.

They should not all become one optimistic mutation abstraction.

Palette query and filtering are fully determined by the published candidate set.
Scroll offset, hover, focus mechanics, and IME composition are presentation
state. The client is the authority for these values; there is nothing to roll
back. Document insertion and a predicted authoritative presence change are
different: the library remains authoritative, so the client needs a sequence id,
basis, acknowledgment, and rebase or rejection behavior.

## Recommended contract

### 1. One retained `UiNode` stream

Keep one closed `UiNode` type with typed optional fields, not a hierarchy and not
a `map<string, Value>`. The node should be sufficient for a native client to
create its retained element:

- stable `UiNodeId` and ordered children;
- closed semantic/widget kind;
- axis, size, inset, gap, scroll, and minimal layer/overlay intent;
- authoritative presence;
- semantic role and accessibility label/state;
- activation or command binding;
- a typed binding to separately synchronized content.

"DOM parity" should mean enough semantic and layout intent for a browser to use
native DOM/CSS without feature-specific placement knowledge. It should not mean
CSS strings, pixels, browser event names, arbitrary attributes, or resolved
geometry.

Use one tree revision and atomic tree update. A compact update can re-emit
changed nodes, including a parent's ordered child list, rather than inventing a
large opcode set. Full snapshot is the resynchronization form of the same node
encoding. Presence changes are node updates; when predicted, they additionally
carry the application identity and basis needed for reconciliation.

### 2. Separately versioned data streams

Do not embed large or rapidly changing content in `UiNode`. A node binds to a
typed stream or channel whose payload is authoritative library data. Examples
include document text, tabs, tree rows, prompt controls, notice actions,
external-modification rows, and palette candidates.

Every stream should have the same outer synchronization contract:

- stream identity;
- base revision and resulting revision;
- snapshot or update;
- atomic application;
- explicit snapshot-required recovery.

Payloads may use one of a small closed set of update shapes where the data
requires it: value replacement, keyed merge, ordered splice, or text splice.
The generic part is synchronization, not the payload.

The full candidate list belongs on the palette stream. The client filters and
ranks it locally, determines the visible rows, and submits an authoritative
candidate id. Adding another fuzzy picker should reuse this stream shape rather
than add a picker-specific report or host verb.

### 3. Name client-owned interaction explicitly

Use a concept such as `ClientInteractionState` for state that is intentionally
local under a declared interaction contract:

- caret, selection, and IME composition for a client-owned input buffer;
- fuzzy query, filtered ordering, highlight, and list scroll;
- viewport scroll offsets;
- hover and native focus mechanics.

The library publishes the data and allowed actions needed to run that
interaction. The client owns moment-to-moment state and sends only semantic
commit, cancel, or command input. This is the reusable low-latency abstraction
for fuzzy-find and similar widgets.

Do not call this optimistic state: no authoritative copy exists to reconcile.
Ownership must be declared by a typed interaction descriptor, not inferred from
widget kind. Some text inputs have authoritative runtime values and command
validation; their local caret or IME handling may be client-owned while their
text edits still require prediction and reconciliation. The descriptor must say
which individual values the client owns, predicts, or only presents.

### 4. Keep authoritative prediction separate

Retain a smaller `PredictedCommand` or `OptimisticMutation` mechanism for a
client applying the expected result of a server-authoritative command before its
response. The command carries a per-client application id and the relevant
authoritative basis. An update acknowledges the last applied id and supplies the
authoritative result; the client drops acknowledged predictions and reapplies
the rest.

Document local echo and predicted visibility changes fit this model. Fuzzy
filtering, scrolling, focus, and IME do not.

### 5. One typed client protocol and host

Inbound messages need only a small stable vocabulary:

- raw input when keymap/device interpretation belongs to the library;
- typed semantic command with typed arguments;
- attach/resync and capability negotiation.

Outbound messages are atomic snapshots or updates over the retained tree and
data streams, plus acknowledgments for predicted commands. Pointer activation
should publish or derive a typed command invocation; it should not add a
transport prefix.

Adopt the current `EditorRuntime` implementation as the one product host. Do not
adopt the current `EditorSession` as that host: it cannot produce state and its
direct dispatch and attach paths omit required runtime behavior.

The final public name should be `EditorSession`, because the aggregate represents
one authoritative editor session. Reaching that endpoint requires renaming, not
promoting, the current class:

- rename the current private dispatch machinery to `CommandExecutor` or
  `CommandDispatcher`;
- make it an implementation detail of the current `EditorRuntime`;
- after the old public `EditorSession` surface is gone, rename `EditorRuntime`
  to `EditorSession`.

Keeping the `EditorRuntime` public name is also mechanically valid, but leaving
both public types is not. The important decision is that today's runtime
implementation wins and today's session implementation becomes subordinate.

The public client surface should contain only operations that preserve the
aggregate's invariants: attach, detach, typed input or command submission,
semantic snapshot/update, and an optional client-requested presentation
projection. It must cover every currently valid ingress before a transport
cutover: raw key/text/pointer input where the library resolves it, semantic
commands, status actions, and binary/clipboard transfer. Otherwise the old host
callbacks survive as a second behavior path.

Configuration and operational controls such as init-script reload, autosave
flush, worker pumping, and live command registration can remain separate
aggregate-owned control or extension methods; they must not create another
editor dispatch seam. Lua command generations, protocol argument codecs, palette
publication, and host keymap compilation currently require catalog mutation or
introspection. Preserve that capability through an aggregate-owned command
extension/registry seam without exposing the private executor or returning an
unrestricted mutable catalog pointer.

Do not introduce an `EditorHost` interface merely to substitute fake editors in
transport tests. There is currently one product implementation, and a one-
implementation interface would preserve stand-in editors that already drift
from runtime behavior. `HttpEditorRoute` can depend directly on the aggregate.
Introduce a behavior interface later only if a second real implementation
appears. This does not prohibit a narrow connection-policy or session-resolver
interface: a multi-session server may need to select and lifetime-manage the
concrete aggregate by authenticated policy. Such an interface must return the
real aggregate; it must not supply its own dispatch or snapshot implementation.

The executable web server should use that library-owned aggregate rather than
maintain an independent protocol. HTTP asset serving and connection policy are
adapter work; editor attachment, framing, replay, validation, dispatch, and
snapshot/update production are library work.

The aggregate must also own the serialization boundary. Today the terminal is
single-threaded, the standalone web host adds `runtimeMutex`, and the typed route
adds `processingMutex`, while `EditorSession` locks only the handler body.
Moving a transport from one path to another should not change atomicity. The
single host should serialize attach, dispatch, runtime reconciliation,
background-result acceptance, and snapshot capture as coherent operations.
An explicit pump/advance operation should accept queued worker results before
snapshotting instead of mutating through a `const_cast`, and pump plus capture
must be atomic against dispatch. Making snapshots true reads additionally
requires moving the pane/panel geometry caches they currently write into
explicit per-view state updated by a named presentation operation. One client's
snapshot must not change another client's later reveal or scroll behavior.

The command executor remains useful internally. It should retain the proven
principal, capability, stale-revision, exception, nested-dispatch, and monotonic
revision rules. Its result should be private and should not contain
`DispatchEffects`; only the aggregate has enough information to compute those
effects. The public dispatch result is produced after all reconciliation and
deferred commands complete.

The aggregate-level nested-dispatch guard cannot simply disappear when
serialization moves outward. A handler that re-enters public dispatch would
block acquiring the aggregate boundary before the private executor could reject
it. First make handler re-entry impossible through the API, or retain a
same-thread marker checked before acquiring the boundary; only then remove the
duplicated guard.

### 6. Make each presentation consume the same model

The browser should mount every root child from the tree instead of routing a
hard-coded subset into fixed hosts. Persistent native controls can still retain
their DOM element and local interaction state; persistence is a client
reconciliation policy, not a reason to remove the node from the tree.

The optional terminal grid service should solve the same `UiNode` layout intent
into cells and hit targets. Terminal-specific glyph composition and measurement
remain in that service, but it should not have another semantic shell tree.

## Concepts to keep, merge, and retire

| Current concept | Direction |
|---|---|
| `UiNode`, typed layout constraints, `UiNodeId` | Keep as the center of the client API. |
| `UiStateSection` and `UiPresenceSection` | Keep useful internal ownership if needed; merge their wire projection into atomic node updates. |
| `MutationPatch` / `ApplicationId` / `PresenceBasis` | Keep only as authoritative prediction machinery; put it on the typed command/update path. |
| `ViewSurfaceBacking` | Promote from a C++ lookup to typed node-to-stream binding visible to clients. |
| Feature snapshot sections | Keep authoritative data, normalize their outer stream/revision contract, and publish complete affordances. |
| `PresentationSnapshot` | Keep as an optional grid projection, never as the semantic client contract. |
| Current `EditorRuntime` implementation | Keep as the product aggregate and sole client host; eventually use the public name `EditorSession`. |
| Current `EditorSession` implementation | Demote and rename to a private command executor; retain its dispatch invariants, remove public host access. |
| `EditorSessionBuilder` | Retire or rename to catalog assembly; it is a two-field construction shim, not an editor-session builder. |
| `HttpEditorSessionHost::snapshot` | Retire; snapshots must come from the same aggregate that accepts the command. Keep connection policy separate. |
| Parallel shell layout semantics | Retire as the terminal solver moves to `UiNode`; keep medium-specific accessibility and hit-geometry projection types where they add information. |
| App-specific `KEY`/`PFOC`/`PSUB`/`TSEL`/`EXMD` protocol | Retire in favor of typed input and command messages. |
| Client-side feature tables for labels, glyphs, and commands | Retire; publish them in authoritative data. |
| Generic universal optimistic state | Do not add. Separate client-owned interaction from predicted authoritative commands. |

## Migration order

**Migration rule:** add no feature-specific ingress verb, fixed DOM host, delta
convention, or substitute editor while this work is in flight. Extend the
target path or stop and repair it.

**Invariant at every step boundary:** `scripts/check.sh push` is green and
`tests/runtime/test_runtime_grid_parity.cpp` remains byte-identical. Do not
regenerate its golden during steps 1-12. Step 13 may regenerate it only for an
intentional, reviewed TUI output change; otherwise it also remains
byte-identical.

1. [ ] Split the command/result/attachment vocabulary out of `EditorSession.h`, so
   protocol and snapshot headers no longer expose the dispatch kernel merely to
   name shared values.
   **Exit:** `Protocol.h`, `session_snapshot.h`, and the aggregate header build
   without including `EditorSession.h`; shared dispatch values live in a
   behavior-free vocabulary header.
2. [ ] Add aggregate-owned typed ingress for every path the transport currently
   carries: commands, raw key/text/pointer input, status actions, and
   binary/clipboard transfer. Add aggregate-owned command extension and
   introspection for Lua registration, argument codecs, palette data, and keymap
   compilation.
   **Exit:** focused tests drive every supported ingress through the aggregate,
   and Lua and protocol code obtain command registration or metadata through the
   aggregate without accessing the dispatch kernel.
3. [ ] Retarget `HttpEditorRoute` directly to the current `EditorRuntime`. Keep only
   authenticated connection identity/capability and aggregate-selection policy
   outside the aggregate.
   **Exit:** `HttpEditorSessionHost::snapshot`, `statusAction`, and `binary` no
   longer exist, and `HttpEditorRoute` references the aggregate directly for
   attach, detach, ingress, and snapshots.
4. [ ] Replace typed-HTTP stand-in editors with focused transport codec tests and a
   smaller number of real-runtime integration tests. Delete shadow command/state
   implementations rather than adapting them.
   **Exit:** `CoreEditorSlice`, the end-to-end fixture's shadow command
   dispatcher, and HTTP test editors no longer exist; transport integration
   tests exercise a real aggregate.
5. Move the current `EditorSession` and `EditorSessionBuilder` out of the public
   API. Rename the former as a private command executor and assemble its catalog
   directly during aggregate construction.
   **Exit:** `EditorSession` and `EditorSessionBuilder` are absent from
   `include/ssg`, and no production code outside the aggregate implementation
   constructs or calls the private executor.
6. [ ] Put pre-dispatch checks, handler execution, post-dispatch reconciliation,
   deferred dispatch, and revision publication under one aggregate-owned
   serialized operation. Prevent handler re-entry before removing the duplicate
   nested-dispatch guard.
   **Exit:** the private executor has one dispatch caller, its result is not the
   public `CommandResult`, and a concurrency test proves no snapshot or second
   dispatch can observe state between handler completion and aggregate
   reconciliation.
7. [ ] Separate explicit background-result pumping from snapshot capture and move
   behavior-bearing geometry caches into per-view state.
   **Exit:** snapshot code contains no `const_cast`, taking a snapshot cannot
   advance the revision, pane/panel geometry is keyed by view, and a two-client
   test proves one client's dimensions cannot change another client's reveal or
   scrolling behavior.
8. [ ] Move the shipped browser from its ad-hoc frames onto the runtime-backed typed
   route while preserving document settlement, replay, and backpressure.
   **Exit:** `KEY`, `CMD`, `PFOC`, `PSUB`, `TSEL`, `EXMD`, `SNAP`, and the bespoke
   browser envelope no longer exist in the app or web client; the shipped route
   uses the typed transport and its reconnect and bounded-queue tests.
9. [ ] Rename the surviving aggregate from `EditorRuntime` to `EditorSession` after
   the old name is free.
   **Exit:** no symbol named `EditorRuntime` remains and no compatibility alias
   was added unless a separately stated compatibility contract requires it.
10. [ ] Publish complete feature affordance data and render every existing
    whole-screen surface through its tree node.
    **Exit:** the browser skips no root child by well-known id, fixed prompt,
    notice, and external-modification DOM hosts are gone, and client-side tables
    that invent feature labels, glyphs, or commands are gone.
11. [ ] Normalize stream envelopes and resynchronization while preserving each
    payload's appropriate patch shape.
    **Exit:** every stream update has explicit update presence, base/result
    revisions, and snapshot-required recovery; the bespoke changed-flag and
    optional-means-changed envelope conventions are absent, while tests cover
    value replacement, keyed merge, ordered splice, and text splice.
12. [ ] Put atomic retained-tree updates and command acknowledgments on the wire;
    remove full `ui`/`ui_state`/`ui_presence` replacement as the steady state.
    **Exit:** `MutationPatch` and `ApplicationId` have typed wire representations,
    the browser consumes `PresenceBasis` for reconciliation, and an ordinary node
    or presence change emits a retained-tree update rather than replacing all
    three UI sections.
13. [ ] Make the grid projection solve the retained tree, then remove the parallel
    semantic shell layout.
    **Exit:** terminal layout has one semantic input tree, the parallel shell
    layout builder no longer exists, and remaining shell types contain only
    medium-specific cells, accessibility, and hit geometry.

Each step should delete a path. Carrying old and new protocols indefinitely
would reproduce the current agility failure.

## Bottom line

The proposed smaller model is sound, but "one node type" alone is not the
missing abstraction; that type already exists. The leverage comes from making
it the only whole-screen placement path, giving its content bindings a uniform
sync contract, distinguishing client-owned interaction from authoritative
prediction, and routing every interaction through one typed protocol. That
removes the feature-by-feature concepts without turning the UI tree into a DOM
property bag or moving editor authority into clients.
