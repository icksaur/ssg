# spec-keymap

## Goals

Let a user script rebind or remove individual keystroke->command mappings on
the running keymap without hand-authoring a full replacement keymap.

## Design

`KeymapViewState` already models the whole live keymap as a flat list of
`KeyBinding{sequence, commandId, context}` entries; `KeymapMatcher` is the
pure validate/resolve/deriveDelta engine over that value, unchanged by this
feature.

`keymap.bind` takes `{sequence, command, context}`: `sequence` is a
space-separated `KeyCodec` stroke string (e.g. `"Escape KeyF KeyT"`),
`command` the target command id, and `context` one of `"*"`/`"editor"`/
`"panel"`/`"prompt"` (absent defaults to `"*"`). It removes any existing
binding for the SAME `(context, sequence)` pair (a rebind, not a duplicate),
appends the new binding, then re-validates the whole resulting keymap
through `KeymapMatcher` -- an unparseable sequence, an unknown context, an
empty command id, or a validation failure (duplicate/ambiguous-prefix/
unreachable binding, or removing the `settings.open` global escape hatch)
rejects the whole call and leaves the keymap unchanged.

`keymap.unbind` takes `{sequence, context}` and removes any binding matching
that pair; removing an absent binding is a no-op success, not an error.

Every init.lua evaluation (startup and every later auto-reload) resets
the live keymap to `defaultTerminalKeymap()` before running the script, so
init.lua's current content is the WHOLE keymap customization -- never
additive across reloads. A line removed from init.lua reverts that
binding on the next reload, matching `theme.define`'s full-reload-replaces
model.

Normative commands owned by this feature:

- `keymap.bind`
- `keymap.unbind`

## Invariants

- K1 (context validity), K2 (prefix-freedom), K6 (a `settings.open` global
  escape hatch always survives) are re-checked on every `keymap.bind` call
  via the existing `KeymapMatcher::validate`/`hasGlobalBinding` -- the same
  checks the compiled-in default keymap must already pass.
- The command is all-or-nothing: a rejected `keymap.bind`/`keymap.unbind`
  call never partially mutates the live keymap.

## Considerations

- Sequence is a single space-separated string, not a Lua array, to avoid
  widening ssg.command's flat string->string argument bridge to carry
  arrays (see `doc/spec-config.md`).
- K5 ("only argument-free commands are keystroke-bindable") is a
  documentation-level convention observed by the compiled-in default
  keymap, not a machine-checked catalog flag: `data/required-commands.json`'s
  `keymap` boolean marks whether a command is excluded from keymap
  reachability for POINTER/CLIENT-fulfillment reasons (e.g. `tree.select`,
  `find.update_query`), not whether it requires an argument -- commands
  like `settings.set`/`cursor.set_position`/`text.insert` are marked
  `keymap:true` despite requiring one. `keymap.bind` therefore does not
  reject binding to an argument-required command id; resolving such a
  binding at runtime dispatches with an empty payload, which every
  existing command handler already rejects gracefully (a "requires a
  typed ... payload" failure), not a crash. A future increment could add a
  real machine-checked "argument-free" catalog flag if this proves
  confusing in practice.

## Acceptance (Definition of Done)

- Observable: binding a new sequence to a known command makes it resolve
  through `KeymapMatcher`/appear in the palette's live keymap detail;
  unbinding removes it; an invalid bind (bad sequence, unknown context,
  reintroduced ambiguity, or removing the last `settings.open` global
  binding) is rejected with the prior keymap unchanged; reloading
  init.lua with a binding line removed reverts to the default keymap for
  that sequence.
- Gates: project build and keymap tests are green.
- Oracles: bind/unbind/rebind/no-op/rejection cases against
  `KeymapViewState` equality (see `doc/spec-config.md`).
