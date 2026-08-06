# spec-lua-widget-composition

Status: draft (spec only; not yet implemented)

## Goals

Let `init.lua` COMPOSE the header and footer chrome from data — a tree of widget
descriptors handed to a new `ssg.chrome{…}` API — instead of the fixed built-in
status fields. A composed region is a row of widgets packed left/right with an
optional centre; each widget is one of the closed `WidgetKind` vocabulary
(label / field / checkbox / text-input / spacer) carrying literal text OR a
reference to a live built-in provider, plus — where the kind allows — a click
`command` (a built-in id or a `ssg.register_command` function). The same
row/widget descriptor vocabulary is designed to compose MANY rows (a Column) so a
future form builder
and a form TAB reuse it unchanged — those are explicitly out of scope to BUILD
here, but the API is shaped to accept them. After this change:

- `init.lua` may replace the header and/or footer STATUS FIELDS with its own
  widget row; when it does not, the built-in chrome is byte-for-byte unchanged.
  The picker input line is not composable and keeps its C++-owned header slot (a
  composed header is left-group only in this spec; see §Replace semantics).
- A composed widget's text is either a literal or a live built-in provider
  (`path` / `branch` / `status` / `follow`), so a custom header keeps a live
  branch field without running Lua on the per-frame path.
- A composed widget's click (for the kinds that allow a command — see the
  per-kind matrix) runs a command id — built-in or a Lua-registered function —
  through the existing dispatch, with no new event plumbing.
- The full closed descriptor vocabulary (incl. text-input) is DEFINED and
  validated now, so a form is describable; only the read-only/command kinds are
  WIRED for header/footer. A `Checkbox` in chrome is DISPLAY-ONLY by definition
  (no toggle affordance exists in a header/footer); a `TextInput` in an
  `ssg.chrome` header/footer is a whole-kind compose-time ERROR (fail-loud, not a
  dead control). Interactive toggling/editing exists only in the deferred form
  context.

Non-goal (explicit): form TAB documents (a new `TabKind`, form state ownership,
interactive input/submit event routing); dynamic per-frame Lua values; changing
the wire protocol (composition is server-side; only the existing
`AccessibilityNode` projection crosses the wire); adding `WidgetKind`s or
`SemanticRole`s.

## Design

### The delivery channel: a new `ssg.chrome` API function

The existing `ssg.command(id, table)` bridge decodes its argument to a FLAT
`unordered_map<string,string>` (Theme.h / Style.h pattern); a widget tree is
nested and cannot pass through it. So this adds a THIRD `init.lua` API function
alongside `command`/`register_command` (the `LuaCommandHost::kApiFunctions`
`static_assert` at LuaCommandHost.cpp:124 makes this a deliberate, guarded
addition):

```lua
ssg.chrome{
  header = {                                   -- LEFT-GROUP ONLY (see §Replace)
    left = { { kind = "field", provider = "path" },
             { kind = "field", provider = "branch" } },
    separator = 1,                             -- default 1
  },
  footer = {                                   -- full left / right / center
    left   = { { kind = "field", provider = "status" } },
    right  = { { kind = "label", text = "RO", role = "footer" },
               { kind = "field", text = "reload", command = "config.reload" } },
    -- center = { kind = "label", text = "…", width = "flex" },  -- optional, single
  },
  -- omit a region entirely to keep its built-in chrome
}
```

(A `header.right` or `header.center` is a compose-time error in this spec; the
header is left-group only while the picker input line owns the trailing cells.)

`ssg.chrome` participates in the SAME staged-publish / gate / rollback reload
lifecycle as the command registrations (LuaCommandHost.cpp:458-527): a call
stages a `ChromeComposition`; a later error anywhere in the script rolls the
whole thing back, leaving the prior chrome (built-in or previously-composed)
intact. Calling `ssg.chrome` twice in one evaluation replaces the staged
composition (last wins), matching “your commands live exactly as long as the
lines that define them”.

### The descriptor vocabulary (nested-table decode)

A recursive Lua-table→C++ decoder (the new work the fork-1 decision buys)
produces these pure value types, which are also the internal composition types:

