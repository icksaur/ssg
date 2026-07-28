# spec-command-registry

## Goal

A command is declared where it is implemented, once, by the component that
offers it. Registration and implementation are the same act, so a command
cannot exist half-wired, and adding a feature's command touches only that
feature.

The catalog is **fully dynamic**: commands are added at runtime, handles are
issued on registration, and there is no point at which the catalog is declared
complete. It never will be complete — plugins and Lua extensions may add
commands at any time — so "complete" is not a property worth designing around.

This supersedes the compiled table in `src/Commands.cpp`
(`doc/spec-commands.md`), which is retained as the record of why the static
catalog was built and what it fixed.

## Why change a design that just landed

The static catalog fixed the right problem — a command's facts were smeared
across nine structures — by putting them in one table. But it put them in a
table *away from the code they govern*, which reintroduces the same class of
defect in a new place:

- **The catalog declares; the handler assumes, and nothing connects them.**
  `text.insert`'s row says `ArgumentKind::TextInput`; its handler `any_cast`s
  `TextInputArguments`. Change one and the mismatch appears at runtime as a
  crash, not at compile time as an error.
- **`effect` and `requiredCapabilities` are unguarded.** The test that appeared
  to check them, `compiledCatalogAgreesOnEffectAndCapabilities`, compares the
  catalog against `p0CommandDescriptors()` — which step C1 reprojected *from the
  catalog*. It compares the catalog to itself. It was meaningful during the
  migration and became a tautology the moment the migration finished.
- **Adding a command is still two files**, and the two must agree by hand.
- **A compiled table cannot describe a plugin's commands at all.**

## Design

### The builder

`CommandSpecBuilder` is a free-standing value. It is not owned by the catalog,
is not obtained from the catalog, and describes exactly one command. Each method
returns `*this`, so the fluent chain is on the builder — never on the catalog.

```cpp
catalog.add(CommandSpecBuilder{"text.insert"}
                .owner("text-input-commands")
                .summary("Insert text at every cursor")
                .mutates()
                .lua()
                .handler<TextInputArguments>(
                    [&impl](CommandContext& context, TextInputArguments const& text) {
                        return impl.insertText(context, text);
                    }));
```

| Method | Meaning | Required |
|---|---|---|
| `CommandSpecBuilder{id}` | The command id | yes |
| `.owner(name)` | The component offering the command; groups the reference | yes |
| `.label(text)` | Palette display text | no — defaults to the humanised id |
| `.summary(text)` | One line for the generated reference | yes |
| `.mutates()` / `.observes()` | `CommandEffect` | **exactly one** |
| `.capability(id)` | A required capability; repeatable | no |
| `.lua()` | Eligible for the versioned Lua API (I20) | no |
| `.initScript()` | `init.lua` may call it at startup; implies `.lua()` | no |
| `.handler<Args>(fn)` | The implementation; deduces the argument type | yes |
| `.handler(fn)` | An implementation taking no arguments | yes (alternative) |

**Effect is deliberately required rather than defaulted.** The predecessor to
the static catalog hardcoded `Mutation` for all 182 commands, and a default is
how that happens. Forcing `.mutates()` or `.observes()` makes "nobody thought
about it" unrepresentable.

**The builder is always valid, possibly incomplete.** It needs no terminator and
tracks no state: it is a description that accumulates. Completeness is not its
concern — see below.

### The catalog

```cpp
CommandHandle add(CommandSpecBuilder spec);
```

Taken **by value**, so a chained temporary and a named builder are both
accepted, and the handler moves in exactly once. (Taking `CommandSpecBuilder&`,
as some builder APIs do, compiles for a chained temporary — each fluent method
returns an lvalue — but rejects a bare `CommandSpecBuilder{"id"}`. That
inconsistency is an artefact of reference binding, not a designed guard.)

**`add` is not named `register`.** `register` is a reserved keyword in C++; a
member function cannot bear that name.

`add` is where every check happens, because a builder passed to `add` is
finished by definition — passing it *is* the completion signal. So:

- a missing required field throws `std::runtime_error` naming the command and
  the field;
- a duplicate id throws, naming the id and both owners;
- the name→handle map, the argument codec, and the descriptor are updated
  incrementally.

This is what removes any need for a completion step. There is no whole-set
question left to ask, because there is no moment at which the set is whole.

### Nothing copies the catalog

A dynamic catalog creates one new failure mode: a consumer holding a *snapshot*
of catalog-derived data silently misses commands registered afterwards. This
already exists in the codebase — `HttpEditorServer` takes a
`CommandArgumentCodecRegistry` **by value** and stores it
(`src/HttpEditorServer.cpp:478`), which was harmless against a fixed table and
is a staleness bug against a growing one.

