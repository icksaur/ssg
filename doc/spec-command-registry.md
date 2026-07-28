# spec-command-registry

## Goal

A command is declared where it is implemented, once, by the component that
offers it. Registration and implementation are the same act, so a command
cannot exist half-wired, and adding a feature's command touches only that
feature.

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

The static design also carries permanent ceremony: a `CommandSpec` row is
positional C-style aggregate initialisation, where `{true, false}` is
`{luaApi, initScript}` only if you remember the field order.

## What a dynamic catalog buys

Registering the spec and the handler **together** collapses the whole class of
declaration/implementation disagreement, because there is only one declaration.
Most valuably, the argument type stops being declared at all: it is *deduced*
from the handler.

```cpp
catalog.add("text.insert")
    .owner("text-input-commands")
    .label("Insert")
    .summary("Insert text at every cursor")
    .mutates()
    .lua()
    .handler<TextInputArguments>(
        [&runtime](CommandContext& context, TextInputArguments const& text) {
            return runtime.insertText(context, text);
        });
```

`ArgumentKind` disappears as an authored fact. `handler<TextInputArguments>`
registers the codec for that type *and* the type-safe unwrap, so a handler
cannot disagree with its codec: there is one type, named once. The `std::any`
`any_cast` moves out of every handler and into one place.

## Design

### The catalog

`CommandCatalog` owns every command for the lifetime of a session. It is
populated during construction, sealed, and then only read.

- `add(id)` returns a `CommandSpecBuilder&`. A duplicate id throws
  `std::runtime_error` immediately, naming the id and both owners. Collisions
  are a programming error, not a runtime condition.
- `seal()` ends registration. After sealing, `add` throws; before sealing,
  lookup throws. There is no window in which the catalog is half-populated and
  readable, so no consumer can observe a partial catalog.
- Iteration is in registration order and is stable within a process.

### The builder

Each field is a named method, so a row is self-documenting and order-free:

| Method | Meaning | Required |
|---|---|---|
| `.owner(name)` | The component offering the command; groups the reference | yes |
| `.label(text)` | Palette display text | no — defaults to the humanised id |
| `.summary(text)` | One line for the generated reference | yes |
| `.mutates()` / `.observes()` | `CommandEffect` | **exactly one** |
| `.capability(id)` | A required capability; repeatable | no |
| `.lua()` | Eligible for the versioned Lua API (I20) | no |
| `.initScript()` | `init.lua` may call it at startup; implies `.lua()` | no |
| `.handler<Args>(fn)` | The implementation; deduces the argument type | yes |
| `.handler(fn)` | An implementation taking no arguments | yes (alternative) |

`seal()` throws if any command is missing a required field. **Effect is
deliberately required rather than defaulted**: the static catalog's predecessor
hardcoded `Mutation` for all 182 commands, and a default is how that happens.
Forcing the author to write `.mutates()` or `.observes()` makes "nobody thought
about it" unrepresentable.

### Where registration lives

Each component exposes one registration function declared in an **internal**
header (`src/runtime/editor_runtime_internal.h`), not in `include/ssg/`:

```cpp
void registerTextInputCommands(CommandCatalog&, EditorRuntime::Impl&);
```

These are wiring, not API. Publishing them in `include/ssg/` would make the
registration mechanism itself a public surface that callers could invoke out of
order or twice — exactly the inconsistent state this design exists to remove.
The composition root is the only caller.

A single composition root calls them in order and seals. That list is the
answer to "which components offer commands"; each component's own file is the
answer to "which commands does this component offer".

### Ordering

The catalog is sealed before `init.lua` is evaluated. This is already the
natural order — the session is built before the config is read — but it becomes
a stated requirement, asserted by the Lua host refusing an unsealed catalog.
Anything `init.lua` may call is therefore registered and usable when it runs.

### Generating the reference

**Documentation generation moves from a build step to a test.** Today
`ssg_command_docs` links the static table and walks it at build time. Under this
design the catalog only exists once registration has run, and registration needs
a runtime: `EditorRuntime::create` calls `std::filesystem::create_directories`
for the scratch and recovery roots (`src/EditorRuntime.cpp:1765`), so a build
step would perform filesystem work during a build — flaky, and wrong.

Constructing a runtime in a temporary workspace is something the test suite
already does routinely. So:

- `doc/commands.md` stays generated and committed.
- `test_commands` constructs a runtime in a temporary workspace, walks the
  sealed catalog, and asserts the committed file matches.
- Regeneration is the same test run with `SSG_UPDATE_DOCS=1`, which rewrites the
  file instead of asserting.

