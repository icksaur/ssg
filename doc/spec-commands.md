# spec-commands

## Goal

One boring, compiled home for what a command *is*: `include/ssg/Commands.h` and
`src/Commands.cpp`. Today a command is a row smeared across nine structures, and
the declared catalog does not compile at all.

## Current state (verified in code)

`p0CommandDescriptors()` (`src/EditorSessionBuilder.cpp:49`) is the compiled
aggregation and is authoritative for *existence*: `build()` throws unless the
bound handlers exactly equal it. But it aggregates 23 hand-written arrays, and
each attribute lives elsewhere:

| Attribute | Location | Form |
|---|---|---|
| id | 23 command-set headers | `std::array<XCommandDescriptor, N>` |
| handler | `src/runtime/*.cpp` | `builder.bind()` calls |
| capability | `EditorSessionBuilder.cpp:39` | one hardcoded `if` |
| effect | `EditorSessionBuilder.cpp:42` | hardcoded `Mutation` for all 182 |
| argument codec | `Protocol.cpp:5361+` | ~20-branch `if/else` on id |
| Lua allowlist | `InitScriptCatalog.h` | separate 5-entry array |
| Lua marshalling | `apps/ssg_main.cpp` | another if-chain |
| palette label | `src/command_metadata.cpp:12` | `kLabels` table |
| declared contract | `data/required-commands.json` | **not compiled** |

The JSON is test-only: its four `SSG_REQUIRED_COMMANDS_PATH` definitions are all
in test blocks, six test files read it (`command_catalog.h`,
`runtime/command_cases.h`, `test_editor_session_assembly.cpp`, `test_lua.cpp`,
`test_protocol.cpp`, `test_required_commands.cpp`), and `strings build/ssg`
finds no reference. Its `owner`, `keymap` and `palette` fields are read by **no
production code**.

**Correction to an earlier reading of this evidence.** `lua: true` on ~180
commands is *not* fiction. `LuaCommandHost` takes a per-host command list
(`LuaCommandHost.h:71`), so `lua` means "eligible for the versioned Lua API"
— which is I20 parity. `kInitScriptCommands` is a different, narrower axis:
which commands *init.lua at startup* may call, currently 5, gated on having
argument marshalling in `dispatchInitScriptCommand`. Any design that collapses
these into one flag either shrinks Lua parity to 5 or grants startup authority
to 180.

## The invariant this changes

`doc/spec.md:45` states the catalog is "an independently reviewed transcription,
never a list generated from the implementation". That rule exists so the public
command surface is a reviewed decision rather than whatever the code happens to
do. It is worth keeping in spirit and changing in mechanism.

**Proposed replacement:** the catalog is a hand-authored declaration in
`Commands.cpp`, reviewed as data, that the implementation must satisfy — not a
list scraped from handlers. It is not "generated from the implementation": no
tool derives it from `builder.bind()` calls. It is the same reviewed
transcription, expressed in the language that compiles, so it cannot drift from
the code it governs. Documentation is generated *from the catalog*, never the
reverse.

This must be approved explicitly at review; if rejected, the alternative is
`configure_file` generating `Commands.cpp` from the JSON, keeping the JSON
authored.

## Design

`Commands.h` declares the types. `Commands.cpp` holds one table of every
command; it may depend on whatever the specs need (argument types, capability
ids), because it is the one place that is allowed to know everything.

```cpp
struct CommandSpec {
    std::string_view id;
    std::string_view owner;        // the feature that implements it
    std::string_view label;        // palette display; empty = humanise the id
    std::string_view summary;      // one line, used for generated docs
    CommandEffect effect;
    std::span<CapabilityId const> requiredCapabilities;
    ArgumentKind argument;         // None, TextInput, ScrollLines, ...
    CommandSurfaces surfaces;      // keymap, palette, lua
};
```

`CommandSurfaces` carries the exposure bits, as **two separate axes** because
they answer different questions:

```cpp
struct CommandSurfaces {
    bool keymap;     // a bare key chord may invoke it
    bool palette;    // it appears in the fuzzy palette
    bool luaApi;     // callable through the versioned Lua API (I20 parity)
    bool initScript; // init.lua at startup may call it
};
```

`luaApi` is the I20 parity bit and is true for nearly every command;
`initScript` is the startup allowlist that `kInitScriptCommands` holds today,
true for 5. `initScript` implies `luaApi`. Keeping them separate is what lets
Lua exposure grow **gradually**: a command opts into `initScript` once its
argument marshalling exists, one field at a time in `Commands.cpp`, without
touching I20 parity or a second allowlist.

`ArgumentKind` replaces the `if/else` codec chain: `Protocol.cpp` maps kind to
codec once, in a table, instead of matching ids.

Derived consumers, all reading the one table:
- `p0CommandDescriptors()` projects id/effect/capabilities.
- The protocol argument-codec registry maps `argument`.
- `commandLabel()` reads `label`.
- The init-script host filters `surfaces.initScript`; a general Lua host filters
  `surfaces.luaApi`.
- A generator emits `doc/commands.md` (see below).

