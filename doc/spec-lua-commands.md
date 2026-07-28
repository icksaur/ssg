# spec-lua-commands

## Goal

A Lua function in `init.lua` becomes a real command: bindable to a key,
findable in the palette, dispatchable through the session, indistinguishable at
the call site from a command written in C++.

This is the capability the dynamic command catalog
(`doc/spec-command-registry.md`) was built to make possible, and the first user
of registration after startup.

## Current state (verified in code)

Two halves exist and are not joined.

**Calling built-in commands from Lua works, and is deliberately narrow.**
`ssg.command(id, args)` dispatches into the real session. `initScriptCommandCatalog`
grants `init.lua` the commands whose catalog row sets `initScript`, which is
five: `theme.define`, `theme.background`, `style.define`, `keymap.bind`,
`keymap.unbind`. About 180 commands set `luaApi`.

That gap is intentional (`doc/spec.md` I20): `luaApi` is *eligibility* — the
versioned Lua API may expose this command — while `initScript` is a *grant* to
one host, which additionally requires argument marshalling to exist in
`dispatchInitScriptCommand`. Widening it is per-command work, not a switch.

**Registering a Lua function is half-built.** `LuaCommandHost` already exposes
`ssg.register_command(id, fn)`, stores the function in the registry, rejects a
duplicate id, and can `invoke()` it. But registrations land in the host's own
`pluginCommands` map, never in `CommandCatalog` — so a registered function is
invisible to the palette, unbindable by the keymap, and undispatchable through
the session.

**The Lua state does not persist.** `evaluateInitScript` (`apps/ssg_main.cpp:380`)
constructs `LuaCommandHost` as a local, so the `lua_State` is closed when the
function returns and a fresh one is built on every reload. `registerCallback`
enforces the consequence: registration is refused unless a script is currently
evaluating (`"plugin commands may only be registered while evaluating"`).

This is the whole difficulty. A Lua closure registered as a command must
outlive the evaluation that created it, and today nothing does.

## Design

### The host outlives the evaluation

`LuaCommandHost` moves from a local in `evaluateInitScript` to a member of the
application, constructed once and living for the process. `evaluate()` is called
on it repeatedly — at startup and on every reload — instead of a new host being
built each time.

Nothing else about the host changes: the instruction and time budgets, the
capability set, and the flat string→string argument shape are all as they are.

**But making the host persistent breaks `register_command` unless its storage
becomes generational too.** Today `registerCallback` rejects an id already in
`pluginCommands`, which is correct when each evaluation starts with an empty
host. With a persistent host, **reloading an unchanged script would fail on the
first line that registers anything**, because the previous evaluation's
registration is still there. This is the sharpest consequence of L2 and is easy
to miss.

So a generation owns both halves of a registration — the catalog row and the
Lua function reference — and they are swapped together.

### Reload retires the previous evaluation's commands

`init.lua` is **reset-then-reapply**: the script's current content is the whole
customisation, so a line deleted from it reverts on the next reload. That is
already true of `keymap.bind` (the keymap is reset first) and of `theme.define`.
Registered commands must behave the same way, or a command deleted from
`init.lua` would linger until the process exited.

So each evaluation is a **generation**, owning both the catalog rows it
registered and the `luaL_ref` function references behind them.

**A scratch generation never touches the catalog.** That is the mechanism, and
it matters because `CommandCatalog::add` enforces global id uniqueness: a
scratch row for `mine.save` could not coexist with the live one, so "check
duplicates against the scratch generation only" is not implementable if scratch
rows are added as they are declared.

They are not. `LuaCommandHost` already stages a evaluation's registrations in a
`RegistrationTransaction` and commits them at the end (`publishStaged`) or
discards them (`rollbackStaged`). Registration during evaluation therefore
records an id and a function reference **in the host**, and the catalog sees a
generation only at the swap. On reload:

1. Evaluate. Each `register_command` stages into the scratch transaction,
   duplicate-checked against **that transaction only** — so re-registering an id
   the live generation owns is the normal case, not an error. The catalog is
   consulted once per id, for the naming rule below, and is not modified.
2. **On success**, publish the swap through **one** catalog call:

       CatalogGeneration replaceGeneration(std::span<CommandHandle const> retire,
                                           std::vector<CommandSpecBuilder> add);

   which takes the catalog's exclusive lock **once** and does the retirements
   and the additions inside it. Then release the old generation's function
   references.