**Who runs what.** The author of a new command runs the test with
`SSG_UPDATE_DOCS=1` and commits the regenerated reference alongside their
change. The push gate runs the suite without that variable, so a reference that
was not regenerated fails the gate. Generation is never implicit in a build,
which is the point: a documentation change is a reviewed diff, not a build
artefact that appears unbidden in someone's working tree.

This deletes the `ssg_command_docs` executable and its `add_custom_command`
entirely. The freshness guarantee is unchanged — the test already exists and
already knows the doc path via `SSG_COMMAND_DOC_PATH`.

### What replaces `ArgumentKind`

Nothing authored. `.handler<Args>` records a `std::type_index` and a codec built
from `Args`. The protocol's codec registry is populated from the catalog at
seal time rather than from a `switch`. A command with no arguments uses the
`.handler(fn)` overload, which is a *declaration* of "no arguments", not a
fallthrough.

## Invariants

- **R1** A command exists only by registration. There is no other way to make
  one dispatchable, and no list of ids anywhere else.
- **R2** A command's handler and its declared facts are registered in one
  expression. A handler's argument type is deduced, never restated.
- **R3** Duplicate registration is fatal at construction, naming both owners.
- **R4** The catalog is sealed before `init.lua` runs and immutable thereafter.
- **R5** Effect is explicit for every command; there is no default.
- **R6** `CommandHandle` is a **process-local** index into the sealed catalog.
  It is never persisted, never sent on the wire, and never compared across
  processes. This is enforced structurally: `index()` is private to the
  registry, and a deleted `toValue(CommandHandle)` overload makes any attempt to
  encode one a compile error rather than a silent success.
- **R7** During the migration only, a command id is registered dynamically
  **xor** present in the static table — never both, never neither.

## Migrating without two sources of truth

D2 and D3 leave the catalog half-migrated across several commits, which is the
riskiest part of this plan: two mechanisms can declare a command, and "the gate
is green" does not prove they have not diverged. The transition therefore gets
its own explicit rule and its own oracle.

**The rule (R7).** During migration the sealed catalog is the union of the
dynamically registered commands and the not-yet-migrated static rows. A
component's commands move as a whole; the moment `registerTextInputCommands`
exists, that owner's rows leave `Commands.cpp` in the same commit.

**Enforced by construction.** `seal()` already throws on a duplicate id, so a
command declared in both places is fatal at startup, not merely tested for. This
is the primary guard, and it costs nothing extra.

**The transition oracle.** `Commands.cpp` keeps, for the migration only, a
frozen snapshot of the pre-migration catalog — data, not a declaration. A test
asserts the sealed catalog matches it at every step.

Pinning ids alone is **not** sufficient: a move that preserves every id can
still flip `effect` from `Mutation` to `Observation` (disabling the
stale-revision check), drop a required capability, or set `initScript` on a
command that did not have it — silently widening authority while satisfying an
id-only check. The snapshot therefore pins each command's **behavioural tuple**:

    { id, effect, requiredCapabilities, luaApi, initScript }

`label` and `summary` are deliberately excluded: they are cosmetic, a move is a
natural moment to improve them, and `doc/commands.md` shows any change in the
same diff.

The oracle fails if a migration commit loses a command, renames one, smuggles a
new one in under cover of the move, or alters any fact that governs behaviour or
authority. **The snapshot and its test are deleted with `Commands.cpp` in D5**;
they exist to make the transition safe, not to become the next thing that must
be maintained.

## Considerations

- **R6 is a new hazard this design creates, and it is the one to watch.** With
  a static table, a handle was an index into a compiled array: the same value
  meant the same command in every process built from the same source. With a
  dynamic catalog, a handle means whatever registration order produced it. If a
  future change registers commands conditionally — a plugin, a feature flag, a
  headless mode omitting a component — the same integer denotes different
  commands in different configurations. The wire protocol already speaks names
  (`CommandRef` carries both), so this is sound today. A test that no encoder
  emits a handle is not sufficient on its own, because it only covers the
  encoders that exist: R6 is therefore enforced in the API shape first — the
  index is not publicly readable and encoding a handle does not compile — with
  the test as a second line.
- **"Find all commands" gets harder, and this is a genuine loss.** Today
  `src/Commands.cpp` is one file listing all 182. Afterwards they are spread
  across ~20 component files. The mitigations: `doc/commands.md` remains
  generated and committed, so the full list is still in the repo and a new
  command still appears in a reviewed diff; and `commandsOwnedBy` still answers
  "what does this component offer". This is a real trade — locality of
  *declaration to implementation* bought at the cost of locality of
  *declaration to declaration*. The user's stated goal is minimum blast radius
  per feature, which is the former.
