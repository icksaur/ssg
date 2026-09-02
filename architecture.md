# Architecture

SSG is a C++20 editor library with a terminal application. The library owns
editor meaning and authoritative product state. Clients own presentation,
native layout, and device I/O. An in-memory native GUI can consume the same
library contracts as the terminal application without reproducing editor
behavior.

The strongest contracts are the public types under `include/`, the target graph
in `CMakeLists.txt` and `cmake/ssg_layers.cmake`, and the focused tests under
`tests/`. This document is a map to those contracts rather than a replacement
for them.

## System model

An `EditorSession` is the serialized aggregate boundary for a running editor.
A host attaches an in-process client with a host-chosen principal and view, then
the client sends either a named `ClientCommand` or typed `ClientInput`. The
session validates identity, capability, and revision; applies the authoritative
transition; advances its revision when semantic state changes; and publishes a
per-client `SessionSnapshot`.

The normal flow is:

1. The host supplies client identity, view identity, and capabilities.
2. `EditorSession` publishes an initial semantic snapshot for that attachment.
3. Commands and semantic input converge on the same core command and transition
   machinery.
4. The core publishes revisioned state without deciding presentation.
5. A client presents that state through the optional grid service or its native
   UI, and resolves its own device input into the published typed inputs.

`EditorSession` serializes state operations across command execution, worker
publication, revision publication, and result construction. Command handlers
that need another command use `deferDispatch`; they do not recursively re-enter
the session.

## Build layers

Each library component has its own public include root. “Depends on” is in the
ordinary direction: the target in the first column may consume the targets in
the last column.

| Target | Responsibility | Public root | SSG dependencies |
|---|---|---|---|
| `ssg_platform` | Operating-system and filesystem adapters | `include/platform` | none |
| `ssg_core` | Editor behavior and authoritative semantic state | `include/core` | `ssg_platform` |
| `ssg_grid` | Optional cell-grid layout, rendering, and hit testing | `include/grid` | `ssg_core` |
| `ssg` | External embedding facade | none; interface target | `ssg_core`, `ssg_platform`, `ssg_grid` |

Core cannot depend on presentation. The grid is an optional presentation
service, not the editor contract. Executables are composition boundaries and may
link the components they host.

`cmake/ssg_layers.cmake` makes this graph executable. It records each target's
allowed direct links, public root, private roots, and public-header inventory.
Configuration fails for a forbidden target edge, an unexpected visible include
root, a missing public header, or duplicate logical ownership.

`cmake/components/*.cmake` are feature manifests, not additional architectural
layers. They assign sources to the owning component and register focused tests.
Allowed and forbidden compile-visibility cases live under
`tests/fixtures/layers`; graph-rejection assertions are enforced by
`cmake/ssg_layers.cmake` at configure time.

## Major components and power spots

A power spot is the one place that owns an important state transition or policy.
New behavior should normally extend these owners instead of reproducing their
knowledge elsewhere.

| Owner | Single responsibility |
|---|---|
| `EditorSession` | Serialize client attachment, command/input ingress, worker publication, revisioning, and per-client snapshots. |
| `InteractionAuthority` | Own whole-screen semantic truth, schema generation, prompt lifecycle, focus/presence projection, and tree revision allocation. |
| `Document::apply` | Validate and atomically apply text transactions against document revision, mode, UTF-8 boundaries, and edit ranges. |
| `Workspace` | Own file/document identity, open/save/close policy, baselines, archives, recovery, and observation of self-saves. |
| `CommandCatalog` and `CommandSpecBuilder` | Own the named command vocabulary, metadata, capability requirements, and handlers. |
| `CommandTransition` and `PreparedTransition` | Preflight and atomically install changes spanning prompt, panel, picker, focus, and tree-provider state. |
| `assembleWholeScreen` | Build the canonical medium-independent whole-screen UI tree. |
| `UiFrame` | Keep schema, dynamic node state, presence, and focus in one validated publication. |
| `GridPresenter` | Project semantic state through the grid-specific layout service and resolve grid view actions against a presentation basis. |
| Platform file and watcher seams | Hide Linux and Windows durability, filesystem-watch, and Git-watch implementations from core behavior. |

Domain models such as selection, history, search, tabs, trees, syntax, LSP,
diff, settings, theme, and status remain separate values and services inside
core. `SessionSnapshotSections` is their publication aggregate, not their
mutation owner.

## Authoritative UI model

The UI virtual machine is medium-independent. It describes what exists and how
nodes relate without publishing pixels, terminal cells, native toolkit objects,
or resolved rectangles.

- `UiSchema` is an immutable tree for one `Generation`. Each `UiNode` has a
  strong `UiNodeId`, a size constraint, semantic style roles, and exactly one
  container or leaf payload. Structural change replaces the schema and advances
  its generation.
