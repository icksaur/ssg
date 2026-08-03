# spec-background-tint-adjust

> **ARCHIVED (superseded by `doc/spec-semantic-color-themes.md`).** This design
> tuned brightness/saturation of derived background washes in HSL
> (`theme.background`, `adjustBackgroundTint`, `BackgroundTintAdjustments`)
> because washes were DERIVED from a shared 16-color palette and could not be set
> independently. The semantic-color-themes refactor gives every role its own
> color, so the diff and selection backgrounds (`diff_added`, `diff_removed`,
> `diff_modified`, `selection`) are set directly via `theme.set` with a plain
> `"#rrggbb"` value -- there is no derived wash left to scale. The `theme.background`
> command, the HSV adjustment, and the multiplier table described below no longer
> exist in the code. Retained only as a record of the retired design; see
> `doc/spec-color.md` for the current model.

## Goals

A user can tune the four background washes -- diff added, diff removed, diff
modified, and text selection -- for brightness and saturation from `init.lua`,
without editing the 16-color palette those washes derive from. Multipliers may
be above or below 1.0. At the shipped defaults of 1.0/1.0 the rendered output is
byte-identical to today.

The problem this solves: a background wash taken flat from a palette slot is
often too strong under the text drawn on top of it. The palette slot itself
cannot simply be dimmed, because the same slot also colors foreground text,
tabs, and tree rows, where full strength is correct.

## Design

### What is adjusted, and what is not

`ThemeSnapshot` gains one field, `BackgroundTintAdjustments`, carrying a
`TintAdjustment{brightness, saturation}` for each of four TARGETS:

- `diff_added` -- the `GitAdded` anchor; `DiffTints::addedRow`/`addedWord`
- `diff_removed` -- the `GitDeleted` anchor; `removedRow`/`removedWord`
- `diff_modified` -- the `GitModified` anchor; `modifiedRow`
- `selection` -- the `Selection` role; `selectionFill`

`DiffTints::modifiedWord` currently reuses the `GitAdded` color (an inserted
span inside a modified line reads as "added"), so it follows `diff_added`. That
keeps the existing "word mark is the same color as what it means" rule intact.

Targets are named for their MEANING, not their color: a theme may make added
cyan, and `diff_added` still identifies it. The mapping from the user's terms is
`inline yellow -> diff_modified`, `red remove -> diff_removed`,
`green add -> diff_added`, `blue text selection -> selection`.

**`diff_modified` is the modified-ROW wash, the yellow one.** Word marks inside
a modified line are painted with the add/remove colors, not a distinct yellow,
so they follow `diff_added`/`diff_removed` rather than `diff_modified`. If the
yellow the user wants to tune is a row background, this mapping is right; if it
is something drawn per-word, it is not, and that must be settled before
implementing.

Nothing else is adjusted. Foreground text, tab and tree highlights, status
colors, and the palette itself are untouched, which is the entire point: the
adjustment exists so a background can differ from the slot it derives from.

### Where the adjustment happens

Inside the existing derivation, not at render time. `deriveDiffTints` and
`deriveSelectionFill` gain the adjustments as a parameter and apply them to the
anchor color they already look up. Consequences that follow, and are required:

- Clients keep receiving finished colors on the snapshot and still never compute
  one (I22). A renderer cannot tell an adjusted tint from an unadjusted one.
- `applyThemeDefine` already re-derives both after a palette change, so a
  `theme.define` following an adjustment automatically re-applies it.
- Adjusting is likewise a re-derivation over the CURRENT palette, so the two
  commands compose in either order with the same result.

### The color model

Adjustment converts sRGB to HSL, scales `S` by `saturation` and `L` by
`brightness`, clamps both to `[0, 1]`, and converts back.