- **Documentation generation cannot stay a build step.** `EditorRuntime::create`
  creates directories on disk, so running registration during a build would do
  filesystem work at build time. Moving generation into `test_commands` — which
  can construct a runtime in a temporary workspace as the suite already does —
  resolves this and deletes a CMake custom command and an executable. The
  freshness guarantee is unchanged. This was the weakest part of the first draft
  of this spec and was found in review.
- **We lose the independently reviewed surface entirely.** `doc/spec.md:45`
  originally required the catalog to be a transcription reviewed apart from the
  implementation; the static-catalog migration already weakened this to "a
  hand-authored table reviewed as data". This design removes the separate
  artefact altogether: the only review surface is the diff of the component
  that gained a command, plus the regenerated `doc/commands.md`. That is a
  deliberate, stated retreat, and `doc/spec.md:45` must be amended again rather
  than quietly contradicted.
- **Registration is not compile-time.** No `constexpr` catalog, and a component
  that is never wired into the composition root silently offers nothing. The
  user's answer is the right one: each component tests its own registration. A
  composition-root test that the sealed catalog is non-empty per owner catches
  the "forgot to wire it up" case cheaply.
- **This is the third catalog design in this project's history.** The first was
  scattered structures plus a non-compiling JSON; the second is the compiled
  table; this is the third. That is not an argument against it — the second
  design taught us that co-locating a command's *facts* is insufficient if they
  are not co-located with its *code* — but the cost of the churn is real and
  should be acknowledged rather than presented as a clean win.

## Testing

The point of this design is that most catalog tests stop being necessary.
What remains:

- **Catalog mechanics** (unit, small): add, duplicate-throws, seal-then-add
  throws, read-before-seal throws, missing-required-field throws, iteration
  order.
- **Transition only** (deleted at D5): the sealed catalog matches the frozen
  behavioural snapshot — `{ id, effect, requiredCapabilities, luaApi,
  initScript }` for every command.
- **Per-component registration** (unit, one per component): the component
  registers the commands it claims to. This is the functional requirement each
  component owns.
- **Composition** (one test): every owner has at least one command in the sealed
  catalog, which catches an unwired component.
- **R6** (one test): no protocol encoder emits a handle.
- **Doc freshness** (one test, existing): `doc/commands.md` matches the sealed
  catalog. The push gate runs the test suite, so a stale committed reference
  fails there; `SSG_UPDATE_DOCS=1` is how the author regenerates it, in the same
  commit as the command they added.

Deleted: `compiledCatalogHasExactlyTheAssembledRegistrysCommands` (registration
*is* assembly), `compiledCatalogAgreesOnEffectAndCapabilities` (already
circular), `everyCommandDeclaresAnArgumentShapeAndIdsAreUnique` (both enforced
by construction), `featureMetadataTablesAnnotateCatalogCommandsAndDeclareNoNewOnes`
(the tables and the catalog become one thing).

## Plan

| # | Step | Check |
|---|------|-------|
| D0 | `CommandCatalog` + `CommandSpecBuilder` + their unit tests, unused by production; freeze the pre-migration behavioural snapshot | mechanics tests pass |
| D1 | Seal a catalog during runtime construction, populated from the static table; teach it to produce what consumers need today (descriptors, codecs, Lua grants, doc rows) | consumer outputs byte-identical to the static catalog's |
| D2 | Move doc generation into `test_commands`, reading the **sealed** catalog; delete `ssg_command_docs` and its `add_custom_command` | `doc/commands.md` byte-identical; no command has moved yet, so any difference is a bug in D2 alone |
| D3 | Migrate one component (`text-input-commands`); its rows leave `Commands.cpp` in the same commit | R7 holds; behavioural snapshot matches; `doc/commands.md` unchanged |
| D4 | Migrate remaining components, a few per commit | R7 and the snapshot oracle hold at every step |
| D5 | Delete `src/Commands.cpp` (including the snapshot and its oracle) and `Commands.h`'s table accessors; delete the tests listed above; amend `doc/spec.md:45` | gate green; nothing enumerates commands |

**The doc move must come before any command moves, not after.** Documentation
is generated from the *sealed* catalog from D2 onward, which is the union of
static rows and registered commands throughout the migration — so it stays
correct at every step. Sequencing it after the component moves would leave a
build step reading a static table that is being emptied, and sequencing it after
D5 would leave it reading a table that no longer exists. D2 is also the only step where
the move is provably behaviour-free: nothing has migrated, so the reference it
produces must be byte-identical or the step is wrong.

D3 is the decision point: if registering one component is awkward, the design
is wrong and little has been spent.

## Acceptance

- Adding a command touches exactly one file: the component that implements it.
- A handler's argument type is written once and deduced everywhere else.
- Duplicate ids and missing effect are impossible to ship.
- `doc/commands.md` is generated from the sealed catalog and current.
- No test enumerates commands.