**The rule turns on which side of the client boundary a consumer is on.**
Something in the runtime's own process can simply ask the catalog, so a copy is
pure liability. A client cannot — it holds a *projection* of session state,
refreshed by revision, exactly as it already does for the keymap, the theme and
every other section. So: in-process consumers hold the catalog; client
projections are revisioned copies. `CompiledKeymap` is the second kind and is
governed by R7, not by the rule below.

For in-process consumers the answer is not a propagation mechanism. It is to
delete the copies:

- **The argument codec registry is owned by the catalog**, not passed around as
  a value. Consumers hold a reference to the catalog and ask it to decode.
  `CommandArgumentCodecRegistry` stops being a constructor parameter of
  `HttpEditorServer`, `ProtocolCodec` and friends.
- The same rule applies to any future catalog-derived table: derive it on
  demand, or hold the catalog.

A consumer that cannot observe a late registration is then not a case that
needs handling, because it cannot be written. This is strictly less code than
the alternative — there is no versioned swap, no invalidation callback, and no
subscription list.

### Registering while the editor is running

Registration is a **session mutation** and is serialised with dispatch. This is
not a formality: `HttpEditorServer` decodes a command request *outside* the
session lock (`src/HttpEditorServer.cpp:201`, before `session.dispatch`) and does
so on per-connection threads, so once the codec registry lives in the catalog, a
decode can run concurrently with a plugin registering.

The contract:

- The catalog is guarded by a `std::shared_mutex`. `add` takes it exclusively;
  every read — decode, dispatch lookup, palette enumeration, doc walk — takes it
  shared.
- A shared lock is used rather than routing decode through the session mutex
  because decode is deliberately off that lock today; serialising every decode
  behind dispatch would be a throughput regression introduced by a refactor.
- Registration is expected to be rare (startup, plugin load, script evaluation)
  and reads frequent, which is exactly the case a shared mutex is for.
- Within a single `add`, the name→handle map, codec table and descriptor list
  are updated together under the exclusive lock, so no reader observes a command
  that is half-registered.

**What a read may keep.** A shared lock alone is not enough, because catalog
reads hand out references into catalog storage — `CommandRegistry::find` already
returns a `CommandRegistration const*`, and a spec's capabilities are a
`std::span`. If storage moved, a borrowed pointer taken under a lock would
dangle the moment a concurrent `add` reallocated, and the shared lock would have
protected nothing.

Rather than imposing a lifetime rule on every caller, **registered commands live
in stable storage** — a deque or chunked array, never a reallocating vector.
Combined with R4, this makes a borrow permanently safe:

- append-only means a registered command is never removed;
- stable storage means it never moves;
- tombstoning rather than compacting means its slot is never reused.

So a `CommandSpec const*` or `CommandRegistration const*` obtained from the
catalog is valid for the life of the process. The shared lock is required only
for the *lookup itself* — the name→handle map may rehash — and not for using
what the lookup returned. Callers need no discipline, which is the point: a
lifetime rule that must be remembered is a lifetime rule that will be forgotten.

This contract exists because "register at any time" is a real capability of this
design, not a hypothetical one. Nothing registers concurrently today; the
discipline is specified now so the first plugin implementation does not have to
discover it.

### The catalog has a revision

Clients are a different matter: a TUI or remote client does not hold the
catalog, it holds a *projection* of it, and `CompiledKeymap` is one.

The catalog therefore carries a **revision**, incremented on every `add`. It is
published to clients the way session state already is, alongside the keymap. A
client recompiles its keymap when the keymap changes **or** the catalog revision
changes, which is R7's rebuild trigger made concrete rather than left as an
obligation.

Staleness here is benign in one direction and that is worth stating: because
registration is append-only and handles are never reused (R4), a stale compiled
keymap can only *fail to resolve* a binding for a newly registered command. It
can never resolve a handle to the wrong command. The revision closes a
missing-binding window, not a misdispatch hazard.

### Handles

`add` returns the `CommandHandle` it issued, so a component can keep the handles
for the commands it registered and invoke them without touching a name.

Registration is **append-only**: a handle is an index, indices are stable under
append, and **a handle is never reused**. If a command is ever unregistered — a
plugin unloading — its slot is tombstoned rather than compacted, because
reusing an index would silently make a stale handle denote a different command.

### What replaces `ArgumentKind`

Nothing authored. `.handler<Args>` records the argument type once and derives
both the codec and the type-safe unwrap from it, so a handler cannot disagree
with its codec. The `std::any` `any_cast` moves out of every handler into one
place. A command with no arguments uses the `.handler(fn)` overload, which is a
*declaration* of "no arguments", not a fallthrough.

