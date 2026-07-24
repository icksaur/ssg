# spec-diff-tint-hue-fidelity

Status: DRAFT. Fixes a REGRESSION introduced by this session's own prior work
(`doc/spec-color-depth-defaults.md`, commit `c3d0003`): the shipped default
theme's diff tints now render as an off-theme, non-git-semantic, HARDCODED
black/blue/red palette instead of shades of the theme's own green/red/orange
Git-anchor colors, because the tightened Indexed256 distinctness gate forces
the derivation into `fixedFallback()` — a literal color palette unrelated to
the configured theme — for the shipped theme, which is not a degenerate case.
This spec also closes the underlying design gap that let a HARDCODED color
palette exist in the codebase at all, undetected by the project's own
color-authority scanner, and states the forward invariant that no presentation
color may ever be a literal unrelated to the active theme.

## Goals

After this change: the default (and any reasonable) theme's diff tints are
recognizably shades of THAT theme's own designated Added/Deleted/Modified
Git colors (green/red/orange for the shipped theme) — desaturated and
darkened, per the existing, correct design intent — at every color depth the
client actually renders, including Indexed256. A deleted file's tint reads as
a red family; an added file's as green; a modified file's as the theme's own
modified-color family (orange, for the shipped theme). No literal, absolute
RGB constant is used to render a diff tint for a normal (non-degenerate)
theme — the black/dark-blue/bright-blue/bright-red palette the user observed
must not appear for the shipped theme at any depth. The design is provably
theme-agnostic: a differently-hued or differently-lit user theme (once
themes are user-configurable — see Considerations) still produces
correctly-hued, correctly-distinct tints without per-theme tuning in the
derivation code.

## Design

### Root cause (confirmed by direct inspection, not inferred)

`deriveDiffTints` (`src/Theme.cpp:283-328`) derives all six tints from the
theme's own `GitAdded`/`GitDeleted`/`GitModified` anchors via
`strongestReadableTint`, then gates the WHOLE derived set behind
`readable() && distinct()` (`~312-313`). `distinct()` was extended
(`doc/spec-color-depth-defaults.md`) to also require pairwise CIE ΔE
separation at `ColorDepth::Indexed256`, with per-depth thresholds
(`kKindDeltaETruecolor=4.0`/`kKindDeltaEIndexed256=5.0`, etc., `~94-104`).
For the SHIPPED default theme, the derived (theme-hued) six-tuple now fails
this Indexed256 check — confirmed directly: `deriveDiffTints`'s output for
the shipped theme is bit-for-bit `fixedFallback(false)`
(`{0,0,0}, {0,0,95}, {0,0,135}, {0,0,95}, {0,0,135}, {95,0,0}}`,
`~272-278`) — black/dark-blue/blue/red, at BOTH Truecolor and Indexed256
(the fallback is chosen once the derived path fails and is then used
unconditionally at every depth, since `DiffTints` carries one static set of
RGB values resolved per-depth at render time, not a different literal set
per depth).

This is a genuine regression from the ALREADY-EXISTING documented rationale
in `deriveDiffTints`'s own comment (`~306-311`): "A fixed set rescues only a
degenerate theme whose near-monochrome anchors make the derived tints
indistinguishable." The shipped theme's anchors — `GitAdded={76,175,80}`
(green), `GitDeleted={239,74,74}` (red), `GitModified={212,149,106}`
(orange) — are NOT near-monochrome; they are three clearly hue-separated
colors. The fact that they now trigger fallback proves the DERIVATION
SEARCH (not the theme) is the actual defect: `strongestReadableTint`
(`~215-227`) walks a SINGLE fixed weight/saturation profile per kind
(row: 40% weight/60% retained saturation; word: 72%/85%, `~299-303`) and
stops at the first weight where `readable()` (which already checks both
depths) passes — it has NEVER accounted for whether the resulting TRIPLE
stays mutually distinct once quantized; distinctness is checked only as an
all-or-nothing gate on the finished set, with no ability to nudge an
individual kind's tint further from another kind's if they end up too close
post-quantization. Tightening the ACCEPTANCE gate without also giving the
SEARCH a way to satisfy the new, stricter target was the actual mistake —
this spec fixes the search, not by loosening the gate back down (which
would silently reintroduce the original invisible-grey bug), but by making
the search good enough to meet the existing, correct target.

### Fix 1 — joint, hue-preserving derivation search (the real fix)

Replace the current "derive independently per kind, gate the whole set,
give up to a literal fallback on any failure" flow with a search that:

