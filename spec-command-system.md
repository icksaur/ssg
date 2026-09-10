# spec-command-system

## Goals

Replace the catalog/system machinery with one small `include/ssg/Command.h`
header that registers and dispatches user commands by string ID. Every
registered command is callable without arguments from Lua, a keybinding, and
the command palette; has one required display label; and produces the same
success, failure, or view action through every caller. Typed client operations
stop pretending to be commands and call ordinary typed functions instead.

## Design

`Command.h` owns the complete command abstraction:

- `Command` contains a required label and a no-argument
  `std::function<CommandResult()>`.
- `Commands` owns a `std::map<std::string, Command>`. The map key is the only
  command identity and its deterministic iteration order is the palette/help
  order.
- `Commands::add(id, label, handler)`, `find(id)`, `all()`, and `dispatch(id)`
  are the ordinary registration, lookup, enumeration, and execution API.
- `Commands::replace(oldIds, replacements)` copies the map, removes only the
  IDs supplied by ScriptHost, validates every replacement against that copy,
  and swaps on success. ScriptHost tracks exactly the IDs published by its
  prior successful evaluation; a replacement therefore cannot remove or
  overwrite a built-in. No ownership bit is stored on ordinary entries.
- `CommandResult` is the sole command outcome. It carries `CommandError`, a
  failure message, and an optional `ViewAction`. A handler returning a view
  action is sufficient; no independent effect declaration describes the same
  fact.
- Dispatch keeps the nested-dispatch refusal and exception-to-failure boundary.
  `Editor` continues to serialize commands under its operation lock.

The chosen mechanism is direct string lookup rather than integer handles.
Command dispatch is human interaction, not a throughput path; one ordered map
deletes the dual identity, revision-driven keymap recompilation, stable-storage
contract, and synchronized index. `CompiledKeymap` stores the command ID it
already receives from configuration.

Registered handlers take no payload. `InputRouting` retains
`RouteCommand`, narrowed to a string ID, and adds typed route results only where
the existing `EditorMutation`/`RouteViewAction` alternatives cannot express an
operation. `Editor::input` applies those operations under the same operation
lock and reconciliation pass as command dispatch.

Existing typed operations move to these owners:

| Family | Input/source | Typed route or function | Application entry |
|---|---|---|---|
| committed text | `ClientKeyInput` | existing edit transaction machinery | `applyInputMutation(Editor&, ...)` |
| explicit selections | `DocumentPointerInput`, `ViewTransitionInput` | existing `ApplySelections`; extend it for the current selection operations rather than carrying `SelectionCommandArguments` | `applyInputMutation(Editor&, ApplySelections)` |
| document/tree scrolling | `ScrollLinesInput`, `ScrollFractionInput` | existing `RouteViewAction` with `ScrollLines`/`ScrollFraction` | `executeInputRoute(Editor&, RouteViewAction, ...)` |
| tab targeting | `TabPointerInput` | new `ActivateTab` / `CloseTab` `EditorMutation` values | `applyInputMutation(Editor&, ...)` |
| tree targeting | `TreePointerInput` and tree model activation | new `ActivateTreeNode` `EditorMutation`; direct `treeCommand` feature functions for provider operations | `applyInputMutation(Editor&, ...)` |
| picker submission | `PickerPointerInput`, client-owned submit | new `SubmitPicker` typed route carrying `PickerActivation` and candidate ID | `executeInputRoute(Editor&, SubmitPicker, ...)` |
| external/UI activation | `ExternalActionPointerInput`, resolved `UiNodeActivationArguments` | new `InvokeExternalAction` / `ActivateUiNode` typed routes | `executeInputRoute(Editor&, ...)` |
| prompt editing/submission | `ClientKeyInput`, `PromptSubmission` | `PromptTextRoute` returns typed prompt mutations; prompt completion calls the owning feature function | `applyInputMutation(Editor&, ...)` |
| settings/configuration | prompt submission or ScriptHost table | existing `Settings`, `Theme`, `Style`, and keymap functions called directly | `Editor::input` or `ScriptHost::dispatchConfiguration` |
| files/tabs/encoding | prompt, picker, status, and tab inputs | existing `bindFile`, `bindTab`, and `bindEncoding` logic becomes named typed feature functions | `Editor::input` typed route |
| find/replace/search | prompt/search input | existing `executeFindReplaceCommand` and workspace search functions receive their concrete argument types directly | `Editor::input` typed route |
| resolved navigation/diff | tree/LSP/navigation result | existing `applyNavigationTransition` / `diffCommand` become typed feature functions | caller already holding `NavigationTarget` / `DiffFileId` |