3. **On failure**: discard the scratch transaction and release its references.
   The catalog is never touched at all, so "the live generation is untouched" is
   true by construction rather than by careful unwinding.

**Why one call and not `retire` followed by `add`.** Each of those takes the
exclusive lock separately, so a reader holding the shared lock between them
would observe a catalog with the old Lua commands gone and the new ones not yet
present. Readers include the palette and protocol decode, so the visible symptom
would be a palette that momentarily lists none of the user's commands, and a
dispatch that momentarily fails for one that is about to exist. A generation is
replaced, not removed and re-added, so the catalog offers exactly that operation.

**Everything that can fail is checked before anything changes.**
`replaceGeneration` runs in two phases inside its one critical section:

- **Validate.** Every addition is checked exactly as `add` checks one — required
  fields present, effect stated, handler bound, capabilities well-formed, id not
  already held by a non-`lua` owner, no id repeated within the batch — and the
  batch is checked to fit `kMaximumCommands`, counting the slots the retirements
  will *not* free, since tombstones keep their slots.
- **Apply.** Only once every check has passed are the retirements and additions
  performed. Nothing in this phase can *reject* the batch: every reason a
  registration is refused has already been evaluated.

  Allocation is the exception, and the honest scope of the guarantee: growing
  the entry deque or the name index may throw `std::bad_alloc`, which would
  leave the swap half applied. This spec does not handle that, for the same
  reason nothing else in SSG does — the catalog holds a few hundred small rows,
  an editor that cannot allocate them is not going to keep running, and the
  machinery to unwind it would be more failure surface than it removes. Reserving
  capacity before the apply phase would narrow the window without closing it,
  which is worse than being clear about the scope.

The split is the point. `add` throws on a bad spec, so validating as it goes
would let the tenth addition fail after the retirements had already happened —
leaving the user with a generation partly removed and partly replaced, which is
precisely the outcome L5 exists to prevent. Capacity is one instance of that
class, not a special case; the rule is that `replaceGeneration` either changes
everything or nothing, for every failure it is designed to detect.

`add` is then the one-command case of the same operation and shares the
validation, so a check cannot be added to one and forgotten in the other.

**Retirement frees the name and kills the handle.** `retire` marks the entry
retired *and* removes its id from the catalog's name index. The entry keeps its
slot, so its handle stays permanently dead (R4) — but the name becomes
available, which is what lets the next generation register the same id and
receive a *new* handle. Without freeing the name, step 2 would throw on the
first unchanged command; without keeping the slot, a stale handle would resolve
to a different command.

A function reference is released exactly once on either path, so a failed reload
does not leak a Lua function and a successful one does not leave the previous
generation's closures alive.

`CommandCatalog` already carries `retired` on each entry and honours it in every
read — `find`, `handleFor`, `commands`, `ownedBy`, `size` all skip a retired
entry. Nothing sets it yet; this design is the reason it exists.

**Retiring tombstones, never compacts.** The entry keeps its slot, so its handle
is never reissued to a different command. A client holding a stale compiled
keymap resolves that handle to a retired command and is told the command is
unknown — never quietly given a different one. This is R4, and it is what makes
reload safe rather than merely tidy.

**A failed reload leaves the previous commands registered.** Retirement is
applied only if the script evaluates successfully, so a broken `init.lua` does
not cost the user the commands they already had. This means retirement cannot be
done first: the evaluation runs against a *scratch* generation, and the swap
happens on success.

Note the narrowness of that claim. It is **not** "a failed reload changes
nothing": a script that calls `theme.define` and then errors has already changed
the theme. Lua has no transactions and the existing dispatch path has no undo,
so the only thing a generation swap can protect is the thing it owns —
registration state. The wider guarantee is not available and is not promised.

### The classes

Three files, one of them new.

**`LuaCommandHost` stays a pure sandbox.** It knows Lua, budgets and the two
globals it exposes; it knows nothing about catalogs or runtimes. It gains
`registeredCommands()`, reporting what the last successful evaluation
registered, and its existing `publishStaged`/`rollbackStaged` are extended to
release the previous generation's references on success. Its public shape is
otherwise unchanged.

**`CommandCatalog` gains `replaceGeneration`**, described above. `add` becomes
its one-command case and shares the validation.

