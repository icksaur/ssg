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

**Addendum 1 (empirical finding during implementation):** an early attempt
at a JOINT, ordered, cross-kind search (locking each Git-role kind's color
in turn and requiring it be distinct from every already-locked kind, at
Indexed256) found a genuine, irreducible conflict for the shipped theme's
`GitDeleted` (red) anchor — no interpolation weight simultaneously
satisfied readability and background-distinctness once the Added kind had
already claimed the same narrow dark-and-readable zone. This was NOT
resolved by loosening `kRetainContrast` alone (swept 0.80 down to 0.40,
still infeasible after the Added lock) — the JOINT/ORDERED design itself
was the problem, not a threshold value.

**Addendum 2 (final decision, supersedes Addendum 1's approach):** rather
than pursue a more complex backtracking search to rescue the joint design,
this spec DESCOPES mutual inter-KIND distinctness to Truecolor only
(Truecolor is the default, primary rendering path since
`doc/spec-color-depth-defaults.md` and works correctly today with no
further changes). At Indexed256 — now an explicitly secondary path — a
tint must still be readable and distinct from plain BACKGROUND (fixing the
original invisible-grey bug), but two different change KINDS are no longer
required to be mutually distinguishable from each other at that depth.
This eliminates the joint/ordered/locking search entirely: each kind is
derived fully independently again (see Design), which is both simpler and
sufficient for the stated priority (Truecolor correctness first).

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
correctly-hued tints, readable and background-distinct at every rendered
depth, without per-theme tuning in the derivation code (mutual distinctness
BETWEEN kinds is guaranteed at Truecolor only — see the descope below —
not at Indexed256).

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
post-quantization.

### DESCOPE (empirical finding during implementation, folded in before
completion): mutual inter-kind distinctness at Indexed256 is dropped

Implementation attempted a joint, per-kind search (locking kinds in a fixed
order and requiring each new kind's candidate be distinct from every
already-fixed kind at the same tier, at Indexed256) and found it
STRUCTURALLY infeasible, not merely mistunable: once the first kind
(Added) locks in a color, that color occupies the same narrow
readable-and-near-background zone every OTHER kind's readable candidates
also live in at Indexed256's coarse resolution — dark, low-saturation
washes of DIFFERENT hues still collapse numerically close to each other at
that depth. A documented sweep of the retain-contrast floor from 0.80 down
to 0.40 could not unblock the Deleted kind after Added was locked, at any
value — this is not a threshold-tuning problem, it is the inter-kind
distinctness-at-Indexed256 requirement itself being unsatisfiable for real,
legitimately-different-hued anchors sharing one dark background.

**Decision (explicit priority call): Truecolor is the primary, default-path
target and works correctly today with no changes needed — Indexed256 is a
secondary, explicitly-opted-into fallback path (`SSG_COLOR_DEPTH=256`, or a
genuinely non-truecolor terminal) and does not need the SAME strength of
guarantee.** Accordingly, DROP the pairwise inter-KIND distinctness
requirement (`kKindDeltaE`) at Indexed256 entirely — keep it, unchanged, at
Truecolor, where three real hues remain easily and cheaply separable and
this guarantee is worth keeping. At Indexed256, require ONLY: (a)
`readable()` (contrast against every syntax foreground — UNCHANGED,
`kRetainContrast`/`kFloorContrast` revert to ONE constant each again, no
Truecolor/Indexed256 split needed since there is no longer a competing
inter-kind constraint pulling in the opposite direction), and (b)
`distinct()`-from-BACKGROUND only (`kRowDeltaE`/`kWordDeltaE`, unchanged,
still checked at both depths) — i.e. a tint must still be genuinely
visible against the plain background at Indexed256 (the ORIGINAL bug —
identical-to-background — remains fixed), but two DIFFERENT kinds (e.g.
Added vs Modified) are no longer required to be distinguishable from EACH
OTHER at Indexed256. This is an accepted, explicit, documented degradation
for the secondary depth: at Indexed256 a user can reliably tell "something
changed here" (real separation from unmodified text) even if two change
KINDS render as a similar dark wash; at Truecolor (the default, primary
path) all three kinds remain both readable, mutually distinct, AND
correctly hued.