### Where registration lives

Each component exposes one registration function declared in an **internal**
header (`src/runtime/editor_runtime_internal.h`), not in `include/ssg/`:

```cpp
void registerTextInputCommands(CommandCatalog&, EditorRuntime::Impl&);
```

These are wiring, not API. Publishing them would make the registration
mechanism a public surface callers could invoke twice or out of order.

`EditorSessionBuilder::build()`'s exact-handler rule is **deleted**. It exists
to prove that every declared command has a handler and vice versa; when a spec
and its handler are registered in one expression, that is true by construction
and the check is vacuous.

### Keymaps bind late, by name

A keymap binds a command **name**, both in `defaultTerminalKeymap()` and in
`init.lua`. This is not a compromise — it is required. A binding may name a
command a plugin has not registered yet, and a keymap outlives any particular
catalog contents.

Consequences:

- `CompiledKeymap` resolves names to handles when it is built. It is already
  rebuilt when the keymap changes; it must **also** be rebuilt when the catalog
  changes, or a binding for a newly registered command would stay unresolved.
- A binding naming an unregistered command resolves to a `CommandRef` carrying
  the name with no handle. Dispatch rejects it as `UnknownCommand` **and can say
  which command** — the behaviour already delivered for uncatalogued bindings.
- The curated keymap is checked against the catalog after core registration.
  This is advisory at runtime, because a name may legitimately belong to a
  plugin, but a **test** asserts every command `defaultTerminalKeymap()` names
  is registered by core. That catches our own typos without constraining
  plugins. Today nothing checks this at all: `KeymapMatcher::validate` requires
  a command id to be non-empty, never that it exists, so a typo in the curated
  keymap ships as a chord that silently does nothing.

### Generating the reference

Documentation generation moves from a build step to a test.
`EditorRuntime::create` calls `std::filesystem::create_directories`
(`src/EditorRuntime.cpp:1765`), so running registration during a build would do
filesystem work at build time. `test_commands` constructs a runtime in a
temporary workspace — as the suite already does routinely — walks the catalog
after core registration, and asserts `doc/commands.md` matches. This deletes the
`ssg_command_docs` executable and its `add_custom_command`.

**Who runs what.** The author of a new command runs the suite with
`SSG_UPDATE_DOCS=1` and commits the regenerated reference in the same commit.
The push gate runs without it, so an un-regenerated reference fails the gate.
Generation is never implicit in a build: a documentation change is a reviewed
diff, not an artefact appearing unbidden in a working tree.

The reference documents the commands **core** registers. Plugin commands are
discoverable at runtime through the palette, not in a committed file.

## Invariants

- **R1** A command exists only by registration. There is no other way to make
  one dispatchable, and no list of ids anywhere else.
- **R2** A command's handler and its declared facts are registered in one
  expression. A handler's argument type is deduced, never restated.
- **R3** Duplicate registration is fatal, naming both owners. Every required
  field is checked at `add`.
- **R4** Registration is append-only, a handle is never reused, and registered
  commands live in stable storage, so a reference obtained from the catalog
  stays valid for the life of the process.
- **R5** Effect is explicit for every command; there is no default.
- **R6** `CommandHandle` is **process-local**. It is never persisted, never sent
  on the wire, and never compared across processes. Enforced structurally:
  `index()` is not publicly readable, and a deleted `toValue(CommandHandle)`
  overload makes encoding one a compile error.
- **R7** A keymap binds a command by name and resolves late. `CompiledKeymap` is
  rebuilt whenever the keymap **or** the catalog revision changes.
- **R8** No **in-process** consumer holds a copy of catalog-derived data: the
  codec registry and every other derived table are obtained from the catalog, so
  a late registration cannot leave a stale copy behind. Client-side projections
  are the exception and are revisioned instead (R7).
- **R9** During the migration only, a command id is registered dynamically
  **xor** present in the static table — never both, never neither.

## Migrating without two sources of truth

The migration leaves the catalog half-migrated across several commits, which is
the riskiest part of this plan: two mechanisms can declare a command, and "the
gate is green" does not prove they have not diverged.

**The rule (R9).** During migration the catalog is the union of dynamically
registered commands and the not-yet-migrated static rows. A component's commands
move as a whole; the moment `registerTextInputCommands` exists, that owner's
rows leave `Commands.cpp` in the same commit.

**Enforced by construction.** `add` throws on a duplicate id, so a command
declared in both places is fatal at startup rather than merely tested for.

