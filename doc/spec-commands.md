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

## Status

C0–C6 are delivered (`aa70d7d`), and the two unread surface flags `keymap` and
`palette` were deleted afterwards (`9268311`) once it emerged they had been
copied into `CommandSpec` from the JSON with no consumer — the same defect the
catalog migration was meant to remove. Adding a command is now two files, or
three with a key chord. `owner` survives with exactly one consumer, the doc
generator's grouping.

# Invocation

Everything above concerns what a command *is*. This part concerns how one is
*invoked*, which the sections above deliberately left alone and which turned out
to carry the migration's remaining cost.

## Why names are the wrong currency on the keystroke path

The catalog made a command's declaration compiled and singular, but a command's
*identity* was still a `std::string` id, and a key's identity was still a
`std::string` code. So the most common operation in a text editor — typing a
character — did this:

1. the terminal decoder **allocated** a `std::string` naming the key,
2. the keymap was scanned linearly, comparing that name against every binding's
   stroke names and every binding's context name,
3. dispatch **constructed another `std::string`** to hash against the registry.

Strings are the right currency at three boundaries, and only there: the palette
(which displays and searches names), `init.lua` and the Lua API (which name
commands in script), and the wire protocol (which must serialise them). None of
those is the keystroke path. The TUI is the priority, and it was paying the full
cost of a generality it never used.

The fix is not to remove names. It is to stop treating a name as the *identity*
of a thing that already has one.

## The two identities

**`CommandHandle`** (`include/ssg/CommandHandle.h`) is a command's index into the
catalog, in a `std::uint16_t`. The catalog is compiled, fixed and ordered, so an
id string was only ever *one name* for a row that already had a perfectly good
index. A handle is minted by resolving a name **once** — at build or
keymap-compile time — and carried by value thereafter. It resolves back to its
`CommandSpec`, so any boundary that genuinely wants the string still gets it.

It lives in its own header, apart from `Commands.h`, because the registry and the
session need to *name* a command without depending on the catalog's data: every
summary, argument shape and documentation line. Identity is small; the catalog is
not.

**`CommandRef`** is how a caller names the command it wants to invoke, and it
exists because a dispatch target carrying a name *and* a handle as independent
fields could carry two different commands, with no principled answer as to which
wins. A ref holds one identity and derives the other: built from a name it
resolves the handle once; built from a handle the name comes free from the
catalog. `ClientCommand::id` is a `CommandRef`, so disagreement is not
representable rather than merely detected, and `name()` is always available — a
rejected dispatch can always say which command it rejected.

A ref, not a bare handle, is also what a compiled binding stores. `keymap.bind`
accepts any non-empty command id, so a binding may name a command the catalog
does not have — a typo, or a command removed since the config was written. Such
a binding still resolves, and the name it carries is the only thing the
resulting `UnknownCommand` rejection can report.

**`KeyCode`** (`include/ssg/KeyCode.h`) is a closed enum of every key the decoder
can name. `KeyStroke::code` is a `KeyCode`, so the decoder emits identity
directly and allocates nothing per keystroke.

The enum, each key's wire name and its display name are **one table**
(`kKeyCodes`), with a `consteval` check proving the table covers the enum exactly
and in declaration order. A key cannot be added without all three. That table
also replaced `keyDisplay`'s twenty-branch string chain: the short form rendered
in the leader hint is now simply the table's display column.

Letters and digits are **contiguous** in the enum by design, so the decoder folds
a printable byte to its key with arithmetic rather than a lookup. This is a real
constraint on the enum's ordering and is commented as such.

## The compiled keymap

`KeymapViewState` remains the authored, transportable keymap: strings, because
that is what a config file, `keymap.bind` and the protocol speak. It is the
source of truth and is unchanged.

`CompiledKeymap` (`include/ssg/CompiledKeymap.h`) is a **derived index** of it,
rebuilt exactly when the authored keymap changes — a `keymap.bind`, a config
reload — and never per keystroke. It resolves command ids to `CommandHandle`s.
Because the decoder now supplies a `KeyCode`, `CompiledStroke` packs key and
modifier bits into a single `std::uint32_t`, and the compiled keymap needs no key
intern table at all.

A binding's context needs no intern table either: a keymap context is already a
closed set — `"*"` plus the `FocusTarget` names — so `CompiledContext` stores the
focus a binding applies to, the global context, or `Never`. `Never` is
load-bearing: an unrecognised context name must match no keystroke, exactly as
the authored matcher's name comparison does, and folding it into the global
context would turn a rejected binding into one active everywhere. Resolution
takes the `FocusTarget` the client already holds, so no context name is built,
hashed or compared on the keystroke path.