- `WidgetDescriptor { WidgetKind kind; string id; optional<Value> value;
  optional<Value> checked; optional<int> width; optional<string> role;
  optional<string> command; int rank; bool keep; Overflow overflow;
  string sigil; }` where a `Value` is a literal (`text`/a bool) XOR a `provider`
  id. `kind` is the closed enum (`Label`/`Field`/`Checkbox`/`TextInput`/`Spacer`;
  `Container` is structural, expressed by the row/column shape, not a leaf kind).
  `role` is a `SemanticRole` name (defaulting per region). Which fields are
  REQUIRED / ALLOWED / FORBIDDEN is per-kind IN THE CHROME (header/footer)
  CONTEXT (the matrix below); the decoder validates that per kind and fails loud
  on a missing-required or a present-forbidden field. `command` is the click
  target (unvalidated at compose time — see Considerations). `id` defaults to a
  generated slot id when omitted. The descriptor TYPE is the superset that also
  serves forms; a future FORM context relaxes the matrix (e.g. `TextInput` and a
  toggleable `Checkbox` gain `value`/`command`), so no descriptor SHAPE change is
  needed later — only a context-specific validation table.

Per-kind field rules IN A CHROME (header/footer) COMPOSITION (the decoder
enforces exactly this for `ssg.chrome`; `—` = forbidden; a future form context
has its own table):

| kind | `value` | `checked` | `width` | `role` | `command` | notes |
|------|---------|-----------|---------|--------|-----------|-------|
| `Label` | required (text\|provider) | — | — | optional | — | static text in a `role` |
| `Field` | required (text\|provider) | — | — | optional | optional | the general cell; a "button" is a Field + command |
| `Checkbox` | optional (caption) | required (bool\|provider) | — | optional | optional | DISPLAY-ONLY in chrome; user-toggle is a form concern |
| `Spacer` | — | — | required in a left/right group; — as center | — | — | center⇒Flex slot; left/right⇒fixed blank of `width` cells |
| `TextInput` | — | — | — | — | — | whole-kind compose-time ERROR in chrome (form-only) |

`value`/`checked` each require EXACTLY one of literal or `provider` when present;
both-or-neither is a compose-time error.
- `RowDescriptor { vector<WidgetDescriptor> left; vector<WidgetDescriptor> right;
  optional<WidgetDescriptor> center; CenterWidth centerWidth; int centerFixed;
  int separator; }`. This is the reused unit: a header is one `RowDescriptor`, a
  footer is one, and a FORM is a `vector<RowDescriptor>` (a Column) — see
  §Forms.
- `ChromeComposition { optional<RowDescriptor> header; optional<RowDescriptor>
  footer; }` — what one `ssg.chrome` call stages.

The decoder is the ONE place Lua structure becomes C++; it is pure (no runtime
deps) and fails loud (returns an error with a path-qualified message, e.g.
`header.left[2]: unknown kind "buton"`), so an invalid table rejects the whole
`ssg.chrome` call and — via the reload lifecycle — the whole script.

### Lowering to the shipped `WidgetStack`

A `RowDescriptor` lowers to a `WidgetStack` (the shipped primitive, unchanged):
each `WidgetDescriptor` becomes a `StackItem` (its resolved text as `content`,
`measureFieldCells`/`displayCells` as `desired`, plus `rank`/`keep`/`overflow`/
`sigil`), packed left or right (center → `center(...)`). `WidgetStack::resolve`
already guarantees non-overlap, rank-collapse, right clamp-truncate, and
fail-loud on a second center — so a composed row inherits every geometry
invariant with NO new layout code. The lowering also carries each descriptor's
`command` and `id` into the projected `AccessibilityNode` (`commandId`), so a
composed widget is hit-tested and dispatched exactly like a built-in field/action
today. `WidgetKind` finally has a consumer: the lowering switches on it to pick
the projection.

Per-kind lowering (phase 1, constrained to what the shipped stack supports — NO
stack extension):

- `Label` — static text in a `role`; no command.
- `Field` — text (literal or provider) + optional `command`; the general
  collapsible cell. A clickable "button" is a `Field` with a `command`.