**The transition oracle.** `Commands.cpp` keeps, for the migration only, a
frozen snapshot of the pre-migration catalog — data, not a declaration. It is
scoped to **core registration**: the oracle runs against a runtime built without
plugins and before `init.lua` is evaluated. Without that scoping a user's
plugin, or any later dynamic registration, would fail the migration oracle for
an entirely unrelated reason.

Pinning ids alone is **not** sufficient: a move that preserves every id can
still flip `effect` from `Mutation` to `Observation` (disabling the
stale-revision check), drop a required capability, or set `initScript` on a
command that did not have it — silently widening authority while satisfying an
id-only check. The snapshot therefore pins each command's **behavioural tuple**:

    { id, effect, requiredCapabilities, luaApi, initScript }

`label` and `summary` are deliberately excluded: they are cosmetic, a move is a
natural moment to improve them, and `doc/commands.md` shows any change in the
same diff.

**The snapshot and its test are deleted with `Commands.cpp` in D5.** They exist
to make the transition safe, not to become the next thing that must be
maintained.

## Considerations

- **Nothing can assert the catalog is complete, and that is the point.** The
  static design could prove "every declared command has a handler"; this design
  makes that unprovable because there is no fixed set to prove it over. What
  replaces it is stronger where it matters — a command *cannot* be declared
  without a handler, because they are one expression — and honest where it does
  not: with plugins, completeness was never going to be a real guarantee.
- **R4 is the invariant that will be violated first.** Handle reuse is the
  natural thing to write when a plugin unloads, and its symptom — a stale handle
  quietly invoking a different command — is silent and severe. Tombstoning
  belongs in the catalog from the start, even though nothing unregisters today,
  so that the first unload implementation cannot get it wrong.
- **R6 is the hazard this design creates.** With a static table a handle was an
  index into a compiled array, meaning the same command in every process. Now it
  means whatever registration order produced it, and that order varies with
  which plugins are loaded. The wire protocol speaks names, so this is sound
  today; the structural enforcement exists so it stays sound.
- **"Find all commands" gets harder, and this is a genuine loss.** Today
  `src/Commands.cpp` lists all 182 in one file. Afterwards they are spread
  across ~20 component files, and plugin commands appear in no file at all. The
  mitigations: `doc/commands.md` remains generated and committed for core
  commands, so a new one still appears in a reviewed diff; `commandsOwnedBy`
  answers "what does this component offer"; and the palette answers it at
  runtime, including plugins. This is a real trade — locality of *declaration to
  implementation* bought at the cost of locality of *declaration to
  declaration* — and it is the trade the project's stated goal asks for.
- **We lose the independently reviewed surface entirely.** `doc/spec.md:45`
  required the catalog to be a transcription reviewed apart from the
  implementation; the static-catalog migration already weakened this to "a
  hand-authored table reviewed as data". This design removes the separate
  artefact: the review surface is the diff of the component that gained a
  command, plus the regenerated reference. That is a deliberate, stated retreat,
  and `doc/spec.md:45` must be amended rather than quietly contradicted.
- **`init.lua` reload needs a scope if Lua ever registers commands.** Reload is
  "reset-then-reapply" — the script's current content is the whole
  customisation. If Lua-registered commands are added later, reload must remove
  the previous evaluation's commands (tombstoning their handles) before
  re-running, or the second evaluation collides with the first. Nothing
  registers from Lua today; this is recorded so the first implementation does
  not discover it the hard way.
- **This is the third catalog design in this project's history.** Scattered
  structures plus a non-compiling JSON; the compiled table; this. That is not an
  argument against it — the second design taught us that co-locating a command's
  *facts* is insufficient if they are not co-located with its *code* — but the
  churn is real and should be acknowledged rather than presented as a clean win.

## Testing

Most catalog tests stop being necessary. What remains:

- **Catalog mechanics** (unit, small): add issues a handle; duplicate throws;
  missing required field throws; handles stay valid across later adds; a
  **reference** taken before later adds is still valid and still names the same
  command; a tombstoned handle is never reissued; iteration is registration
  order.
- **Per-component registration** (unit, one per component): the component
  registers the commands it claims to. This is the functional requirement each
  component owns.
- **Curated keymap** (one test): every command `defaultTerminalKeymap()` names
  is registered by core.
- **R6** (one test): no protocol encoder emits a handle.
- **Doc freshness** (one test, existing): `doc/commands.md` matches the catalog
  after core registration.
- **Staleness** (one test): a command registered after a client has taken a
  snapshot becomes bindable once the client observes the new catalog revision.
- **Concurrency** (one test, sanitiser build): registering while other threads
  decode and dispatch is race-free.
