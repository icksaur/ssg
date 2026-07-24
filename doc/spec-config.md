# spec-config

## Goals

SSG loads a user config script (`init.lua`) once at startup, from a standard
per-OS location, and runs it in the SAME sandboxed Lua host the codebase
already has (`LuaCommandHost` — see `doc/features/language-services.md`).
`init.lua` calls SSG palette commands to configure the editor. The first
callable command is `theme.define`, which takes a Lua table of named colors
and applies it to the running session's palette. A missing `init.lua` is not
an error (SSG runs with defaults); a script error is reported but does not
prevent the editor from starting.

Non-goals (explicit scope cuts, revisit only if they prove limiting):
- Hot-reload of `init.lua` while running (a future `config.reload` command
  could re-run it; not built here).
- Any command beyond `theme.define` (`cycleTheme` etc. are mentioned by the
  user as future examples of the same argument-passing mechanism, not
  required now — there is not even a multi-theme registry to cycle over
  yet).
- A workspace-level or second config file layered over the user one
  (Settings.h already has this `User`/`Workspace` scope split for typed
  settings; `init.lua` is user-scope only for now).
- Validating/normalizing arbitrary Lua argument shapes generically. Only the
  one shape `theme.define` needs (a flat string→string table) is supported.

## Design

**Where `init.lua` lives.** One path per OS, resolved by a new platform
primitive mirroring the existing `userCacheRoot()` (`include/ssg/
platform_files.h`, `src/platform/linux_files.cpp` / `windows_files.cpp`):

- Linux: `userConfigRoot("ssg")` → `$XDG_CONFIG_HOME/ssg` if `XDG_CONFIG_HOME`
  is set and absolute, else `$HOME/.config/ssg` (same fallback shape
  `userCacheRoot` already uses for `XDG_CACHE_HOME`/`~/.cache`). The init
  script is `userConfigRoot("ssg") / "init.lua"`.
- Windows: `userConfigRoot("ssg")` → `%APPDATA%\ssg` (roaming config;
  deliberately NOT `%LOCALAPPDATA%`, which `userCacheRoot` already uses for
  local/disposable cache — config is the thing a user backs up/syncs, cache
  is not). Same `GetEnvironmentVariableW` two-call sizing pattern as
  `windows_files.cpp`'s existing `user_cache_root`.

Both throw on an unresolvable root (missing `HOME`/`APPDATA`), same as
`userCacheRoot` — the caller (`ssg_main.cpp`) catches this the same way it
already handles other startup path failures and falls back to running
without a config script rather than refusing to start.

**Where it runs in startup.** `ssg_main.cpp` already does: create runtime →
attach client → dispatch `file.open` → enter render loop
(`doc/spec-fast-startup.md`). Add one step between attach and `file.open`:
resolve the init script path, read it if present, and evaluate it through a
`LuaCommandHost` constructed for this purpose. If the file does not exist,
skip silently (not an error). If it exists but fails to read or fails to
evaluate (syntax/runtime/budget error), print a one-line diagnostic to
stderr and continue starting — a broken config script must never block
opening the editor.