Resolution applies **the same rules** `KeymapMatcher` applies — first eligible
match wins, a `"*"` binding upgrades a focus-specific match, a strict prefix is
`Pending` — expressed over integers. It decides no policy of its own; a
divergence between the two would be a bug, not a design.

The scan remains linear over bindings, because it must detect *prefixes* to
report `Pending` for a partial chord. At ~61 bindings compared as integers this
is not worth indexing, and a map could not answer the prefix question anyway.

## What the path costs now

| Stage | Before | After |
|---|---|---|
| Printable typed | `std::string` allocated per keystroke | nothing allocated |
| Stroke → identity | string compare per binding | `std::uint32_t` compare per binding |
| Registry dispatch | `std::string` constructed, then hashed | array index |
| Leader hint | twenty-branch prefix-strip chain | table column |

A key the keymap never mentions — an ordinary printable — now fails every binding
on an integer compare. Text insertion is still architecturally the *fallthrough*
when no binding matched, which is discussed under Considerations below.

## Invariants

- **V1** A command's identity on the keystroke path is a `CommandHandle`, carried
  in a `CommandRef` that can always name the command. Command id strings are
  resolved at construction or keymap-compile time, never per keystroke, and a
  command that cannot be identified can still be named in a diagnostic.
- **V2** `CompiledKeymap` is derived from `KeymapViewState` and owns no policy.
  Its resolution rules must match `KeymapMatcher`'s; where they disagree,
  `KeymapMatcher` is correct and the compiled form is wrong.
- **V3** `KeyCode` is the sole spelling of a key's identity. A key's enumerator,
  wire name and display name are declared together in `kKeyCodes`.
- **V4** Names remain authoritative at the palette, Lua and wire boundaries.
  Nothing on those boundaries may be converted to handles for speed.

## Considerations

- **Two spellings of a keymap now exist**, authored and compiled, and they must
  not disagree. The compiled form is rebuilt whenever the authored one changes
  and derives every binding from it, so divergence requires a bug in
  `CompiledKeymap`'s constructor rather than ordinary drift. This is guarded by
  `compiledKeymapResolvesIdenticallyToTheAuthoredMatcher`, a differential test
  that compares the two resolvers over adversarial keymaps (duplicate globals, a
  global shadowing a focus binding, prefix chains, an unknown context) rather
  than pinning either one's answers — an oracle would state the rules twice and
  still not prove they agree.
- **The pending chord has two forms**, compiled for matching and authored for the
  leader hint and the app-local quit chord. `PendingChord` in `apps/ssg_main.cpp`
  owns both because six call sites clear the chord, and a clear that forgot one
  form would leave resolution matching strokes the user had abandoned.
- **A binding naming an unknown key is now rejected** rather than silently
  producing a binding no key can trigger. `KeymapErrorCode::InvalidStroke`
  already existed for this; it now fires. This is a behaviour change and an
  improvement, but it is a behaviour change.
- **The decoder does not emit function keys**, though the key codec has always
  accepted `F5` and a test asserted it round-trips. `KeyCode` names F1–F12 to
  preserve that parity, so a binding on one still parses and still cannot fire.
  The free-string design hid this gap; closing it is separate work.
- **Prompt input bypasses all of this.** Backspace and text in a prompt are
  handled by two hardcoded if-chains in `apps/ssg_main.cpp` that never reach the
  catalog or the registry, and the two chains test `find` and `replace` in
  opposite orders. The same physical key is a first-class command in the editor
  and a special case in a prompt. This asymmetry predates the fast path and is
  not addressed by it.
- **Text insertion is defined by failure.** A printable is pushed onto the chord,
  fails to match, and is routed as text from the no-match branch. This is cheap
  now, but it means the editor's most common operation is the default case of a
  fallthrough rather than a stated rule.
- **Origin.** This part was built as a spike (`cb0893f`, `955bb37`) on
  `spike/handle-dispatch` to find the simplest thing that could work, explicitly
  without tests, and was then reviewed and hardened. It compiles, the full suite
  passes, and the binary was driven over a pty to confirm printables, Backspace,
  CSI Delete and both chords behave identically to `master`. Review found the
  dual-identity dispatch hazard, the lost diagnostic on the handle path, and
  per-keystroke context string work; all three are fixed above. V2 is guarded by
  a perturbation-verified differential test, and V1's naming guarantee by
  `compiledKeymapCarriesTheNameOfAnUncataloguedCommand`; V3 and V4 rest on the
  type system rather than on tests.

## Acceptance

- No `std::string` is allocated or compared to route a keystroke to a handler.
- The authored keymap remains the source of truth and the only thing a user or a
  config file edits.
- Palette, Lua and protocol behaviour are unchanged.
- `CompiledKeymap` and `KeymapMatcher` resolve identically; verified by
  perturbation (breaking the global-precedence upgrade, the `Never` context, and
  prefix detection each fail the differential test).