Within each family:

- A user action with useful no-argument behavior keeps that behavior as a
  command. Its typed form becomes an ordinary function reached from
  `ClientInput` or another typed operation.
- A required-argument operation leaves the registry entirely. Text insertion,
  explicit selection placement, scrolling, UI activation, prompt updates,
  picker submission, tab/node targeting, settings mutation, encoding mutation,
  file/path completion, and resolved navigation are typed input/application
  functions.
- `theme.set`, `style.define`, `keymap.bind`, and `keymap.unbind` become
  `ScriptHost::dispatchConfiguration` branches that decode the existing flat
  Lua table and call the existing domain operation directly. Their existing
  Lua syntax remains accepted, but they are not palette/keybinding commands.
- Lua `ssg.register` requires an ID, label, and no-argument function. A
  successful reload atomically replaces the prior script registrations.

Command composition uses IDs only. A Lua command may queue another no-argument
command for the current dispatch to drain. Queued IDs resolve when drained;
registration and draining both occur on the editor thread, so replacement
cannot interleave. Typed workflows such as picker submission and prompt
completion own their sequencing in typed Editor functions; they do not encode
continuation state as command payloads.

Lua generation publication retains the existing two-phase lifetime:
`LuaCommandHost` stages new `luaL_ref` values while the previous
`pluginCommands` references remain alive; the ScriptHost publish gate swaps the
validated `Commands` map using handlers that capture ScriptHost plus command
ID, not a raw Lua reference; only then does `LuaCommandHost::publishStaged`
replace and release the previous references. Any evaluation or map validation
failure discards only staged references.

The command palette and generated help enumerate `Commands::all()` directly.
Because all entries are invocable, there is no argument-based eligibility
filter. Help presents the ID and label without owner groups or summaries.
Palette candidates are a snapshot for display, but submit revalidates the
candidate ID and originating `PickerActivation` against current state. A
command removed by reload is reported as unknown; it can never alias another
handler. After the selected dispatch, picker submission closes only when the
same activation remains open, preserving UI opened by the selected command
without consulting command metadata.

## Invariants

- **CMD-1 — Uniform invocation:** Every entry in `Commands` is invocable with no
  payload through string dispatch, keybindings, `ssg.command(id)`, and the
  command palette. State this on `Command` in `include/ssg/Command.h`.
- **CMD-2 — Single identity:** A command has exactly one identity: its string
  map key. No process-local alias may select a different entry. State this on
  `Commands` in `include/ssg/Command.h`.
- **CMD-3 — One declaration:** ID, required label, and handler are registered
  together; duplicate or empty values are rejected before mutation. State this
  on `Commands::add` in `include/ssg/Command.h`.
- **CMD-4 — Atomic script publication:** A failed Lua evaluation or invalid
  replacement leaves both the prior command map and prior Lua functions
  callable. State this on `Commands::replace` and the ScriptHost publication
  boundary.
- **CMD-5 — Serialized composition:** No command handler runs while another
  command handler is executing. State this on `Editor::dispatch`. The chosen
  queue mechanism drains requested commands after the current handler and
  clears the remainder on failure.
- **CMD-6 — Typed operations stay typed:** Required or optional payloads never
  enter `Command`, `Commands`, or `CommandResult`; their type is preserved from
  `ClientInput` to the ordinary function that consumes it. State this on the
  typed input application boundary in `Editor`.
- **CMD-7 — Outcome is authoritative:** Success, failure, and an optional
  `ViewAction` are represented only by `CommandResult`; no metadata must agree
  with the returned outcome. State this on `CommandResult`.
- **CMD-8 — Published equality:** The command palette and Lua no-argument
  command surface are projections of the same live `Commands` map, including
  script registrations. State this on their enumeration functions.

## Considerations

- Preserve command IDs and labels that users reference in keymaps, Lua, tests,
  or visible palette output unless an ID names a typed operation removed from
  the user-command surface.