- `Checkbox` — DISPLAY-ONLY here: `checkboxText(checked, caption, glyphs)` with
  `checked` from a literal bool or a provider; it is NOT user-toggleable in
  header/footer (toggling is a form concern — see §Forms). An optional `command`
  makes the whole cell clickable.
- `Spacer` — the shipped stack has NO general flex item in the left/right groups
  (only the CENTER slot is flex). So a `Spacer` lowers to: the `center` Flex slot
  when used as center (at most one, same as `center`), OR a fixed-width blank
  cell (an empty `Field` of `width` cells) in a left/right group. A left/right
  `Spacer` without an explicit `width` is a compose-time error. This spec adds no
  flex-in-group extension to `WidgetStack`.
- `TextInput` is DEFINED in the vocabulary but is a whole-kind compose-time ERROR
  inside an `ssg.chrome` header/footer (it requires the read-write field state +
  edit event routing that only the deferred form surface provides). It is
  describable so a form is describable; it fails loud here rather than rendering a
  dead control. (A chrome `Checkbox` is not an error — it is display-only by
  definition, per above; toggle affordance exists only in the form context.)

`Value` resolution: a `provider` widget's text is filled each frame by the
existing status-field providers (`path`/`branch`/`status`/`follow`,
StatusFields.cpp:110-138) — the composition references them by id, C++ resolves
them where `projectStatusFields` would. A provider widget also INHERITS that
provider's built-in command coupling (`bindStatusFieldCommands`): e.g. the
`follow` field stays clickable with its follow-mode action, and `path`/`branch`
keep whatever command the built-in field carries, UNLESS the descriptor supplies
its own `command` (which overrides). A `text` literal is static and carries only
the descriptor's own `command`. This is the "named provider, not per-frame Lua"
resolution of the static-vs-live tension: the hot path never calls Lua.

### Replace semantics

When `ChromeComposition.header` is present it OWNS the header region's STATUS
FIELDS: the built-in status-field projection for that region is skipped and the
composed row is laid out instead. An absent region keeps its built-in chrome
verbatim. To keep a live built-in value, a composed widget references it by
`provider` id; there is no merge/interleave of built-in and composed items, so
there is no ownership ambiguity.

The picker INPUT LINE is NOT part of the composable surface. It remains a
C++-owned seam that, when a picker is open, occupies its reserved region in the
header exactly as today (the `inputLineReservation` floor, and the input line +
ghost after the composed left group). To keep that boundary unambiguous, a
composed HEADER row is LEFT-GROUP ONLY in this spec: `header.right` and
`header.center` are a compose-time error (the header has no built-in right group
today, and a right/centre item would race the input line for the trailing cells).
The FOOTER row supports full `left` / `right` / `center` (it has no input line).
Header right/centre composition is deferred to when the input line itself is
modeled as a composable widget (a later spec), and the descriptor vocabulary
already admits it without change.

### Forms and the form-tab seam (considered, DEFERRED)

The `RowDescriptor` is deliberately region-agnostic so the SAME vocabulary
composes a multi-row surface: a form is a Column of `RowDescriptor`s, solved by
the existing `solveLayout` (Row/Column/Exact/Flex) exactly as the shell regions
are. This spec DEFINES the interactive kind (`TextInput`) in the vocabulary and a
`Checkbox` that a future form context can make toggleable, so a form is fully
DESCRIBABLE, but WIRES only the read-only/command kinds for header/footer (chrome
`Checkbox` is display-only; chrome `TextInput` is rejected).

What a form TAB additionally needs — and this spec does NOT build — is called out
so the API does not paint it into a corner: a new `TabKind::Form` (alongside the
existing `ReadOnlyOutput` virtual-document seam, TabManager.h:31-37); ownership of
per-field input STATE (a text field's buffer, a checkbox's checked bit) which,
unlike header/footer, is read-write and must live server-side and survive
snapshots; and event routing for edit/toggle/submit (a `TextInput` widget would
carry a `command` binding to a Lua-registered command that receives the field
value). The descriptor TYPE already has the slots this needs (`id`, `kind`,
`value`, `command`); only the per-context VALIDATION table (and the runtime
STATE/event wiring) changes — the chrome table rejects `TextInput` and treats
`Checkbox` as display-only, and a future form table permits a `TextInput` and a
toggleable `Checkbox` with their `value`/`command`. So the form work is additive
(a new context table + state + a `TabKind`), not a descriptor reshape.