HSL over HSV or raw channel scaling because `L` is the axis a user means by
"brightness" for a background wash (scaling raw RGB channels also shifts
saturation, and HSV's `V` does not dim toward black the way a wash needs).

Two properties the mechanism must have, both of which are the whole reason this
is safe to ship at 1.0:

- **Identity is exact.** `adjust(c, 1.0, 1.0) == c` for every color, with no
  rounding drift. This is asserted directly over the palette and a broad sample,
  because it is what makes "no visible change at defaults" a fact rather than a
  hope. The implementation also short-circuits the neutral case. Measured, the
  round trip is exact for all 16.7M sRGB colors, so that short-circuit is not
  load-bearing TODAY -- it is a guard so identity cannot quietly stop holding if
  the rounding rule or color space is ever changed.
- **Clamping is saturating, not wrapping.** `brightness = 3.0` on an already
  light color yields white, never a wrapped dark value. Hue is never modified,
  so a wash cannot change identity under adjustment -- red stays red.

Multipliers must be finite and `>= 0`. Zero is meaningful (`saturation = 0` is
gray, `brightness = 0` is black). Negative and non-finite values are rejected.
There is no upper bound; clamping makes large values harmless and monotonic.

### Where the math lives

`src/color.cpp`, NOT `src/Theme.cpp`.

This is forced, not stylistic. `tests/test_theme.cpp`'s
`sourceAndConfigHaveNoIndependentColorSources` forbids the verbs
`lighten|darken|shade|tint|blend|gradient(`, `hsl(`, hex literals, and
`SrgbColor{` outside a fixed exemption list, and `Theme.cpp` additionally may
not contain a bare RGB triple. `src/color.cpp` is already exempt and is already
the home of color computation (`resolveColor`, `xterm256Color`). Putting the
HSL conversion there needs no change to the guard; putting it in `Theme.cpp`
would require weakening a guard that exists to protect exactly this boundary.

### Configuration surface

A new command, `theme.background`, alongside `theme.define` and sharing its
all-or-nothing validation.

**The argument shape is constrained, not chosen.** `LuaCommandHost` accepts
exactly one payload shape -- an optional FLAT `string -> string` table -- and
rejects a non-table, a non-string key, or a non-string value BEFORE dispatch
(`include/ssg/LuaCommandHost.h`, `src/LuaCommandHost.cpp`). Nested tables and
numeric values are therefore not expressible today. The command uses flat keys
with numeric strings, which is the same idiom `theme.define` already uses for
its `"#rrggbb"` values:

```lua
ssg.command("theme.background", {
    brightness = "0.85",                -- applies to every target
    saturation = "0.70",
    selection_brightness = "1.10",      -- per-target override
    diff_added_saturation = "0.55",
})
```

Keys are `brightness`, `saturation`, and `<target>_brightness` /
`<target>_saturation` for each of `diff_added`, `diff_removed`,
`diff_modified`, `selection` -- ten possible keys, all optional.

Resolution per target and axis: a per-target key wins; otherwise the global key;
otherwise 1.0. So `selection_brightness` alone leaves selection's saturation on
the global value.

Widening the Lua argument model to carry numbers and nested tables is
deliberately OUT OF SCOPE: it would change a seam every command shares, to gain
syntax only this command wants.

A global pair plus per-target overrides, rather than either alone, because the
expected workflow is "dim all the washes, then bring one back" while the four
start at different perceived intensities.

Rejected: an unknown key, a value that does not parse as a number, a negative or
non-finite multiplier. Rejection leaves the current adjustments untouched, with
no partial application.

## Invariants

- **I22 (color authority)** -- adjustment happens inside the theme derivation,
  so the theme remains the only source of color and clients still consume
  finished values. Multipliers are tuning parameters, not colors.
- **I8 (palette cardinality)** -- unaffected; adjustment never adds, removes, or
  edits a palette entry.
- Derived colors stay a pure function of their inputs: same palette + same
  adjustments -> same tints, on every client.
- The flat model holds. Adjustment scales one anchor color; it does not
  reintroduce blending toward Background, per-theme search, or readability
  gating (`doc/spec-color.md` History).

## Considerations

- **The Lua seam takes flat string tables only.** This shapes the whole config
  surface (see Design); it is a seam every command shares, so widening it for
  one command is out of scope. A design using nested tables or numeric values
  would be rejected before dispatch, and would read like a validation bug.
- **`ThemeSnapshot` is on the wire.** Adding a field requires `toValue` and
  `decodePresent` in `src/Protocol.cpp` (~4594-4620), the aggregate
  initialization at ~4619, and REGENERATION of the canonical goldens via
  `SSG_REGEN_PROTOCOL_FIXTURES=1 ./build/test_protocol`. A field added only to
  the struct compiles and silently fails to round-trip.
- **`applyThemeDefine` aggregate-initializes `ThemeSnapshot`** with `{}, {}` for
  the derived fields; a new field must be threaded there or it silently resets
  to default on every `theme.define`. This is the most likely bug in the change:
  it would look correct until a user calls both commands.
- **Adjustments must survive `theme.define`.** They are theme state, not a
  one-shot transform, so they live on the snapshot and re-apply on every
  re-derivation.
- **`test_theme.cpp` pins `defaultTheme()` equality** and the co-visible-role
  distinctness rules; a new field on the snapshot participates in `operator==`
  and must be identity-valued in the default theme or those tests fail --
  correctly.
- **Perceived brightness is not `L`.** Equal `brightness` on yellow and blue
  will not look equally strong; that is expected and is why the user tunes per
  target rather than globally.
- **Ansi16 erases fine adjustment.** With 16 swatches, a small multiplier change
  resolves to the same color. Not a defect; the adjustment is meaningful at
  Truecolor and mostly meaningful at Indexed256.

## Risks and Mitigations

- Rounding drift making 1.0/1.0 not identity -> short-circuit the no-op case;
  oracle asserts exact equality across the whole palette.
- Silent wire breakage -> protocol round-trip oracle over a NON-default
  adjustment, so a missing encode/decode fails rather than passing on zeros.
- Adjustment silently lost on `theme.define` -> explicit oracle that sets an
  adjustment, then calls `theme.define`, then asserts the tint is still adjusted.
- Scope creep into a readability search -> explicitly out of scope; multipliers
  are stated by the user, never solved for.

## Acceptance (Definition of Done)

- Observable: with no config, the rendered screen is byte-identical to before
  (verified by the existing terminal-parity and library-contract goldens passing
  UNCHANGED). With `theme.background{ brightness = 0.5 }` in `init.lua`, the
  four washes are visibly darker while text, tabs, and tree rows are unchanged.
  Needs user signoff, since the point is subjective appearance.
- Budgets: no measurable startup or per-frame cost; adjustment happens once per
  derivation, not per cell.
- Gates: `bash scripts/check.sh` green.
- Oracles: per Plan below.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `adjustBackgroundTint(SrgbColor, TintAdjustment)` in the sanctioned color home: sRGB->HSL, scale S and L, clamp to [0,1], convert back, with an exact no-op short-circuit | `include/ssg/color.h`, `src/color.cpp`, `tests/test_color.cpp` | test: identity is EXACT for all 16 default-palette colors at 1.0/1.0 (**verify by perturbation** -- remove the short-circuit, confirm at least one color drifts, restore); hand-computed HSL cases for a mid-tone; `saturation=0` yields gray (R==G==B); `brightness=0` yields black; large multipliers clamp to white/full saturation rather than wrapping; hue is unchanged for any multiplier | I22 |
| 2 | Add `TintAdjustment`/`BackgroundTintAdjustments` and thread them onto `ThemeSnapshot`; make `deriveDiffTints`/`deriveSelectionFill` take and apply them; identity in `defaultTheme()` | `include/ssg/Theme.h`, `src/Theme.cpp`, `src/DefaultTheme.cpp` | test: at identity, every derived tint equals the anchor palette color exactly (pins "no visible change"); with a non-identity adjustment, the tint differs from the anchor while the ANCHOR PALETTE ENTRY is unchanged (proves the palette is not mutated) | I8, I22 |
| 3 | Thread adjustments through `applyThemeDefine`'s aggregate init and re-derivation | `src/Theme.cpp` | test: set a non-identity adjustment, call `theme.define` changing an unrelated slot, assert the adjustment still applies (the silent-reset bug) | - |
| 4 | Wire `ThemeSnapshot`'s new field through the protocol codec and regenerate the canonical goldens | `src/Protocol.cpp`, `tests/test_protocol.cpp`, `tests/fixtures/protocol/*.hex` | test: round-trip a NON-default adjustment through encode/decode (a zero-valued round trip would pass even if the field were dropped) | - |
| 5 | Add the `theme.background` command: payload type, parsing of the flat numeric-string keys, validation (unknown key, unparseable number, negative, non-finite), all-or-nothing apply, re-derivation | `include/ssg/Theme.h`, `src/Theme.cpp`, `src/runtime/presentation.cpp`, `tests/test_theme.cpp` | test: a per-target key overrides the global one; a target with only one axis set inherits the global for the other; each rejection case leaves the previous adjustments intact (assert the snapshot is UNCHANGED, not merely that the call failed) | - |
| 5b | Register the command-argument codec for `theme.background` | `src/Protocol.cpp` (`buildCommandArgumentCodecRegistry`), `tests/test_protocol.cpp` | the existing `registryRejectsMissingEntries` test requires an entry for EVERY P0 command, so a missing codec fails the gate rather than silently dropping the payload; add a round-trip case carrying a non-default adjustment | P0-catalog |
| 6 | Add the init.lua dispatch and argument decoding, mirroring `theme.define` | `apps/ssg_main.cpp`, `include/ssg/InitScriptCatalog.h` | test: existing `test_config_doc` coverage gate forces a `doc/config.md` entry for the new command | - |
| 7 | P0 catalog cascade for `theme.background` | `data/required-commands.json`, `tests/test_required_commands.cpp` (list + category count + `static_assert`), `tests/runtime/command_cases.h` (+ `static_assert`), `doc/features/theme.md` | existing catalog-parity and feature-doc-union tests green | P0-catalog |
| 8 | Document the command and its four targets | `doc/config.md`, `doc/spec-color.md` | `test_config_doc` green | - |
| 9 | Verify: gate green, terminal-parity and library-contract goldens UNCHANGED (proving 1.0/1.0 really is a no-op), then a PTY capture at a non-default setting for signoff | - | full gate green; goldens not regenerated | - |

## Rationale

The multipliers are deliberately dumb. Every retired color design in
`doc/spec-color.md` failed the same way: it computed a color the user could not
predict, then had to be un-computed when it looked wrong on a real theme. A
stated multiplier is inspectable, composes with `theme.define` in any order, and
cannot surprise a theme author.

Adjusting inside the derivation rather than at render time is what keeps that
true across clients: the alternative -- shipping multipliers to each client and
having it scale colors as it paints -- would put color authorship back in the
renderer, which is precisely what I22 forbids.

Defaulting to 1.0/1.0 makes this change provably invisible: the terminal-parity
and library-contract goldens must pass unchanged, so the "no visible difference"
claim is enforced by existing tests rather than asserted.