- Replace optional-payload commands with a no-argument command plus a clearly
  named typed function. Do not keep an `std::any` compatibility path.
- View actions remain valid outcomes for no-argument commands such as visual
  cursor movement and pane focus. Typed client scrolling may return a view
  action without becoming a command.
- Picker submission must still validate the published candidate and activation,
  execute the selected command before closing the originating picker, and not
  close UI opened by that command.
- Prompt submission must route collected values to typed operations without
  restoring argument-carrying commands.
- A keybinding may continue to name an unregistered ID; dispatch reports that
  ID as unknown. Registering a Lua command later makes the same binding work
  without recompiling command handles.
- Script generation replacement may reuse its own IDs but may not replace a
  built-in or duplicate an ID in the offered generation. ScriptHost's retained
  ID set is the complete removal authority passed to `Commands::replace`.
- Registration occurs on the editor thread. The removed catalog concurrency
  claims must not survive in names, comments, or tests.
- `include/ssg/InputCommandNames.h` is deleted as each typed route leaves
  string command dispatch. Remaining no-argument command IDs are authored only
  by their registration, keymap/configuration strings, and user-facing inputs.
- The spec is deleted when implementation merges after its invariants are
  promoted to the named code boundaries.

Migration classification:

- Retain as no-argument commands: text deletion/newline/tab actions; cursor and
  selection motions when no explicit position is supplied; undo/redo;
  clipboard; edit suite; no-argument find/replace actions; file save/reload/
  delete and no-argument open/new behavior; active-tab actions; draft actions;
  follow-edits actions; tree selection/activation of the current node; palette,
  file-finder, search-result, and history navigation; LSP actions; pane/panel
  actions; appearance toggles; prompt/status navigation; settings open/export;
  help; and dynamic Lua registrations.
- Split into the retained no-argument command plus a typed function: optional
  selection targets, find query/replacement updates, workspace preview/apply,
  workspace/file open and new paths, active-tab close/close-others targets,
  workspace search query, and goto-line input.
- Remove from the command map: text insertion; explicit cursor/selection
  placement; scrolling; UI/prompt value/focus activation; settings mutations;
  explicit file/save-as/rename/new-directory/recent/dropped-content operations;
  tab activation/movement by ID; encoding/EOL operations; diff operations by
  file ID; tree node selection/invocation/scrolling; picker submit/execute/
  guarded close; and resolved file/symbol navigation.
- Keep outside the command map as ScriptHost configuration:
  `theme.set`, `style.define`, `keymap.bind`, and `keymap.unbind`.

## Risks and Mitigations

- Typed operations currently hidden behind command dispatch may bypass
  reconciliation. Centralize their application under the same
  `Editor::dispatch`/`Editor::input` reconciliation boundary before deleting
  registrations, and retain end-to-end input tests.
- Removing `CommandEffect::Routing` can weaken picker/UI activation ordering.
  Replace each effect-dependent branch with a typed operation whose return type
  states the next action, then delete the effect check only after routing tests
  pass.
- Lua reload can expose catalog entries whose Lua functions were discarded, or
  vice versa. Build and validate the replacement map in the publish gate, swap
  it before releasing old Lua references, and retain failure-injection tests.
- Removing command revisions can leave a stale palette. Build palette
  candidates from the live map or retain a cache whose invalidation is owned by
  `Commands`; do not couple keymap compilation to command registration.
- Broad registration edits can silently drop a keybindable action. Capture the
  current no-argument command IDs and visible labels as a pre-change golden,
  then require deliberate review of every removal from that surface.
- Temporary coexistence can create two dispatch paths. Migrate one operation
  family at a time and delete its old registration and payload test in the same
  step.

## Acceptance (Definition of Done)

- Observable: Ctrl+P lists every registered built-in and script command with
  its required label; keybindings and `ssg.command(id)` invoke the same entry;
  existing typed mouse, text, prompt, picker, settings, file, and navigation
  interactions retain their behavior without appearing as commands.