- Still derives each kind's hue SOLELY from its own theme Git anchor
  (`GitAdded`/`GitDeleted`/`GitModified`) — never a hardcoded hue.
- For the row tint tier, instead of one fixed (weight, retainedSaturation)
  pair applied uniformly to all three kinds, allow the search to walk BOTH
  weight and retained-saturation jointly per kind, continuing past the
  first-readable point to also require the CURRENT candidate is
  `distinct()`-separated from the background and from the SAME tier's other
  two already-chosen kinds (row-vs-row, word-vs-word) at BOTH Truecolor and
  Indexed256 — i.e. distinctness becomes part of the per-kind stopping
  condition, not a global post-hoc gate over independently-chosen values.
  Process kinds in a fixed, deterministic order (Added, then Deleted, then
  Modified) so each new kind's search is aware of the already-fixed
  colors before it.
- **Deterministic bounds and tie-break (settled, not left to
  implementation choice):** candidates are enumerated on a fixed integer
  grid — weight and retainedSaturation each stepped in whole percentage
  points from the kind's existing starting ceiling (row: 40%/60%; word:
  72%/85%, unchanged as upper bounds) DOWN to 1%, in that order (weight
  outer loop, retainedSaturation inner loop, both strictly decreasing, so
  the walk is finite — at most 100×100 candidates per kind — and always
  terminates). The FIRST candidate (in this fixed enumeration order) that
  passes readable() AND is distinct from the background and from every
  already-fixed kind at this tier, at both depths, is chosen — ties (more
  than one candidate at the same weight passing) break to the HIGHEST
  retainedSaturation at that weight (preserves as much of the anchor's
  original hue/chroma as possible; weight is prioritized over saturation
  because weight controls how strongly the anchor blends into the
  background — the primary lightness/wash-strength knob — while retained
  saturation is the secondary color-purity knob). If the entire grid is
  exhausted with no passing candidate for a kind, derivation for the WHOLE
  six-tuple fails and the rescue path (Fix 2) is used — this is the only
  case a real (non-degenerate) theme should reach it, and Acceptance
  requires proving the shipped theme does not.
- If, after this joint search, the six-tuple is readable and distinct at
  both depths, return it — the shipped theme's clearly-separated anchors are
  expected to succeed here without ever reaching a fallback (this is the
  Acceptance oracle below).
- Fallback is reserved for GENUINELY degenerate cases (anchors that are
  themselves near-identical, e.g. the existing
  `nearMonochromeAnchorsUseAReadableDistinctFallback` fixture) where no
  weight/saturation combination can separate hues that do not exist in the
  input.

### Fix 2 — the rescue path must also be theme-derived, not a literal

`fixedFallback()`'s absolute RGB constants (`~272-278`) are themselves a
violation of this project's own color-authority principle (I22: "Theme is
the only source of color values") in spirit, even though they physically
live inside `Theme.cpp` (the one file authorized to mint color): a
LITERAL constant is not derived from the configured theme at all, and,
as observed, is not even git-semantically sensible (removed/deleted
rendered as blue, not red). Replace `fixedFallback()`'s hardcoded RGB
constants with a DERIVED rescue: given the theme's own background and
foreground luminance, construct three SYNTHETIC anchor hues at the
canonical hue ANGLES the Git roles conventionally use (green ≈ 120°,
red ≈ 0°, an orange/amber for modified ≈ 30–40°) at a chroma/lightness
matched to the theme's OWN background-relative envelope (e.g. derived from
`themeBackground`'s and the foreground's luminance, the same inputs the
primary derivation already uses) — then run the SAME `strongestReadableTint`
/ joint-search machinery on those synthetic anchors instead of the theme's
own (degenerate) ones. This is a hue-angle CONSTANT (a fixed point in the
derivation ALGORITHM, like `kFloorContrast`), not a hardcoded final COLOR —
the actual RGB values it produces still depend on and adapt to the theme's
background/foreground, satisfying "no independent color source" for any
theme, including a hypothetical future light theme or a theme with an
unusual background lightness. This closes the gap for good: no path in this
feature ever returns a color that is not a function of the active theme.

### Fix 3 — the color-authority scanner has a real gap; close it