- **Transition only** (deleted at D5): after core registration and before any
  plugin or `init.lua` evaluation, the catalog matches the frozen behavioural
  snapshot.

Deleted: `compiledCatalogHasExactlyTheAssembledRegistrysCommands` (registration
*is* assembly), `compiledCatalogAgreesOnEffectAndCapabilities` (already
circular), `everyCommandDeclaresAnArgumentShapeAndIdsAreUnique` (both enforced
by construction), `featureMetadataTablesAnnotateCatalogCommandsAndDeclareNoNewOnes`
(the tables and the catalog become one thing).

## Plan

| # | Step | Check |
|---|------|-------|
| D0 | `CommandSpecBuilder` + `CommandCatalog` (stable storage) + mechanics tests, unused by production; freeze the pre-migration behavioural snapshot | mechanics tests pass; a reference taken before many later adds is still valid |
| D1 | Populate a catalog during runtime construction from the static table; teach it to produce what consumers need (descriptors, codecs, Lua grants, doc rows); delete `build()`'s exact-handler rule | consumer outputs byte-identical to the static catalog's |
| D1a | Stop copying catalog-derived data: the codec registry moves into the catalog and `HttpEditorServer`/`ProtocolCodec` take a catalog reference; guard the catalog with a shared mutex; add the catalog revision and publish it to clients | R8 holds; protocol round-trips unchanged; a late registration is decodable; concurrent decode-while-registering is race-free under sanitisers |
| D2 | Move doc generation into `test_commands`, reading the catalog; delete `ssg_command_docs` and its `add_custom_command` | `doc/commands.md` byte-identical; no command has moved yet, so any difference is a bug in D2 alone |
| D3 | Migrate one component (`text-input-commands`); its rows leave `Commands.cpp` in the same commit | R9 holds; behavioural snapshot matches; reference unchanged |
| D4 | Migrate remaining components, a few per commit | R9 and the snapshot oracle hold at every step |
| D5 | Delete `src/Commands.cpp` (including the snapshot and its oracle) and `Commands.h`'s table accessors; delete the tests listed above; amend `doc/spec.md:45` | gate green; nothing enumerates commands |

**The doc move must come before any command moves.** From D2 onward the
reference is generated from the catalog, which is the union of static rows and
registered commands throughout the migration, so it stays correct at every step.
Sequencing it later would leave a build step reading a static table that is
being emptied. D2 is also the only step where the move is provably
behaviour-free: nothing has migrated, so the output must be byte-identical.

D3 is the decision point: if registering one component is awkward, the design
is wrong and little has been spent.

## Acceptance

- Adding a command touches exactly one file: the component that implements it.
- A handler's argument type is written once and deduced everywhere else.
- Duplicate ids and missing effect are impossible to ship.
- A command can be registered at any time, and a keymap binding for it resolves
  once it exists.
- `doc/commands.md` is generated for core commands and current.
- No test **restates** the command list. Tests may walk the catalog — the doc
  reference is generated that way — but none holds its own copy of what the
  commands are.

## Status

Delivered. D0–D5 are complete: all 176 commands are registered by the
components that implement them, `src/Commands.cpp` and `include/ssg/Commands.h`
are deleted, and `doc/spec.md`'s catalog rule is amended above.

**What the migration found.** Each batch exposed a fact the static table had
been recording wrongly, all of the same shape: `ArgumentKind::None` meant "no
argument on the WIRE", and reading it as "no argument at all" dropped payloads
that commands were relying on in process.

- Five of the six text commands were declared as taking text; only insertion
  ever read it.
- `tab.activate`, `file.open` and the tree and status invocations take an
  argument when given one and act on the current thing otherwise.
- `theme.define`, `diff.open_file` and their neighbours take typed payloads
  that have no wire form at all.
- `replace.workspace_apply` applies the preview it already holds when given
  none.

The builder therefore ended with four honest combinations rather than one:
an argument is carried **on the wire or only in process**, and is **required or
has a defined meaning when absent**. Each is a separate method, so a command
states which it is and cannot be silently misread.

**What was deleted.** `p0CommandDescriptors`, `EditorSessionBuilder::bind`, the
`ArgumentKind` enum and its type mapping, `commandLabel`'s id-only overload, the
`ssg_command_docs` build step, and eight tests that existed to prove a
declaration and its handler agreed.

**Handles are catalog-relative.** `commandHandle(id)` could not survive: it
resolved a name against the one global table, and there is no longer one.
`CommandRef` always carries the name and carries a handle only when someone
holding a catalog resolved it; `CompiledKeymap` takes the catalog and does that
once per binding, rebuilding when the catalog's revision changes.