- Budgets: No command handles, `std::any` payloads, binder templates,
  `CommandSpec`, `CommandCatalog`, command effects, owner/summary/Lua flags,
  tombstones, or argument metadata remain. Command infrastructure is implemented
  entirely in `include/ssg/Command.h`; typed operation code remains with its
  owning feature. The final removal probe is
  `rg 'CommandCatalog|CommandHandle|CommandName|CommandSpec|CommandEffect|CommandContext|CommandHandlerBinding|bind(Wire|InProcess|Optional|NoArgument|Untyped)Handler|std::any' include/ssg/Command.h include/ssg/CompiledKeymap.h include/ssg/Editor.h src tests`;
  command-system matches must be empty and unrelated `std::any` matches must be
  reviewed explicitly.
- Gates: `cmake --build build`, `ctest --preset all`, and `git diff --check` are
  green; `python tools/unreached.py --write unreached.txt` adds no command-system
  residue.
- Oracles: `tests/fixtures/commands/user-surface.tsv`, captured before migration
  from the running `CommandCatalog` by the temporary
  `ssg_tests --dump-command-surface` mode and reviewed with an explicit
  keep/split/remove/config classification column, versus final
  `Commands::all()` for
  retained user commands; hand-case dispatch tests for success, unknown ID,
  failure, thrown handler, view action, and nested dispatch; transactional Lua
  reload tests for accepted and rejected generations; existing route/input
  hand cases moved from typed command dispatch to typed operations; palette,
  keybinding, Lua, picker-ordering, and prompt-submission end-to-end tests.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add the temporary runtime dump mode, capture `user-surface.tsv`, and review every row against the keep/split/remove/config classification above before implementation | `tests/test_dispatcher.cpp`, `tests/all_command_ids.h`, `tests/fixtures/commands/user-surface.tsv`, `tests/test_command_metadata.cpp` | golden: `build/ssg_tests --dump-command-surface` exactly reproduces the reviewed fixture before migration | CMD-1, CMD-6 |