## Invariants

- Server owns layout/rendering; the client is a dumb renderer. The composition
  and its lowering live server-side; only the existing `AccessibilityNode`
  projection crosses the wire — NO protocol/wire change.
- The per-frame render path runs NO Lua. Composed values are literals or built-in
  provider lookups; Lua runs only at load (`ssg.chrome`) and at click (command
  dispatch).
- Whole-call / whole-script fail-loud: an invalid descriptor rejects the entire
  `ssg.chrome` call, and any script error rolls the composition back to the prior
  state (built-in or previously-composed) — matching `theme.set`/`style.define`.
- With NO `ssg.chrome` call, the header/footer output is byte-identical to today
  (the built-in projection path is untouched when a region is uncomposed).
- Composed rows go through `WidgetStack::resolve`, so non-overlap, rank-collapse,
  clamp-truncate, and second-center fail-loud hold with no new layout code.
- The `WidgetKind` set stays CLOSED; `init.lua` composes trees of these
  primitives, it does not define new kinds. Adding a `SemanticRole` or `Style`
  glyph key still requires its existing wiring sites; this spec adds none.
- Every `init.lua` API function is documented in `doc/config.md`
  (`test_config_doc`); `ssg.chrome` must be.

## Considerations

- **Command validation timing.** A widget’s `command` may forward-reference a
  `ssg.register_command` defined later in the same script, or a built-in. So the
  command string is NOT validated at compose time (that would impose an ordering
  constraint); an unknown command at CLICK time is surfaced/failed then, exactly
  as `keymap.bind` treats its command target. `provider` ids and `kind`s, by
  contrast, ARE validated at compose time (closed sets).
- **The input-line reservation in a composed header.** A composed header is
  laid out over the header width MINUS the `inputLineReservation` floor when a
  picker is open (ShellState.cpp:498-503), identical to the built-in header, so
  the palette input line has room and composed fields do not reflow as the query
  grows (spec-input-line.md). The input line + ghost are placed by the existing
  C++ seam after the composed left group; the composition never emits them.
- **Value exclusivity.** `value` and `checked` each carry EXACTLY one of a
  literal or a `provider` when present; a field that is required per the per-kind
  matrix must be present, a forbidden one must be absent, and a present `Value`
  with both/neither literal+provider is a compose-time error. (`Spacer` has no
  `value`; a `Checkbox` requires a `checked` source; see the matrix.)
- **Widget count / cost (concrete caps).** The composition is bounded and the
  decoder rejects past explicit limits with a path-qualified message (whole-call
  reject): max WIDGETS PER ROW-SIDE 64; max TOTAL widgets per `ssg.chrome` call
  256. Over a limit → `<path>: exceeds <limit-name> (<n> > <max>)`; negative
  `separator`/`width` are likewise fail-loud (`must not be negative`). In CHROME
  the per-side cap is what bites (a header's one side + a footer's three slots is
  bounded well under 256); the total cap is a forward-looking ceiling for the
  multi-row FORM reuse. A nesting-DEPTH cap lands with the nested/forms decoder
  (a `Container` can recurse there); the phase-1 chrome schema is flat, so depth
  is bounded by the fixed shape and no depth guard ships now. The numbers are
  mechanism (tunable) not invariant.
- **Closed-vocabulary honesty.** There is no `Button` kind; a clickable button is
  a `Label`/`Field` with a `command`. The vocabulary stays the shipped six.

## Risks and Mitigations

- **Nested-table decoder is new surface (injection/crash).** Mitigation: the
  decoder is pure and total over Lua values, count-bounded (per-side/total caps)
  with numeric fields range-checked, and has a reference-impl round-trip oracle
  (table → descriptor → compare) incl. malformed cases; it cannot execute Lua,
  only read values. (A nesting-depth cap arrives with the nested/forms decoder.)
- **Replace path diverges from built-in geometry OR interaction.** Mitigation:
  composed rows use the SAME `WidgetStack` lowering + input-line floor as the
  built-in path, and a provider widget inherits the built-in field's command
  coupling; the parity golden compares the FULL node incl. `commandId`, so a
  regression in either geometry or click behavior fails it.