`tests/test_theme.cpp`'s `sourceAndConfigHaveNoIndependentColorSources`
(`~589-627`) scans for hardcoded color literals via patterns including
`\bSrgbColor\s*[\{\(]` — requiring the literal TYPE NAME to appear at the
use site. `fixedFallback()`'s constants (`return {{0, 0, 0}, {0, 0, 95}, ...}`)
use brace-elision (aggregate initialization with no `SrgbColor` token
present) and so evade this scanner entirely — this is precisely why a
hardcoded, off-theme palette could exist in reviewed, tested code without
detection. Since Fix 2 removes `fixedFallback`'s literals entirely, this
gap is moot for the specific case that caused this bug, but the SCANNER
ITSELF should be strengthened so it cannot be defeated by brace-elision
again in the future — extend its pattern set to also flag a bare
three-or-four-integer 0–255 brace-initializer list (a structural pattern,
not a type-name match) within any non-excluded file, or add a targeted,
narrower check specific to `Theme.cpp` (the one file most likely to need
this exception in the future) asserting every color-producing function's
output is demonstrably a function of its `palette`/anchor PARAMETERS (see
Acceptance's hue-fidelity oracle, which is a stronger, more direct test of
the same property than a textual scan could ever be).

### Hue fidelity (the missing acceptance property)

The prior fold's `distinct()` fix proved COLORS ARE DIFFERENT FROM EACH
OTHER — it never proved they are the RIGHT colors. This is precisely how a
distinct-but-wrong-hue (blue instead of red) fallback slipped through
review undetected: every existing oracle checked contrast and separation,
none checked HUE. Add a hue-fidelity check: a derived (or rescued)
`addedRow`/`addedWord` must fall within a hue-angle tolerance of the
green family and correlate with the theme's own `GitAdded` anchor's hue;
`removedRow`/`removedWord` within tolerance of the red family and the
theme's `GitDeleted` anchor's hue; `modifiedRow`/`modifiedWord` within
tolerance of the theme's `GitModified` anchor's hue. This must hold at
BOTH Truecolor and Indexed256 — it is the oracle that would have caught
this regression immediately, and the one this spec's Acceptance section
requires be written FIRST (red-before-green) against the current
(fallback-triggering) code.