**Documentation generation.** CMake cannot read a C++ table at configure time,
so the mechanism is a build-time generator, not `configure_file`: a small
`ssg_command_docs` executable links `Commands.cpp`, walks the catalog, and
writes `doc/commands.md`. An `add_custom_command` runs it whenever
`Commands.cpp` changes, and a test asserts the checked-in `doc/commands.md`
matches what the generator produces (failing with the regeneration command), so
the doc cannot drift and is never hand-edited. This keeps ONE source: the
generator has no table of its own, only formatting.

**JSON policy.** `data/required-commands.json` is deleted. JSON in this project
is for HTTP/wire formats only, not for internal catalogs.

## Invariants

- **C1** Every command appears exactly once in `Commands.cpp`. Nothing else
  declares a command id.
- **C2** Bound handlers exactly equal the catalog; `build()` already enforces
  this and keeps doing so.
- **C3** `surfaces.initScript` is the only truth for what `init.lua` may call,
  and `surfaces.luaApi` the only truth for I20 parity. No second allowlist, and
  `initScript` implies `luaApi`.
- **C4** Generated documentation is derived from the catalog and never edited by
  hand.
- **C5** No id-matching `if`-chain anywhere: attributes are looked up, not
  branched on.

## Considerations

- **I20 needs rewording too.** `doc/spec.md:153` claims every user-visible
  command is Lua-callable. That is true of `luaApi` eligibility and false of
  what `init.lua` can reach today (5). The invariant should distinguish the two:
  the Lua *API* offers parity, while a *host* is configured with the subset it
  grants, and `init.lua` is one such host. Without this the spec looks like it
  is quietly narrowing I20.
- **This is a large mechanical change** (23 command sets, ~182 rows). It must be
  behaviour-preserving, and the existing catalog-vs-registry tests are the
  safety net during the move. Doing it in one commit is a mistake; the plan
  below moves attributes one at a time with the gate green between each.
- **`std::span<CapabilityId const>` in a constexpr table** needs the capability
  arrays to have static storage. If that proves awkward, a small fixed-capacity
  inline array is the fallback; capabilities are 0 or 1 today.
- **Losing the independent transcription is a real cost**, not a free win. The
  mitigation is that `Commands.cpp` is reviewed as data and a command's
  existence is still an explicit authored line — a reviewer sees a new row in a
  diff exactly as they saw a new JSON line.
- **`owner` currently has no consumer.** It stays only because generated docs
  group by it; if the generator does not use it, it should not exist.

## Plan

| # | Step | Files | Check |
|---|------|-------|-------|
| C0 | Add `Commands.h`/`Commands.cpp` with the full table. Its equality oracle covers only the fields C0 does not change: ids, effect, capabilities, and the argument-kind mapping, against both `p0CommandDescriptors()` and the JSON, both directions. Surface metadata is NOT compared here -- `initScript` is a new axis the JSON never carried, so comparing it would assert a fiction | `include/ssg/Commands.h`, `src/Commands.cpp`, `tests/` | ids/effect/capabilities/argument equal in both directions |
| C1 | Reproject `p0CommandDescriptors()` from the table; delete the capability `if` and the blanket `Mutation` | `src/EditorSessionBuilder.cpp` | gate green, no behaviour change |
| C2 | Move argument codecs to `ArgumentKind` lookup | `src/Protocol.cpp` | codec registry test; round-trips unchanged |
| C3 | Move labels from `kLabels`; delete `command_metadata.cpp`'s table | `src/command_metadata.cpp` | palette label test |
| C4 | Adopt the two surface axes: `initScript` reproduces exactly today's `kInitScriptCommands` set, `luaApi` reproduces today's `lua` flags. Delete `InitScriptCatalog.h` | `apps/ssg_main.cpp` | `init.lua` reaches exactly the 5 exposed commands, no more and no fewer; a general Lua host still reaches the `luaApi` set |
| C5 | Delete the 23 per-feature descriptor arrays | 23 headers | gate green |
| C6 | Add the `ssg_command_docs` generator and the doc-matches-catalog test; delete `required-commands.json` and its test loader | `cmake/`, `tests/command_catalog.h` | generated doc matches the table; catalog-vs-registry guards still fail on an unintended addition (verified by perturbation) |

Each step keeps the gate green on its own.

**What replaces the JSON as the addition guard.** After C6 the surviving guards
are `requiredCatalogEqualsAssembledRegistryExactly` (catalog vs assembled
registry), `build()`'s exact-handler rule, and the argument-codec registry
coverage check -- all of which fail on a command that exists without being
fully wired. What they do NOT do is force a *human decision* about widening the
public surface: a reviewer must see the new row in `Commands.cpp`. That is
weaker than a separately-authored file and is the accepted cost of this change;
it is mitigated by generated `doc/commands.md` appearing in the same diff, which
makes a surface change visible in review as a documentation change.

## Acceptance

- Adding a command touches `Commands.cpp` and its handler. Two files.
- No id-matching `if`-chain remains.
- `doc/commands.md` is generated and current.
- No JSON outside HTTP/wire use.