- `UiStateSection` contains generation-scoped dynamic leaf values and the
  authoritative focus path.
- `UiPresenceSection` contains the present state of nodes at a
  `PresenceBasis`.
- `UiFrame` validates and publishes those three parts together.

`assembleWholeScreen` owns the canonical root/header/body/panel/content/footer
structure. `InteractionAuthority` rebuilds focus and presence from
`WholeScreenTruth`, so clients do not infer product state from geometry or
prompt kinds. Typed well-known areas let a client hand selected subtrees to
specialized native widgets while the library remains the authority for their
data and transitions.

## Commands and mutations

Commands are registered in `CommandCatalog`; each command has metadata,
capability requirements, and a handler. `EditorSession::dispatch` is the
authoritative ingress for named commands. Typed `ClientInput` is normalized into
the same command and transition machinery.

`CommandTransition` describes a closed set of changes across interacting
features. `PreparedTransition` validates the complete proposal before it is
installed, preventing a client from observing partially applied prompt, panel,
picker, focus, or tree state.

Commands that require native layout return a medium-neutral `ViewAction`.
The client resolves it against the matching presentation basis and may apply a
presentation-only change locally or return a basis-stamped `ViewTransitionInput`.
That transition vocabulary is intentionally narrow: only resolved follow,
pane-focus, selection, and active pointer-gesture results can return to core.

`MutationPatch` carries library-owned visibility mutations. A client may make an
optimistic local presentation change against a `PresenceBasis`; the next
authoritative `UiPresenceSection` settles that prediction. This supports
responsive in-memory clients without granting them authority over editor state.

## Snapshot publication

`SessionSnapshot` is the complete semantic publication for an attached client:
document, selection, history, prompt and search state, settings, tabs, diff,
trees, syntax, language-service state, theme, palette, notice state, and one
validated `UiFrame`.

Snapshots are move-only values. Their contract permits concurrent const reads,
but moving or destroying a snapshot requires external exclusion. Presentation
and input adapters consume snapshots; they do not mutate them.

## Presentation and clients

### Grid and terminal

`ssg_grid` is an optional presentation service. `GridPresenter` lowers a
semantic snapshot to a `CellGrid`; `Layout` solves terminal cell geometry; the
renderer applies theme roles and syntax state. The terminal application owns raw
terminal mode, terminal capability probing, input decoding, pointer routing,
clipboard escape sequences, frame writes, and restoration.

The terminal app translates device input into named commands and typed library
input, and uses published snapshots to resolve its presentation work. It does
not establish editor policy, command semantics, or authoritative layout state.

### Native in-memory clients

An in-memory GUI attaches directly to `EditorSession`, consumes
`SessionSnapshot` and `UiFrame`, and supplies its native layout solver and
widgets. The UI-VM provides semantic structure, constraints, values, focus, and
presence; the client decides pixels, retained objects, accessibility APIs, and
device interaction.

Routine client responsiveness remains local: a native widget can retain content,
filter the published palette candidate universe, and predict published presence
changes without a round trip. Commands and authoritative state changes still
return through the typed library ingress. Core keymap and focus routing explicitly
identify the small set of palette edits a client performs locally.

## Directory map

| Path | Purpose |
|---|---|
| `include/core/ssg` | Public editor and UI-VM contracts |
| `include/grid/ssg` | Public optional grid presentation contracts |
| `include/platform/ssg` | Public platform abstraction contracts |
| `src/` | Implementations, organized by owning library component |
| `apps/` | Terminal process host, terminal I/O/input adaptation, and init-script bootstrapping |
| `examples/tui/` | Reference terminal fixture and embedding examples |
| `tests/` | Focused owner tests and layer fixtures |
| `benchmarks/` | Editor and startup measurements with focused correctness checks |
| `cmake/components/` | Source ownership and focused-test manifests |
| `data/` | Checked-in command, theme, and Unicode inputs |
| `vendor/` | Vendored dependencies |

## Deliberate pressure points and exceptions

- `EditorSession` is intentionally a central aggregate. Its serialized
  publication and command ingress are the boundary that prevents clients from
  creating competing editor state.
- The UI-VM is intentionally richer than the grid. It is the shared semantic
  contract for native clients, while grid geometry remains in `ssg_grid`.
- `UiFrameDeltaCodec` is an in-memory convenience for retained clients; it does
  not introduce a transport or a second authority.
- `GridPresenter` owns terminal-grid lowering, but the terminal application
  owns terminal APIs and device I/O.
- Platform adapters are isolated below core so Linux and Windows behavior can
  differ without leaking platform objects into editor contracts.