| 2 | Route scrolling directly to `RouteViewAction` and delete scroll command registrations and argument wrappers | `src/input_routing.cpp`, `src/ViewCommands.cpp`, `src/navigation.cpp`, `include/ssg/InputCommandNames.h`, scroll/input tests | hand cases: document/tree line and fraction inputs produce the same actions and refusals | CMD-6, CMD-7 |
| 3 | Route committed text and explicit selection operations through edit transactions and `ApplySelections`; retain only no-argument edit/motion commands | `src/input_routing.cpp`, `src/editing.cpp`, `src/navigation.cpp`, document-pointer/input/edit tests | hand cases: insertion, click, additive click, drag, resolved selection, and navigation placement produce the same text/selections | CMD-1, CMD-6 |
| 4 | Route tab, tree-node, external-action, and UI-node targeting through typed `EditorMutation`/route values and feature functions | `include/ssg/ClientInput.h`, `include/ssg/InputRouting.h`, `src/input_routing.cpp`, `src/Editor.cpp`, `src/files.cpp`, `src/navigation.cpp`, `src/ViewCommands.cpp`, `src/ExternalModificationFlow.cpp`, pointer/input/session tests | hand cases: primary/auxiliary tab clicks, tree activation, external actions, and UI activation preserve outcomes | CMD-6, CMD-7 |
| 5 | Route prompt editing and completion directly to typed find/file/settings functions; remove prompt value/focus and required path/settings command registrations | `include/ssg/PromptSurface.h`, `src/PromptSurface.cpp`, `src/PromptRouting.cpp`, `src/ViewCommands.cpp`, `src/editing.cpp`, `src/files.cpp`, prompt/path/settings tests | hand cases: prompt text, focus, submit, cancel, path completion, and setting import preserve state and errors | CMD-1, CMD-6 |
| 6 | Split optional file/tab/encoding actions into retained no-argument commands and typed target functions; remove required-argument registrations | `src/files.cpp`, `src/Editor.cpp`, file/tab/encoding session tests | golden: retained IDs/labels unchanged; hand cases: explicit paths, tab IDs, encoding, EOL, and dropped content preserve behavior | CMD-1, CMD-6 |
| 7 | Split optional find/replace/workspace-search actions and route typed query/preview values directly | `src/editing.cpp`, `src/Editor.cpp`, find/replace/search tests | hand cases: no-argument actions and explicit query/preview operations reach identical snapshots and errors | CMD-1, CMD-6 |
| 8 | Route picker submission/close as `SubmitPicker`, dispatch the selected no-argument ID, and close only the still-matching activation; remove picker command payloads and duplicate execute path | `include/ssg/ClientInput.h`, `include/ssg/InputRouting.h`, `src/input_routing.cpp`, `src/Editor.cpp`, `src/navigation.cpp`, picker/input/session tests | seam oracle: selected command opens replacement UI and the replacement remains; stale candidate/activation is rejected; selected command precedes guarded close | CMD-5, CMD-6, CMD-8 |
| 9 | Route resolved navigation, diff targets, and internal tree invocations directly to typed feature functions | `src/navigation.cpp`, `src/Editor.cpp`, `src/language_services.cpp`, navigation/diff/tree tests | hand cases: history cursor, target opening, reveal behavior, diff selection, and provider invocation are unchanged | CMD-5, CMD-6 |
| 10 | Introduce `Command.h`, consolidate `Editor.h` command fields/signatures once, and migrate retained built-ins to direct `add(id, label, handler)` calls | `include/ssg/Command.h`, `include/ssg/Editor.h`, `src/Editor.cpp`, `src/ViewCommands.cpp`, `src/editing.cpp`, `src/files.cpp`, `src/navigation.cpp`, `src/language_services.cpp`, `src/help.cpp`, `src/ExternalModificationFlow.cpp`, `src/SystemClipboardReader.cpp` | hand cases: add/find/enumerate/dispatch success, unknown, failure, throw, view action, duplicate, and nested refusal; golden: retained built-in map | CMD-1, CMD-2, CMD-3, CMD-7 |
| 11 | Remove handles and catalog revisions from key routing; retain authored string IDs through resolution and dispatch | `include/ssg/CompiledKeymap.h`, `src/CompiledKeymap.cpp`, `include/ssg/Keymap.h`, `src/input_routing.cpp`, keymap/input tests | seam oracle: a key bound to an unknown ID reports that ID, then invokes a Lua command registered under that ID without rebuilding handle state | CMD-1, CMD-2 |
| 12 | Move startup configuration to `ScriptHost::dispatchConfiguration`, require labels for `ssg.register`, and atomically replace the ScriptHost-owned ID set while preserving staged/old Lua references | `include/ssg/LuaCommandHost.h`, `src/LuaCommandHost.cpp`, `include/ssg/ScriptHost.h`, `src/ScriptHost.cpp`, `doc/config.md`, `tests/test_lua.cpp`, `tests/test_script_host.cpp`, `tests/test_config_doc.cpp` | transaction oracle: failed evaluation/map replacement preserves prior callable functions and entries; end-to-end: script command appears in palette and runs through Lua and keybinding | CMD-1, CMD-4, CMD-5, CMD-8 |
| 13 | Project palette/help from the live map using ID and label, then remove old metadata fixtures and the temporary dump mode | `src/EditorViews.cpp`, `src/help.cpp`, `tests/test_command_metadata.cpp`, `tests/test_commands.cpp`, `tests/all_command_ids.h`, `tests/test_main.cpp`, palette/help tests | golden: palette and help equal final retained map including script replacements | CMD-3, CMD-8 |
| 14 | Delete the catalog, handles, binders, effects, payload/result layers, `InputCommandNames.h`, obsolete tests/build entries, and this spec after promoting its contracts | `include/ssg/CommandCatalog.h`, `src/CommandCatalog.cpp`, `include/ssg/CommandHandle.h`, `include/ssg/InputCommandNames.h`, `tests/test_command_catalog.cpp`, `CMakeLists.txt`, `spec-command-system.md`, all remaining include sites | removal probe from Acceptance; full gates and linker-unreachable report; review confirms every invariant has one code owner | CMD-1, CMD-2, CMD-3, CMD-4, CMD-5, CMD-6, CMD-7, CMD-8 |

## Rationale (optional, skippable)

The audit found that handles, stable storage, tombstones, revisions, transport
metadata, effect declarations, and repeated spec/binding/entry/result layers
serve no live SSG requirement. Typed client data was forced through the command
system and then erased into `std::any`, creating most of that machinery.
Keeping typed operations typed leaves commands as the ordered string-to-function
map the application actually needs.