**`ScriptHost` (new, `include/ssg/ScriptHost.h`) is the wiring**, and the only
thing the application sees:

```cpp
class ScriptHost {
public:
    explicit ScriptHost(EditorRuntime& runtime);
    LuaResult evaluate(std::string_view script);
};
```

It owns the `LuaCommandHost` — and therefore the `lua_State`'s lifetime — the
live generation's handles, and the thread the state belongs to. `evaluate` is
called at startup and on every reload; everything this spec describes happens
behind it.

It is also the one place that builds a Lua-backed handler, so L6 is enforced in
exactly one place: the handler compares `std::this_thread::get_id()` against the
thread `ScriptHost` was constructed on and refuses otherwise.

About 250 lines move out of `apps/ssg_main.cpp`, which currently holds this
policy inline: the capability set, the grant list, the argument marshalling, and
`evaluateInitScript` itself.

**`InitScriptWatcher` stays in the application.** It owns a thread and a wake
pipe wired into `main`'s `select()`, and its two-identical-reads stability rule
decides *when* to reload rather than what a reload means. Moving it would drag
the application's event loop into the library. Path resolution stays for the
same reason: `~/.config/ssg/init.lua` is an application convention.

### Threading

A `lua_State` is not thread-safe, and a registered command's handler re-enters
Lua. So the rule is simple and inherited rather than invented:

**A Lua-backed command's handler runs only on the thread that owns the Lua
state.** That is the main thread, and it is already where evaluation happens:
the config watcher runs on its own thread but only *queues* a script and wakes
the main loop, which calls `drainAndEvaluate` on the main thread
(`apps/ssg_main.cpp:544`).

Two consequences:

- **The catalog's `shared_mutex` does not make Lua safe.** It protects the
  catalog's own structure; it says nothing about what a handler does. A handler
  that re-enters Lua from an HTTP connection thread would be a data race with a
  concurrent evaluation even though every catalog access was correctly locked.
- **A Lua-backed command dispatched from a remote client must be marshalled to
  the main thread.** Until that exists, such a command is refused when dispatched
  from any other thread, with a message saying so. Refusing is not a limitation
  to hide: silently running it would be a race, and a remote client invoking a
  local user's Lua function is a question this spec does not answer.

The narrow rule — Lua-backed commands are invocable from the TUI and from Lua
itself, and refused elsewhere — is what the TUI needs and is honest about what
it does not cover.

### What a registered command declares

`ssg.register_command(id, fn)` gains an optional third argument, a flat
string→string table, matching the shape the Lua bridge already supports:

```lua
ssg.register_command("mine.save_and_format", function()
    ssg.command("edit.toggle_comment")
    ssg.command("file.save")
end, { summary = "Comment and save", label = "Save and Format" })
```

The registered command is added to the catalog with:

- **owner** `"lua"`, so the generated reference and `commandsOwnedBy` can
  distinguish user commands from the editor's own. `doc/commands.md` documents
  only core commands; a Lua command is discoverable at runtime through the
  palette, which is where a user's own commands belong.
- **effect** `Mutation`, always. A Lua function may call any command it has been
  granted, so it must be assumed to mutate; declaring otherwise would let a
  user's command bypass the stale-revision check.
- **no capabilities**, and **no argument**. A registered function takes no
  parameters in this design. Arguments can be added later; they are not needed
  to compose existing commands, which is the motivating case.
- **`luaApi` false**. A Lua command is not part of the versioned Lua API — it is
  a user's own, and exposing it to other hosts is a separate decision.

### Naming

A registered id must contain a dot, and must not be an id the catalog already
holds from an owner other than `lua`.

That second rule needs no list of reserved prefixes: **the catalog is the source
of truth**, and it already knows every registered command and who owns it. A
list of prefixes would be a second place to update whenever a component is
added — the exact duplication the command-registry work removed.

The check is therefore "is this id already registered by someone other than a
previous Lua generation?", which rejects a user shadowing `file.save` at the
line that wrote it, while still allowing a Lua generation to re-register its own
ids on reload. A user command whose id collides with a component registered
*later* is not possible: components register at startup, before any script runs.

## Invariants

- **L1** A Lua-registered command is a command: same catalog, same dispatch,
  same palette, same keymap. There is no second registry.
- **L2** The Lua state lives for the process. A registered function outlives the
  evaluation that created it.
- **L3** Each evaluation is a generation. A successful reload retires the
  previous generation's commands before the new generation's take effect.