- **Regressing the uncomposed default.** Mitigation: the built-in projection is
  untouched when a region is uncomposed; all existing goldens stay green WITHOUT
  regeneration.
- **Scope creep into forms.** Mitigation: interactive kinds are describable +
  validated but NOT wired; no `TabKind::Form`, no form state, no submit routing
  in this spec.

## Acceptance (Definition of Done)

- Observable: with no `init.lua` chrome, output is byte-identical (goldens without
  regeneration). With a sample `ssg.chrome` (a provider-backed header + a
  literal/label footer with a click command), the custom chrome renders and the
  click dispatches. Because this is user-visible output, the custom-render result
  needs VISUAL SIGNOFF before commit.
- Budgets: the per-frame render path executes no Lua (assert composition resolves
  via provider lookups only); reload stays within the existing script time/cap.
- Gates: `bash scripts/check.sh` green.
- Oracles:
  - Nested-table decode — reference-impl round-trip unit test (Lua table ↔
    descriptor tree) incl. fail-loud cases (unknown kind, unknown provider,
    both/neither literal+provider on `value`/`checked`, missing-required or
    present-forbidden field per kind, `TextInput` in chrome, header right/centre,
    left/right `Spacer` without width, second center, over each cap) with
    path-qualified messages.
  - Lowering — a composed provider-only header projects the SAME FULL node
    `(id,kind,rect,role,content,commandId)` as the built-in header including the
    provider's inherited command coupling (so a composed `follow` field keeps its
    click action — the parity check compares `commandId`, not just text/shape);
    a composed footer with an explicit `command` hit-tests to that command.
  - Replace — a composed header omits the built-in fields it did not reference;
    an uncomposed footer is unchanged.
  - Reload rollback — a script that calls `ssg.chrome` then errors leaves the
    prior chrome intact.
  - Docs — `test_config_doc` green with `ssg.chrome` documented.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define the descriptor value types (`WidgetDescriptor` with `value`/`checked`/`width`/`role`/`command`, `RowDescriptor`, `ChromeComposition`; `Value` = literal XOR provider) + the pure nested-table decoder enforcing the per-kind CHROME-context field matrix, path-qualified fail-loud errors, and the concrete caps (64/side, 256/call; negatives rejected). No Lua wiring yet. | new `include/ssg/ChromeComposition.h`, `src/ChromeComposition.cpp`, `tests/test_chrome_composition.cpp`, a `cmake/components/*.cmake` | round-trip decode unit tests incl. every fail-loud case (per-kind required/forbidden field, value exclusivity, `TextInput`-in-chrome, header right/centre, spacer width, caps) | decoder purity; closed kinds; value exclusivity |