This ELIMINATES the joint/ordered/locking search entirely — with no
cross-kind constraint left to satisfy at Indexed256, and Truecolor's
existing constraints already achievable independently per kind (this is
exactly the ORIGINAL, pre-this-session `strongestReadableTint` shape,
which already passed Truecolor's requirements fine), each kind is derived
FULLY INDEPENDENTLY again — no fixed processing order, no lock-in, no
backtracking, no per-kind grid search needed. `strongestReadableTint`'s
existing single-weight-descent loop (already correct in spirit) simply
needs its stopping condition to ALSO require `distinct()`-from-background
at whichever depths currently apply (both, using the now-narrower
Indexed256 check above) before accepting a candidate — a small, local
change, not a new search algorithm.

**Addendum 3 (empirical finding during Fix 1 implementation, further
descopes hue-fidelity itself):** implementing Fix 1's stopping condition
found that, independent of any inter-kind interaction, a SINGLE kind's own
readable+background-distinct requirement is ALSO incompatible with hue
fidelity at Indexed256 for the shipped theme's real dark background,
because `strongestReadableTint`'s interpolation walks a straight RGB line
from background toward a saturation-reduced anchor: at LOW interpolation
weight (close to background) the candidate quantizes to Indexed256's
achromatic gray ramp (losing hue) while still passing `readable()` +
background-`distinct()`; at HIGH weight (closer to the anchor, hue
correctly ≈120° for `GitAdded`, within 3°) the candidate consistently
FAILS `readable()` at Indexed256 (contrast retention breaks down once the
quantized candidate diverges enough from background to carry visible
hue). An empirical per-weight sweep (1–100, in steps of 5, both roles)
found NO weight where hue-within-60°-of-anchor, `readable()`, and
background-`distinct()` all hold simultaneously at Indexed256 — this is a
genuine geometric limit of Indexed256's coarse palette against this
background, not a tunable threshold problem (see Fix 1 below).

**Decision (extends the Addendum 2 priority call to hue-fidelity itself):**
hue-fidelity is ALSO required at Truecolor only, not Indexed256. At
Indexed256, a tint must still be `readable()` and background-`distinct()`
(unchanged, still fixes the original invisible-grey bug — "something
changed here" remains genuinely visible), but its HUE is not required to
correlate with the anchor's hue at that depth — an Indexed256 rendering
may legitimately appear as a readable, background-distinct achromatic (or
off-hue) wash while Truecolor, the default and primary path, shows the
correct git-semantic hue. This is consistent with, and for the same
reason as, the existing inter-kind-distinctness descope (Addendum 2):
Indexed256 is an explicitly secondary, opted-into path and does not carry
the full guarantee bundle Truecolor does.



Replace `strongestReadableTint`'s stopping condition (currently: first
weight where `readable()` passes) with: first weight where `readable()`
passes AND the candidate is `distinct()`-from-background (row tier:
`kRowDeltaE`; word tier: `kWordDeltaE`) at BOTH Truecolor and Indexed256.
Each of the three kinds continues to be derived independently, in any
order (order no longer matters — there is no cross-kind interaction left).
After all three kinds are derived, two SEPARATE, existing checks run
Truecolor-only (not Indexed256, per the descopes above): `distinct()`'s
inter-kind pairwise comparison (confirms the three real hues stay mutually
separable), and the hue-fidelity oracle (confirms each kind's own hue
correlates with its own anchor's hue). Indexed256 has neither an
inter-kind check nor a hue-fidelity check — only readable()/background-
distinct(), as stated above.
- If, after this, the six-tuple is readable, background-distinct at both
  depths, AND Truecolor-inter-kind-distinct, return it — the shipped
  theme's clearly-separated anchors are expected to succeed here without
  ever reaching a fallback (this is the Acceptance oracle below).
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
derivation (independently per kind, per the simplified Fix 1 design above —
no joint/ordered search) on those synthetic anchors instead of the theme's
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
tolerance of the theme's `GitModified` anchor's hue. Per Addendum 3 above,
this must hold at TRUECOLOR ONLY — it is the oracle that would have
caught this regression immediately at the primary/default depth, and the
one this spec's Acceptance section requires be written FIRST
(red-before-green) against the current (fallback-triggering) code, initially
covering both depths to prove the regression, then narrowed to Truecolor
once Addendum 3's Indexed256 infeasibility is folded in.

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
  `doc/spec-color-depth-defaults.md` — the READABILITY requirement
  (`kRetainContrast`/`kFloorContrast`) and the BACKGROUND-distinctness
  requirement (`kRowDeltaE`/`kWordDeltaE`) are UNCHANGED (one constant
  each again, no per-depth split needed) and apply at both depths, so the
  original invisible-grey bug (a tint identical to background) remains
  fixed at Indexed256 exactly as before. The INTER-KIND distinctness
  requirement (`kKindDeltaE`) is EXPLICITLY DESCOPED to Truecolor only, per
  a deliberate priority decision recorded above — Indexed256 no longer
  guarantees two different change KINDS render as visually distinguishable
  from EACH OTHER, only that each remains readable and distinguishable
  from plain background. This is a narrowing of what
  `doc/spec-color-depth-defaults.md` established, made explicit and
  intentional, not an accidental regression.

## Considerations

- **User-configurable themes (forward-looking, out of scope to IMPLEMENT
  here, but a hard constraint on this fix's DESIGN):** this project's
  `Theme` class already supports arbitrary palette/role assignment (see
  `theme.define`, `doc/spec-config.md`); the derivation algorithm fixed
  here MUST be correct for ANY future palette a user supplies — not tuned
  to the shipped theme's specific numbers. Concretely: the existing
  `nearMonochromeAnchorsUseAReadableDistinctFallback` and
  `lightThemeFallbackRemainsReadableAndDistinct` fixture themes already
  test this generality; add at least one more synthetic fixture theme with
  a DIFFERENT, clearly-separated (non-degenerate) anchor hue arrangement
  (e.g. anchors rotated to unconventional hues, still well-separated) to
  prove the per-kind derivation generalizes and is not accidentally tuned
  to the shipped palette's exact numbers.
- The per-kind derivation's stepping strategy (Fix 1: `strongestReadableTint`'s
  existing single-weight-descent loop, now ALSO requiring
  background-distinctness at its stopping condition) is a small, local
  change to an already-correct-in-shape function — no joint/ordered/locking
  search is needed once inter-kind distinctness is scoped to Truecolor only
  (Design), since there is no longer any cross-kind interaction to
  coordinate. This is deliberately the SIMPLER of the two designs this spec
  considered; the more complex joint-search design was abandoned during
  implementation when it proved structurally infeasible for the shipped
  theme, not merely mistunable (see Design).
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

- Each kind's independent derivation could still fail to find a
  readable-and-background-distinct candidate for SOME real (non-shipped)
  theme with an unusually extreme anchor (e.g. near-identical to
  background itself) ⇒ the rescue path (Fix 2) exists for exactly this
  residual case, and — critically — is now ALSO theme-derived, so even a
  rescue no longer reproduces this bug's git-semantically-wrong blue/black
  palette.
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
  black/dark-blue/blue/bright-red palette from this bug — at the new
  Truecolor default (the primary target). At a forced `SSG_COLOR_DEPTH=256`,
  each tint remains readable and visibly different from plain background
  (the original bug is fixed there too), though two different change kinds
  are no longer guaranteed to be mutually distinguishable from each other,
  nor hue-correlated with their anchor, at that depth (an explicit,
  documented priority call — see Design). Visual signoff required (this is
  the same user-visible rendering property the earlier color-depth-defaults
  spec targeted, now corrected, with Truecolor as the primary target).
- Gates: `bash scripts/check.sh` green with and without `SSG_TREESITTER`.
- Oracles:
  - hue-fidelity (write FIRST, must FAIL against current/`c3d0003` code):
    a property test asserting the shipped theme's `deriveDiffTints` output
    resolves, at TRUECOLOR (per Addendum 3's descope), to hue angles within
    tolerance of `GitAdded`/`GitDeleted`/`GitModified`'s own hues for the
    corresponding row/word tint pairs. This is the oracle this project's
    existing test suite was missing.
  - no-fallback-for-non-degenerate-theme: **concrete testability seam
    (settled, not left ambiguous)** — `deriveDiffTints` gains an internal
    result type (or the existing function is split into a testable helper
    plus a thin public wrapper) that reports WHICH path produced the
    returned tints: `Primary` (each kind's independent derivation
    succeeded) or `Rescue` (a kind's derivation exhausted and the
    synthetic-hue fallback was used). This is test-only-visible detail
    (e.g. an overload or a `namespace ssg::testing`-style accessor), not a
    change to `DiffTints`'s public shape or the render path, which only
    ever needs the final colors. The test asserts the shipped default
    theme's path is `Primary`, directly and unambiguously — no need to
    infer it indirectly from hue correlation. This directly answers
    reviewer scrutiny: the hue-fidelity oracle alone cannot distinguish
    "primary path, correctly hued" from "rescue path, ALSO correctly hued"
    (both are expected to pass hue-fidelity once Fix 2 lands) — the
    path-reporting seam is what makes "the shipped theme never needs
    rescue" a directly provable, non-circular fact rather than an
    inference.
  - inter-kind Truecolor distinctness (unchanged from
    `doc/spec-color-depth-defaults.md`): the shipped theme's three row
    tints and three word tints remain pairwise distinct from EACH OTHER at
    Truecolor (the existing `kKindDeltaE` check, now scoped to Truecolor
    only per the descope above) — this proves the priority decision did
    not silently also weaken the PRIMARY depth's guarantee.
  - generality: the near-monochrome and light-theme fixture tests continue
    to pass (updated values as needed, per Risks); a NEW synthetic
    non-degenerate, non-shipped-theme fixture (Considerations) also passes
    Truecolor hue-fidelity, readability/background-distinctness at both
    depths, and Truecolor inter-kind distinctness, proving the derivation
    is not tuned to the shipped palette specifically, AND is confirmed (via
    the same path-reporting seam) to take the `Primary` path, not
    `Rescue` — a non-degenerate fixture theme silently depending on rescue
    would itself indicate the per-kind derivation is too weak.
  - regression: `test_theme.cpp`'s BACKGROUND-distinctness gate (from
    `doc/spec-color-depth-defaults.md`; `deriveSelectionFill`'s separate
    readability gate mentioned alongside it here was later REMOVED, see
    commit 5319ab5 -- `selectionFill` is now a flat anchor color with no
    readability derivation to regress) continues to pass unmodified in
    its PROPERTY at both depths (only fallback-triggered VALUES may
    change if Fix 2 changes what the rescue produces); the INTER-KIND
    distinctness gate's and the HUE-FIDELITY gate's scope both narrow to
    Truecolor-only, an intentional, documented change to those tests'
    assertions (Addendum 2 and Addendum 3 respectively), not a silent
    regression.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Red-before-green: add the hue-fidelity property test against the shipped theme at both depths (must fail against current `c3d0003` code, which returns `fixedFallback`'s blue/black palette) | `tests/test_theme.cpp` | fails pre-fix | - |
| 2 | Scope `distinct()`'s inter-KIND check (`kKindDeltaE`) to Truecolor only; keep BACKGROUND-distinctness (`kRowDeltaE`/`kWordDeltaE`) and `readable()` (`kRetainContrast`/`kFloorContrast`) unchanged, one constant each, at both depths (the descope) | `src/Theme.cpp` | existing distinctness/readability tests updated to reflect Truecolor-only inter-kind scope; background-distinctness tests unchanged | I22 |
| 3 | Fold background-distinctness into `strongestReadableTint`'s existing per-kind stopping condition (Fix 1) so each kind is derived independently at both depths; narrow step 1's hue-fidelity oracle to Truecolor-only (Addendum 3 — an empirical per-weight sweep found single-kind hue-fidelity itself, not just inter-kind separation, is infeasible at Indexed256 against the shipped theme's real background); add the path-reporting testability seam (Primary/Rescue) to `deriveDiffTints`; confirm the shipped theme's anchors succeed via the Primary path without rescue | `src/Theme.cpp`, `include/ssg/Theme.h` if the seam needs a declared type | Truecolor hue-fidelity oracle passes for the shipped theme; Indexed256 readable+background-distinct oracle passes for the shipped theme (no hue requirement); new test asserts the shipped theme's path is `Primary`; Truecolor inter-kind distinctness oracle passes | I22 |
| 4 | Replace `fixedFallback`'s hardcoded RGB constants with the theme-derived synthetic-hue rescue (Fix 2); update near-monochrome/light-theme fixture test expectations if their taken values change | `src/Theme.cpp`, `tests/test_theme.cpp` | near-monochrome/light-theme fixtures still readable+background-distinct at both depths, now Truecolor-hue-correct, Truecolor inter-kind-distinct; new synthetic non-degenerate fixture (Considerations) passes | I22 |
| 5 | Strengthen or replace the color-authority scanner to close the brace-elision gap (Fix 3) | `tests/test_theme.cpp` | scanner now flags a reintroduced bare-literal palette; no new false positives against the current codebase | I22 |
| 6 | Regenerate any golden/fixture files whose rendered output embeds the (now-corrected) diff-tint hex values | `tests/fixtures/tui/*.txt` (`SSG_REGEN_GOLDEN=1`), any other fixture embedding theme/diff_tints hex values (search beyond `tests/fixtures/tui/`) | dual-gate green | snapshot/delta symmetry |
| 7 | Manual/automated end-to-end visual confirmation against a real git repo (mirroring the verification already done for the color-depth-defaults fix), PRIORITIZING Truecolor (the default) | - | real captured terminal bytes show green/red/orange family SGR codes, not blue/black, at Truecolor by default; forced-256 shows readable, background-distinct (not necessarily mutually distinct or correctly hued) tints | - |

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