- **L4** Retirement tombstones. A retired command's handle is never reissued,
  so a stale handle resolves to "unknown", never to a different command.
- **L5** A failed evaluation changes no *registration* state: the previous
  generation's commands remain registered and callable. Effects the script
  already produced through `ssg.command` are not undone; nothing in this design
  can undo them.
- **L6** A Lua-backed handler runs only on the thread owning the Lua state.
  Dispatch from any other thread is refused, not serialised silently.

## Considerations

- **L5 forces the ordering, and the ordering is the hard part.** "Retire, then
  evaluate" is the obvious implementation and it is wrong: a script that fails
  halfway would leave the user with neither their old commands nor their new
  ones. Evaluating into a scratch generation and swapping on success is more
  work and is the only version that satisfies L5.
- **Reload is not atomic, and L5 must not be read as saying it is.** A script
  that registers two commands, calls `theme.define`, and then errors has already
  changed the theme. The scratch generation discards both registrations, which
  is the right outcome for the state this design owns, but the theme stays
  changed. That asymmetry exists today for every `init.lua` effect and this
  design neither creates nor fixes it. L5 is scoped to registration state for
  exactly this reason.
- **L6 will be the first thing someone wants to relax.** The temptation is to
  wrap the handler in a lock. That does not help: the budget hook, the registry
  references and the handle table are all state a second thread would corrupt
  even under a lock held by the first. Relaxing L6 means marshalling the call to
  the owning thread, which needs a queue and a way to return a result — real
  work, deliberately not in this spec.
- **The catalog grows monotonically across reloads.** Every reload registers a
  new generation and tombstones the old one, so a user editing `init.lua` fifty
  times in a session leaves fifty tombstones. The handle space is 16 bits
  (`CommandCatalog::kMaximumCommands`), so a session that reloads a
  ten-command script six thousand times would exhaust it and registration would
  throw. That is far outside plausible use, and the alternative — reusing
  handles — is the one thing R4 forbids. Recorded because the arithmetic should
  be someone's deliberate decision rather than a surprise.
- **This does not widen `ssg.command`'s grant.** A user command can only call
  the five commands `init.lua` may already call, which is enough to compose
  theme, style and keymap changes but not enough to write an interesting editor
  command. Widening the grant is separate work (argument marshalling per
  command) and is the obvious next step after this lands.
- **Registration remains evaluation-scoped.** `register_command` still refuses
  outside an evaluation, even with a persistent state. A command registered from
  inside a *running command* would belong to no generation and could not be
  retired.

## Testing

- **Persistence** (unit): a command registered during one evaluation is
  dispatchable after that evaluation returns.
- **Retirement** (unit): a command present in the first script and absent from
  the second is unknown after reload; one present in both survives; one only in
  the second appears.
- **Unchanged reload** (unit): evaluating the same script twice succeeds, and
  the command still dispatches. This is the case a persistent host breaks if the
  function store is not generational.
- **Tombstoning** (unit): the handle of a retired command does not resolve, and
  is not reissued to a command registered afterwards — including when that
  command re-registers the same id.
- **Failed reload** (unit): after a script that errors, the previous
  generation's commands are still dispatchable.
- **Composition** (integration): a registered command that calls `ssg.command`
  twice performs both, dispatched through the session like any other.
- **Keymap** (integration): a registered command bound with `keymap.bind`
  resolves and runs from a keystroke.
- **Thread refusal** (unit): dispatching a Lua-backed command from a non-owning
  thread is refused with a clear message.
- **Atomic swap** (unit, sanitiser build): a reader looping over `commands()`
  while a generation is replaced never observes a catalog missing both
  generations.
- **Capacity** (unit): a swap that would exceed the handle space throws and
  leaves the previous generation registered and dispatchable.
- **All-or-nothing** (unit): a swap whose last addition is invalid — a missing
  summary, an empty capability, an id repeated within the batch — throws and
  leaves the previous generation registered and dispatchable, with none of the
  batch's earlier additions present.

## Plan