| 2 | Lower a `RowDescriptor` to a `WidgetStack` + `AccessibilityNode`s (switch on `WidgetKind`; resolve `provider` vs literal; inherit provider value/label/command unless overridden; carry `command`→`commandId`; a widget `role` overrides the region default; `Spacer` occupies space but emits no node). Build-only reuse of the shipped stack; a `ChromeProviderResolver` supplies provider `(value,label,command)`. The input-line floor is applied by the phase-3 caller (the composed header is laid out over the same fieldWidth the built-in uses). DONE, green. | `include/ssg/ChromeLowering.h`, `src/ChromeLowering.cpp`, `tests/test_chrome_lowering.cpp` | hand/golden: literal/provider/checkbox/spacer/drop cases + parity — a provider-only composed header == built-in header (`computeShellLayout`) span-for-span INCLUDING `commandId` | non-overlap; server-owned; no wire change |
| 3 | Route the built-in header/footer projection through the composition: when a region is composed, lower it; else keep the built-in path verbatim. As-built: `ShellLayoutRequest` carries `std::optional<ChromeComposition> composedChrome` + a `ChromeProviderResolver`; `computeShellLayout` branches header (left-group replace over the input-line-floored fieldWidth) and footer (whole-region replace, FooterField nodes). Extracted `Rect`/`GridSize` into `include/ssg/Geometry.h` to break the new `ShellState.h`↔`Widget.h` include cycle. | `include/ssg/Geometry.h`, `include/ssg/ShellState.h`, `include/ssg/Widget.h`, `src/ShellState.cpp`, `tests/test_ui_layout.cpp` | existing `ui_layout`/`test_hit_test`/`test_render` green WITHOUT regeneration (golden `shellLayoutMatchesTheCommittedGolden` unchanged) + replace oracles (composed replaces built-in; one-region composed leaves the other built-in; composed provider resolves + input line coexists) | uncomposed default byte-identical |
| 4 | Add the `ssg.chrome` API function (3rd `kApiFunctions` entry + installer + nested-table entry point), staged into the reload lifecycle with rollback. As-built: `LuaCommandHostOptions.chromeProviders` injects the valid provider ids; a `lua_State`→`ChromeValue` walker (depth+node+stack-checked, cycle-safe) feeds `decodeChromeComposition`; the decoded composition stages into `RegistrationTransaction.stagedChrome` (last-wins), publishes to `Impl::publishedChrome` on the same success point as commands, and is dropped on rollback; `composition()`/`ScriptHost::chromeComposition()` surface it; `ScriptHost` sources providers from `defaultStatusFieldProviders`. Minimal `ssg.chrome` doc added to keep `test_config_doc` green (expanded in step 5). | `include/ssg/LuaCommandHost.h`, `src/LuaCommandHost.cpp`, `include/ssg/ScriptHost.h`, `src/ScriptHost.cpp`, `doc/config.md`, `tests/test_lua.cpp` | script-host tests: compose stages/surfaces; error rolls back (prior kept, drop-call reverts to built-in); second call last-wins; invalid descriptor fails loud; cyclic table rejected | whole-script fail-loud; no per-frame Lua |
| 5 | Document `ssg.chrome` (the descriptor vocabulary, provider ids, click commands, replace semantics) + record the deferred form-tab seam. | `doc/config.md`, `doc/spec-lua-widget-composition.md` | `test_config_doc` green | no wire/API drift |
| 6 | WIRE the staged composition into the render path, then ship a sample. AS-BUILT: (a) `EditorRuntime::setComposedChrome` stores into `Impl::composedChrome` and calls `session->advanceRevision()` only when the value CHANGES. (b) app funnel extracted to `apps/init_script.{h,cpp}` (`ssg::app::evaluateInitScript`, compiled into ssg_app + ssg_startup_probe + test_ssg_app); both startup (`loadInitScript`) and reload (`drainAndEvaluate`) push `runtime.setComposedChrome(scripts.chromeComposition())`. (c) `shellView` builds the resolver capturing the projected+bound `StatusField`s by value; sets `request.composedChrome`/`chromeProviderResolver`. (d) sample `doc/examples/init-chrome.lua`. | `include/ssg/EditorRuntime.h`, `src/EditorRuntime.cpp`, `src/runtime/snapshot.cpp`, `apps/init_script.{h,cpp}`, `apps/ssg_main.cpp`, `cmake/components/{ssg-app,startup-benchmark}.cmake`, `tests/runtime/test_runtime_presentation.cpp`, `tests/test_ssg_app.cpp`, `doc/examples/init-chrome.lua` | runtime oracle `composedChromeReplacesBuiltinChromeAndTracksRevision` (compose replaces built-in path field w/ resolved value+label+inherited command; identical re-push = no revision bump; clear reverts + bumps); host oracle `evaluateInitScriptPushesChromeCompositionToTheRuntime` (both funnel calls); + user VISUAL SIGNOFF of the sample render | uncomposed default byte-identical; delta clients repaint on chrome change |

## Rationale (optional, skippable)

spec-chrome-stacks (done) funneled all chrome through the closed-primitive
`WidgetStack` specifically so a data-composition binding would be additive. This
spec cashes that in: the only genuinely new machinery is the nested-table decoder
and a richer `WidgetDescriptor` that finally gives `WidgetKind` a consumer;
layout, projection, hit-testing, and the reload lifecycle are all reused. Keeping
the descriptor region-agnostic (a `RowDescriptor` serves header, footer, and a
Column of form rows) is what lets the same API grow into a form builder without a
second vocabulary, while deferring form STATE/submit keeps this spec's surface
small and its default path provably neutral.