**Sandbox reuse, not a new one.** `LuaCommandHost` already provides
everything the "sandbox" requirement asks for: a fresh Lua 5.4 state with
only `base`/`table`/`string`/`math`/`utf8` opened (no `io`, `os`, `package`,
`debug`, `dofile`, `loadfile`, `load`), an instruction budget and a
wall-clock budget enforced via a Lua hook, and a capability-checked
dispatcher boundary (`InvocationPrincipal{pluginId, InvocationOrigin::Lua,
capabilities}`). The init-script host is a plain `LuaCommandHost` instance
built with `evaluate()` called once; it does not need `invoke()` (no
registered Lua-side commands are called back into during startup) or the
`expose()`/handle-table facilities (init.lua does not need a live C++
object reference — it only calls one-shot configuration commands).
Construct it with every capability the init-script commands
(`theme.define` today) declare as required, since it runs at `System`-like
trust (it is the user's own machine-local file, not a remote/plugin input)
— but still route it through `InvocationOrigin::Lua` and the normal
capability check, so a future capability-gated command added to the catalog
is denied by default rather than silently trusted.

**The missing dispatch seam: a synthetic attached Lua client.**
`EditorRuntime::dispatch(ClientId, ClientCommand)` resolves its
`CommandContext`'s principal from whatever `InvocationPrincipal` was
registered for that `ClientId` at `attach()` time (`include/ssg/
EditorRuntime.h`) — there is no per-call principal override. Nothing in
today's codebase attaches a Lua-origin client in production (the only
attach call is `ssg_main.cpp`'s interactive `InProcess` client); this seam
must be built, not assumed. Concretely: `ssg_main.cpp` calls
`runtime.attach(InvocationPrincipal{luaClientId, InvocationOrigin::Lua,
initScriptCapabilities}, someViewId)` ONCE for the init-script host,
BEFORE evaluating `init.lua`, and the `LuaCommandHost`'s `LuaDispatcher`
lambda closes over `luaClientId` and calls `runtime.dispatch(luaClientId,
ClientCommand{commandId, runtime.revision(), payload})` for every
`ssg.command(...)` call. This is what makes "runs at `InvocationOrigin::
Lua` with a named capability grant, capability-checked like any other Lua
caller" (this spec's own Risk mitigation) actually true rather than
aspirational — without this attached client, either the call has no
principal to resolve (undispatchable) or someone takes a shortcut and
routes it through the INTERACTIVE client's `InProcess` principal, silently
granting init.lua whatever capabilities the interactive session has
instead of the intentionally-scoped grant list.

**Argument passing (the missing piece).** Today `ssg.command(id)`
(`src/LuaCommandHost.cpp`'s `commandCallback`) calls the dispatcher with
`LuaInvocation{commandId, principal}` — no argument slot exists; every
catalog command reachable from Lua today is implicitly zero-argument.
`theme.define(table)` needs a table argument to reach the native command's
`std::any` payload. Extend `ssg.command(id, args)`: `args` is an OPTIONAL
second parameter. When present, `commandCallback` requires it to be a Lua
table whose keys and values are both strings (`luaL_checktype` +
`lua_next` walk, matching the case-insensitivity/whitespace strictness of
nothing — keys/values are taken verbatim), builds a
`std::unordered_map<std::string, std::string>`, and passes it to the
dispatcher inside `LuaInvocation` as a new optional field (e.g.
`std::optional<std::unordered_map<std::string, std::string>> arguments`).
A non-table second argument, or a table with a non-string key/value, is a
`LuaError::InvalidScript`-class rejection (raised before the dispatcher is
called, so a malformed call can never reach a command handler). The
dispatcher glue that turns a `LuaInvocation` into a `ClientCommand` payload
(session-assembly code, not `LuaCommandHost` itself — mirrors how the
existing zero-arg path already works) wraps that map into whatever
payload type the target command's descriptor expects; for `theme.define`
that is a dedicated `ThemeDefineArguments{colors}` struct, not the raw map,
so the command handler's `std::any_cast` stays exactly as typed as every
other command's (see `src/EditCommands.cpp`'s pattern).

This flat string→string shape is the ONLY argument shape built now — it is
enough for `theme.define`'s color table and is the simplest useful
generalization beyond zero args. A richer Lua↔C++ value marshaling layer
(nested tables, numbers, arrays) is explicitly not attempted; if a later
command needs more than flat string keys/values, that is a new spec.

**`theme.define`'s color table.** SSG's palette (`include/ssg/Theme.h`) is a
fixed 16-slot indexed palette (`kThemePaletteSize`) — this is the classic
ANSI 16-color slot set, needed because SSG also renders on 16-color
terminals by index, not just truecolor. `theme.define`'s table keys are
those 16 ANSI slot names (`black`, `red`, `green`, `yellow`, `blue`,
`magenta`, `cyan`, `white`, `brightBlack`, `brightRed`, `brightGreen`,
`brightYellow`, `brightBlue`, `brightMagenta`, `brightCyan`,
`brightWhite`), each optionally mapped to a `"#rrggbb"` hex string. The
table may be partial: any name not present keeps the CURRENT active
theme's color for that slot. `theme.define` does NOT change which
semantic role or syntax scope points at which slot — the active theme's
`RoleMapping`/`SyntaxMapping` tables are reused unchanged; only the
`palette` RGB values are replaced. This keeps the feature genuinely "a
table of named colors" (the user's own phrasing) rather than requiring a
full theme file's role/syntax assignments as well, while still producing a
complete, renderable `ThemeSnapshot` (recomputing `DiffTints`/
`selectionFill` via the existing `deriveDiffTints`/`deriveSelectionFill`
over the new palette, since those are pure functions of palette + role/
syntax indices, unaffected by this change's scope).

An unknown key in the table (not one of the 16 names) or a value that
isn't a valid `"#rrggbb"` string is a command-level rejection
(`CommandHandlerResult::failure`), not a Lua-level error — the table shape
was valid Lua (strings to strings), the CONTENT was invalid, which is the
command handler's job to validate, same as any other command's argument
validation.

`theme.define`'s effect: replace `EditorRuntime::Impl::theme` (currently
initialized once from the hardcoded `defaultTheme()`,
`src/EditorRuntime.cpp`) with the new `ThemeSnapshot`, and publish whatever
delta the client-facing snapshot layer already uses for a theme change (this
project has no theme-change delta today because nothing mutates `theme`
after construction — adding one is in scope; its exact shape is an
implementation-time decision, not fixed here beyond "existing snapshot/delta
conventions apply, no new transport").

## Invariants

- **No new Lua sandbox**: `theme.define` and any future init-script command
  are ordinary `LuaCommandHost` catalog commands, capability-checked and
  budgeted exactly like every other Lua-callable command
  (`doc/features/language-services.md`). Nothing about init-script loading
  gets its own bespoke trust or execution model.
- **A broken/missing config never blocks startup.** Absent file → silently
  skipped. Present-but-broken file → one-line stderr diagnostic, editor
  still opens with default settings/theme.
- **Command payload typing stays strict.** The Lua→native argument bridge
  produces a flat string map at the Lua boundary, but each command's
  handler receives its own typed struct via `std::any_cast`, not the raw
  map — no handler downstream of the bridge does its own map-key parsing.

## Considerations

- **Path resolution failure vs. absent file are different.** `HOME`/
  `APPDATA` being unset is an environment problem (rare, but real in
  minimal containers); `init.lua` not existing is the normal no-config
  case. Both must result in "start with defaults," but path-resolution
  failure is worth a diagnostic (environment is unusual) while an absent
  file is not (this is the expected common case for most users).
- **Palette slot naming choice.** Using the 16 classic ANSI names (not
  `SemanticRole` names like `foreground`/`gitAdded`) is a deliberate
  mechanism choice: it matches how every other terminal color-scheme config
  format names things (a user coming from any terminal emulator's config
  already knows these names), and it sidesteps needing 32 semantic-role
  colors (`kSemanticRoleCount`) to fit inside a 16-slot indexed palette. A
  later spec could add a SEPARATE role-remapping command if direct
  role→color control is wanted; this spec only replaces the 16 palette
  RGB values.
- **One init-script host per process, not per session/client.** `init.lua`
  configures the running SSG PROCESS (single TUI instance, single
  workspace) once at startup; it is not re-evaluated per attached client
  or per workspace. This matches today's single-process TUI shape
  (`apps/ssg_main.cpp`) and is why the host is constructed once in
  `ssg_main.cpp`, not inside `EditorRuntime`.
- **`ThemeDefineArguments` bypassing `std::unordered_map` at the handler
  boundary** is a small but real design point: without it, EVERY consumer
  of the command dispatch table would need to know the Lua-bridge's map
  shape, which leaks the Lua integration into command handlers that should
  be origin-agnostic (the same `theme.define` command must also be
  reachable from `InvocationOrigin::InProcess`/`Websocket` with a
  non-Lua-sourced payload, e.g. a future settings-UI).

## Risks and Mitigations

- **Risk**: a slow or infinite-looping init script delays every startup.
  **Mitigation**: reuse `LuaCommandHost`'s existing instruction/wall-clock
  budget unchanged — no new budget tuning, no exemption for init scripts.
- **Risk**: capability creep — init.lua runs with elevated trust since it's
  a local file, which could become a precedent for skipping capability
  checks elsewhere. **Mitigation**: init.lua is granted capabilities
  explicitly and by name (whatever `theme.define` declares), not a
  wildcard/all-capabilities grant, and still goes through
  `InvocationPrincipal::hasCapability` like every other Lua caller — "runs
  at startup" is not a bypass of the capability system, it is a specific,
  named grant list.
- **Risk**: Windows path convention mismatch (config vs. cache root) causes
  confusion if a later feature reuses `userConfigRoot` incorrectly.
  **Mitigation**: name and doc-comment the primitive distinctly from
  `userCacheRoot`, stating the roaming-vs-local distinction inline.

## Acceptance (Definition of Done)

- Observable: placing `~/.config/ssg/init.lua` (Linux) with
  `ssg.command("theme.define", { red = "#ff5555" })` changes the running
  editor's rendered red palette slot; removing the file, or leaving it
  absent, starts with the unmodified default theme; a syntactically broken
  script prints a diagnostic and still starts normally.
- Budgets: unchanged — the existing `LuaCommandHostOptions` instruction/
  time budget applies to init-script evaluation with no special-casing.
- Gates: `bash scripts/check.sh` (tree-sitter ON) and
  `BUILD_DIR=build-no-ts bash scripts/check.sh` (OFF) both green.
- Oracles:
  - `userConfigRoot` unit tests mirroring `test_platform_files.cpp`'s
    existing `userCacheRoot` cases (XDG env set/unset/relative-and-ignored,
    Windows APPDATA present/missing), proving path shape and rejection of
    a non-single-component application name.
  - A `LuaCommandHost` test proving `ssg.command(id, table)` reaches the
    dispatcher with the table decoded as a string map, and that a
    non-table or mixed-type second argument is rejected BEFORE the
    dispatcher is invoked (dispatcher-not-called is the falsifiable part,
    not just "returns an error").
  - An integration oracle for the synthetic Lua client seam: dispatch a
    capability-gated command through the attached Lua-origin client and
    assert it is REJECTED when the init-script capability grant omits that
    capability, and ACCEPTED when granted — proving the call actually
    traverses `InvocationPrincipal::hasCapability` under
    `InvocationOrigin::Lua`, not an accidental bypass through the
    interactive client's `InProcess` principal.
  - A `theme.define` command-handler test: full 16-key table replaces
    every slot; a partial table leaves untouched slots exactly equal to
    the prior theme's values (byte-for-byte `SrgbColor` equality, not
    "looks similar"); an unknown key or malformed hex string is rejected
    with the ORIGINAL theme snapshot unchanged (all-or-nothing, no partial
    apply on error).
  - An `ssg_main.cpp`-level integration test (or the existing PTY harness
    pattern from the diff-colors session, `/tmp/drive_ssg_diff_check.py`
    style) proving a real `init.lua` on disk changes rendered SGR bytes
    for a palette slot, and that a missing file changes nothing and prints
    nothing.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `userConfigRoot(applicationName)` platform primitive (Linux XDG_CONFIG_HOME/~/.config, Windows %APPDATA%) | `include/ssg/platform_files.h`, `src/platform/linux_files.cpp`, `src/platform/windows_files.cpp`, `tests/test_platform_files.cpp` | test: mirrors existing `userCacheRoot` cases | - |
| 2 | Extend `ssg.command(id, args)` to accept an optional Lua string→string table and thread it through `LuaInvocation` | `include/ssg/LuaCommandHost.h`, `src/LuaCommandHost.cpp`, `tests/test_lua.cpp` | test: dispatcher receives decoded map; malformed second arg rejected pre-dispatch | - |
| 3 | Add `theme.define` command (descriptor + handler + `ThemeDefineArguments`), wired into the command registry alongside other command sets, INCLUDING the P0 command-catalog wiring (`src/EditorSessionBuilder.cpp` and `data/required-commands.json`/its Lua-parity tests) so registry-completeness and Lua-catalog-parity invariants stay intact | wherever theme-affecting commands are registered (new `ThemeCommands.h/.cpp` alongside `EditCommands.cpp`'s pattern), `src/EditorRuntime.cpp` (theme mutation + delta), `src/EditorSessionBuilder.cpp`, `data/required-commands.json` | test: full/partial/invalid table cases against `ThemeSnapshot` equality; catalog-parity test still passes with the new entry | - |
| 4 | Attach a synthetic Lua-origin client at startup and wire init-script loading into `ssg_main.cpp` (resolve path, read, `runtime.attach(InvocationPrincipal{..., InvocationOrigin::Lua, ...})`, construct `LuaCommandHost` with a dispatcher closing over that ClientId, `evaluate()`, diagnostic-and-continue on failure) | `apps/ssg_main.cpp` | test: PTY-harness-style real-binary run with a real `init.lua` on disk; capability-gated command accepted/rejected per grant list | - |
| 5 | End-to-end verification + both gates | - | both `scripts/check.sh` configs green | - |

## Rationale

The user's own phrasing ("a table of named colors") is taken literally as
the palette-slot-name shape rather than a semantic-role shape, because it
is both the simpler mechanism (no need to cover 32 semantic roles to
produce a legal 16-slot palette) and the more portable one (ANSI slot names
are a decades-old, widely recognized config vocabulary; semantic role names
are SSG-specific and would need their own stable public naming contract
before being safe to expose to user scripts). `theme.define` intentionally
does not touch role/syntax mappings so that a user only overriding a couple
of colors (e.g. just `red`) does not have to also specify all 32 role
assignments to get a valid theme — the active theme's role assignments are
a sensible default they're layering color changes on top of.

`LuaCommandHost` already existing, fully built, and NOT yet wired into
`EditorRuntime`/`ssg_main.cpp` at all, is the biggest simplification this
spec gets to lean on: the "sandbox" ask is already solved (restricted
stdlib, instruction/time budgets, capability checks) by a previous
milestone (`doc/features/language-services.md`, Plan step 3) that was built
ahead of having a caller. This spec's job is almost entirely "wire it up
and add the one missing piece" (argument passing) rather than designing a
scripting sandbox from scratch.