| # | Step | Check |
|---|------|-------|
| L0 | `replaceGeneration` on `CommandCatalog`: validate the whole batch, then retire and add, under one exclusive lock; retiring frees the name and keeps the slot; `add` becomes its one-command case | a retired command is unknown; its id can be registered again and receives a NEW handle; the old handle stays dead; no reader observes a partial swap; a swap containing ANY invalid spec, or exceeding capacity, changes nothing |
| L1 | Hoist `LuaCommandHost` to process lifetime; `evaluate()` called per reload | existing `init.lua` behaviour unchanged; state survives a reload |
| L2 | Bridge `register_command` into the catalog as owner `lua` | a registered command appears in the palette and dispatches |
| L3 | Generations: evaluate into a scratch generation, swap on success, retire the previous and release its function references | L3 and L5 hold; an unchanged script reloads cleanly; a broken script leaves the old commands working |
| L4 | Refuse a Lua-backed dispatch from a non-owning thread | thread-refusal test passes |
| L5 | Document in `doc/config.md`; add the `register_command` reference | config doc coverage test passes |

L2 is the decision point: if a registered command is not dispatchable through
the ordinary path with no special-casing, the catalog work did not deliver what
it promised.

## Acceptance

- A function in `init.lua` can be bound to a key and run from a keystroke.
- Deleting it from `init.lua` and saving removes the command.
- A broken `init.lua` leaves the previous commands working.
- A Lua command is never dispatched off the thread owning the Lua state.
- No second command registry exists.

## Status

L0-L2 are implemented (`CommandCatalog::replaceGeneration`, `ScriptHost`, and
the `register_command` bridge). The class breakdown above was added after the
five review rounds and reflects what shipped.

### What L2's decision point found

A command a script registers IS an ordinary catalog command. It is registered
through the same `CommandSpecBuilder`, appears in the palette, is bindable by
name from `keymap.bind`, and is dispatched through the same path as every
built-in. Nothing downstream distinguishes it. That part of the design holds,
and the catalog work earned its keep.

**But a script's command cannot yet call another command.** `EditorSession::
dispatch` holds a non-recursive `std::mutex` across the handler call
(`src/EditorSession.cpp`), so a handler that dispatches deadlocks against
itself. A Lua-backed handler is the first handler that dispatches: every
built-in mutates the session directly instead, so this constraint has existed
unnoticed and unwritten since the session was built.

This would make a script command that does nothing but call built-ins -- the
obvious first thing a user would write -- hang the editor. It is a pre-existing
invariant ("a handler must not dispatch") that nothing stated, tested or
enforced, and that only a re-entrant handler can violate. `EditorRuntime`'s
palette path already worked around it, describing the session lock as
"non-reentrant" in passing.

**Nesting is now refused rather than hung.** `EditorSession` records which
thread is inside `dispatch` and the revision it is running against
(`activeDispatchRevision`). `EditorRuntime::dispatch` consults it before
touching anything that takes the session lock and returns `HandlerFailed` with
a message naming the rule. `EditorSession::revision()` answers from the same
record instead of taking the lock, because a handler asking for the revision is
asking from inside a dispatch that already knows it -- without this the
deadlock simply moved into the argument list of the nested call.

That is the minimum honest behavior, not the goal: a deadlock is never an
acceptable way to report a rule, but refusing composition is not much of an
answer either. **Whether a handler SHOULD be able to dispatch remains open and
needs its own spec.** The real options are to release the lock around handler
execution, or to admit same-thread re-entrancy explicitly and define what a
nested mutation does to the revision. Until one is chosen, a script's function
may compute and register but cannot call `ssg.command` -- and now finds that
out immediately.

### The refusable step runs before the irreversible one

The host and the catalog each hold half of a Lua command -- the function and the
registration -- and both are replaced on reload. Whichever commits first can be
left holding a generation the other has abandoned.

Originally `LuaCommandHost::evaluate` published its registrations and
`ScriptHost` offered them to the catalog afterwards. A batch the catalog
refused -- an id colliding with a built-in, say -- then left the previous
generation listed in the catalog with its Lua functions already released:
commands that look available and fail when invoked. Retiring those entries too
would keep the two in agreement, but only by destroying a working generation
because its *replacement* was invalid, which is the opposite of what a failed
reload should do.

So the order is inverted instead. `LuaCommandHostOptions` carries a
`publishGate`, called with the evaluation's command ids after they are staged
and before they replace anything; `ScriptHost` sets it to the catalog swap. A
refusal discards the staged registrations and returns, leaving the previous
generation whole on both sides. The step that can refuse now runs before the
step that cannot be undone, so there is no window in which the two disagree and
nothing to reconcile afterwards.