**Concrete hue computation (settled, not left to implementation choice):**
hue is the standard HSL hue channel (degrees, 0–360, computed from sRGB via
the conventional max/min-channel formula — the same well-known conversion
this codebase's existing `lab()`/`deltaE()` functions neighbor, but HSL hue
specifically, not CIE hue, since HSL hue is simpler, has no chroma-dependent
instability near gray, and is the natural fit for "is this reddish vs
greenish vs orangeish"). Hue DISTANCE between two angles is the standard
circular/wraparound distance: `min(|a-b|, 360-|a-b|)`, never a naive linear
subtraction (which would wrongly report ~350° apart as far when it is
actually 10° apart across the 0°/360° wrap). Tolerance: ±40° from the
anchor's own hue — chosen because the intentional darkening/desaturation
this feature applies can shift HSL hue slightly at very low
lightness/saturation (where hue becomes numerically less stable), and 40°
is comfortably narrower than the ~120° separation between the red/green/
orange family members themselves (so no tolerance window can overlap
another family's), while still admitting the shift. Validate this
concretely against the shipped theme's actual anchors
(`GitAdded={76,175,80}`, `GitDeleted={239,74,74}`,
`GitModified={212,149,106}`) during implementation; narrow the tolerance if
40° proves too permissive to actually distinguish families for these
specific anchors (it should not be, given ~120° family separation, but
confirm rather than assume).

## Invariants

- I22 (Color authority) — EXTENDED interpretation, made explicit and
  testable: no function in this codebase may return a presentation color
  that is not a computed function of the active `Theme`'s own palette/anchor
  values. A literal RGB constant used AS A FINAL COLOR (not as a tuning
  parameter of a derivation, like a contrast floor or a hue-angle constant)
  is a violation regardless of which file it lives in, including
  `Theme.cpp` itself. `fixedFallback`'s hardcoded palette (Fix 2 removes it)
  was — in hindsight — always such a violation; it merely predates this
  spec's tightened scrutiny.
- Diff content-consumer contract (`doc/features/workspace-live-diffs.md`) —
  unaffected; this spec touches only tint COLOR derivation, not diff
  computation.
- Existing readability/distinctness contract from
  `doc/spec-color-depth-defaults.md` — PRESERVED, not loosened: this spec
  does not reduce the Indexed256 distinctness requirement; it makes the
  search capable of MEETING that requirement with theme-derived hues
  instead of giving up to an unrelated palette.

## Considerations

- **User-configurable themes (forward-looking, out of scope to IMPLEMENT
  here, but a hard constraint on this fix's DESIGN):** this project's
  `Theme` class already supports arbitrary palette/role assignment
  (data-driven `.theme` files, `data/themes/default.theme`); a settings/UI
  surface for a user to author or switch themes at runtime is planned but
  not implemented by this spec. The derivation algorithm fixed here MUST
  be correct for ANY future palette a user supplies — not tuned to the
  shipped theme's specific numbers. Concretely: the existing
  `nearMonochromeAnchorsUseAReadableDistinctFallback` and
  `lightThemeFallbackRemainsReadableAndDistinct` fixture themes already
  test this generality; add at least one more synthetic fixture theme with
  a DIFFERENT, clearly-separated (non-degenerate) anchor hue arrangement
  (e.g. anchors rotated to unconventional hues, still well-separated) to
  prove the joint search generalizes and is not accidentally tuned to the
  shipped palette's exact numbers.
- The joint search's exact stepping strategy (how weight/saturation are
  walked, in what order, what step size) is a MECHANISM, not an invariant
  — implement whatever concretely satisfies the Acceptance oracles; do not
  over-specify the interpolation math here.
- `fixedFallback`'s synthetic hue angles (green≈120°, red≈0°, orange≈30–40°)
  are themselves a design choice belonging to this feature, not a project-
  wide invariant — document the chosen angles with a brief code comment
  (mirroring `kFloorContrast`'s comment style) explaining why those angles
  were chosen (matching conventional git-diff UI color conventions).
- The bare-aggregate-literal scanner strengthening (Fix 3) risks new false
  positives (e.g. legitimate non-color 3-integer literals elsewhere in the
  codebase, like `GridSize{80, 24}` or similar). If a structural brace-list
  scan proves too noisy, the NARROWER Theme.cpp-specific
  function-of-its-parameters test (also described in Fix 3) is an
  acceptable, sufficient alternative — implement whichever is more robust
  after a quick empirical check of the existing codebase for likely false
  positives.

## Risks and Mitigations

- The joint search could still fail to find a jointly-readable-and-distinct
  triple for SOME real (non-shipped) theme with unusually close anchor hues
  that are nonetheless not degenerate enough to be "near-monochrome" ⇒ the
  rescue path (Fix 2) exists for exactly this residual case, and — critically
  — is now ALSO theme-derived, so even a rescue no longer reproduces this
  bug's git-semantically-wrong blue/black palette.
- A hue-fidelity oracle with too tight an angular tolerance could itself
  force unnecessary fallback for a legitimately-hued but slightly-rotated
  theme color ⇒ choose a generous tolerance (e.g. ±30–40° from the anchor's
  own hue, wide enough to admit the darkening/desaturation this feature
  intentionally applies, which can shift hue angle slightly at extreme
  lightness) — validate empirically against the shipped theme's own anchors
  first, tighten only if that proves too loose to distinguish red from
  green.
- Removing `fixedFallback`'s literals could regress the EXISTING
  near-monochrome/light-theme fixture tests if the synthetic-hue rescue
  produces different (but still correct) values ⇒ update those tests'
  expected VALUES if needed, but their asserted PROPERTIES (readable,
  distinct, and now also hue-correct) must still hold — this is not a
  regression to paper over, it is those tests catching up to a stronger
  Rescue design.

## Acceptance (Definition of Done)

- Observable: the shipped default theme's diff tints, rendered in the TUI
  against a real git repo with add/modify/delete, show clearly green-family
  (added), red-family (removed/deleted), and the theme's own modified-color
  family (orange-family, for the shipped theme) tints — NOT the
  black/dark-blue/blue/bright-red palette from this bug — at BOTH the new
  Truecolor default AND a forced `SSG_COLOR_DEPTH=256`. Visual signoff
  required (this is the same user-visible rendering property the earlier
  color-depth-defaults spec targeted, now corrected).
- Gates: `bash scripts/check.sh` green with and without `SSG_TREESITTER`.
- Oracles:
  - hue-fidelity (write FIRST, must FAIL against current/`c3d0003` code):
    a property test asserting the shipped theme's `deriveDiffTints` output
    resolves, at BOTH Truecolor and Indexed256, to hue angles within
    tolerance of `GitAdded`/`GitDeleted`/`GitModified`'s own hues for the
    corresponding row/word tint pairs. This is the oracle this project's
    existing test suite was missing.
  - no-fallback-for-non-degenerate-theme: **concrete testability seam
    (settled, not left ambiguous)** — `deriveDiffTints` gains an internal
    result type (or the existing function is split into a testable helper
    plus a thin public wrapper) that reports WHICH path produced the
    returned tints: `Primary` (the joint anchor-derived search succeeded)
    or `Rescue` (the search exhausted and the synthetic-hue fallback was
    used). This is test-only-visible detail (e.g. an overload or a
    `namespace ssg::testing`-style accessor), not a change to
    `DiffTints`'s public shape or the render path, which only ever needs
    the final colors. The test asserts the shipped default theme's path is
    `Primary`, directly and unambiguously — no need to infer it indirectly
    from hue correlation. This directly answers reviewer scrutiny: the
    hue-fidelity oracle alone cannot distinguish "primary path, correctly
    hued" from "rescue path, ALSO correctly hued" (both are expected to
    pass hue-fidelity once Fix 2 lands) — the path-reporting seam is what
    makes "the shipped theme never needs rescue" a directly provable,
    non-circular fact rather than an inference.
  - generality: the near-monochrome and light-theme fixture tests continue
    to pass (updated values as needed, per Risks); a NEW synthetic
    non-degenerate, non-shipped-theme fixture (Considerations) also passes
    hue-fidelity and readability/distinctness at both depths, proving the
    search is not tuned to the shipped palette specifically, AND is
    confirmed (via the same path-reporting seam) to take the `Primary`
    path, not `Rescue` — a non-degenerate fixture theme silently depending
    on rescue would itself indicate the joint search is too weak.
  - regression: `test_color.cpp`/`test_theme.cpp`'s existing readability/
    distinctness gates (from `doc/spec-color-depth-defaults.md`) continue to
    pass unmodified in their PROPERTY (only fallback-triggered VALUES may
    change if Fix 2 changes what the rescue produces).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Red-before-green: add the hue-fidelity property test against the shipped theme at both depths (must fail against current `c3d0003` code, which returns `fixedFallback`'s blue/black palette) | `tests/test_theme.cpp` | fails pre-fix | - |
| 2 | Add the path-reporting testability seam (Primary/Rescue) to `deriveDiffTints`; implement the joint, hue-preserving derivation search (Fix 1) so the shipped theme's anchors succeed via the Primary path without rescue | `src/Theme.cpp`, `include/ssg/Theme.h` if the seam needs a declared type | step 1's oracle passes for the shipped theme; new test asserts the shipped theme's path is `Primary` | I22 |
| 3 | Replace `fixedFallback`'s hardcoded RGB constants with the theme-derived synthetic-hue rescue (Fix 2); update near-monochrome/light-theme fixture test expectations if their taken values change | `src/Theme.cpp`, `tests/test_theme.cpp` | near-monochrome/light-theme fixtures still readable+distinct+now hue-correct at both depths; new synthetic non-degenerate fixture (Considerations) passes | I22 |
| 4 | Strengthen or replace the color-authority scanner to close the brace-elision gap (Fix 3) | `tests/test_theme.cpp` | scanner now flags a reintroduced bare-literal palette; no new false positives against the current codebase | I22 |
| 5 | Regenerate any golden/fixture files whose rendered output embeds the (now-corrected) diff-tint hex values | `tests/fixtures/tui/*.txt` (`SSG_REGEN_GOLDEN=1`), any other fixture embedding theme/diff_tints hex values (search beyond `tests/fixtures/tui/`) | dual-gate green | snapshot/delta symmetry |
| 6 | Manual/automated end-to-end visual confirmation against a real git repo (mirroring the verification already done for the color-depth-defaults fix) | - | real captured terminal bytes show green/red/orange family SGR codes, not blue/black, at both Truecolor and forced-256 | - |

## Rationale (skippable)

This bug is a direct consequence of fixing one property (distinctness) in
isolation from another (hue correctness) that nobody had written an oracle
for. The earlier spec's own Risks section explicitly named this exact
failure mode ("Indexed256-aware distinct() could force MOST or ALL real
themes into the fixed fallback set, making the derived, theme-hued path
effectively dead code") and flagged it as a risk to validate empirically —
that validation did not happen thoroughly enough before landing, and it
happened to the shipped theme. The fix is not to loosen the gate (that
reintroduces the ORIGINAL invisible-grey bug) but to make the search smart
enough to meet it, and to finally close the parallel gap that let an
unrelated, hardcoded, git-semantically-backwards color palette exist in the
one file this project trusts as the sole source of color.
