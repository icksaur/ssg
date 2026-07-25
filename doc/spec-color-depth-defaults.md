# spec-color-depth-defaults

Status: DRAFT. Two related fixes: (1) terminal color-depth DETECTION defaults
too conservatively, causing genuinely truecolor-capable sessions (most commonly
over SSH, where `COLORTERM` is not forwarded by default) to render at a lower
depth than the terminal actually supports; (2) the theme's diff-tint
DISTINCTNESS gate is only verified at Truecolor, so a theme's derived diff
colors can — and, for the shipped default theme, do — collapse to
indistinguishable near-identical grays once quantized to Indexed256, which is
exactly the depth affected by (1). Terminal-client only; the browser client
renders full RGB directly and is unaffected by either fix.

## Goals

After this change:
- An interactive terminal session is assumed truecolor-capable by default
  unless there is a specific negative signal, rather than defaulting to the
  least-capable tier (`Ansi16`) whenever `COLORTERM` is absent — which is the
  common case over SSH regardless of the local terminal's real capability,
  since SSH does not forward `COLORTERM` by default.
- A user or operator can force a specific depth via an explicit override,
  correcting the rare case where the improved heuristic still gets it wrong in
  either direction (a genuinely limited terminal, or a desire to test a lower
  depth deliberately).
- When Indexed256 IS the correct, real depth (override, or a confirmed
  256-color-only session), diff tints remain visually distinct from each other
  and from the background after quantization — not just readable in the
  Truecolor math that produced them. Today's shipped default theme fails this:
  its derived diff-row tints collapse to nearly the same gray at Indexed256.

## Design

### Part A — Detection defaults toward truecolor

Current behavior (`apps/ssg_terminal.cpp:39-51`, `detect_color_depth`):
`COLORTERM` == `truecolor`/`24bit` -> Truecolor; else `TERM` contains
`256color` -> Indexed256; else -> Ansi16 (the least-capable tier, chosen for
every case not positively proven otherwise).

The practical failure mode: SSH does not forward `COLORTERM` unless both the
client's `SendEnv` and the server's `AcceptEnv` explicitly whitelist it, which
neither does by default (only `LANG`/`LC_*` typically are) — so a fully
truecolor-capable local terminal (Kitty, Windows Terminal, iTerm2, etc.)
SSH'd into a remote host arrives with `COLORTERM` unset, `TERM=xterm-256color`
at best, and is detected as Indexed256 even though the terminal is truecolor
capable end to end. This is not a rare edge case — SSH is one of the most
common ways developers actually use a terminal.

New precedence, highest to lowest:
1. **Explicit override** (new): an `SSG_COLOR_DEPTH` environment variable,
   values `truecolor`/`24bit`, `256`/`256color`/`indexed256`, `16`/`ansi16`
   (case-insensitive). Set, valid -> use it, skip all heuristics below. Set,
   unrecognized value -> fall through to heuristics (never a hard error; a
   typo must degrade to the existing safe behavior, not crash or misbehave).
   This is the direct, zero-risk fix for exactly this user's situation
   (`SSG_COLOR_DEPTH=truecolor` over an SSH session where `COLORTERM` is lost)
   and the general escape hatch for any future misdetection in either
   direction.
2. **`COLORTERM`** == `truecolor`/`24bit` -> Truecolor (unchanged; the existing
   definitive positive signal — this is a direct assertion by the terminal
   itself and outranks even `TERM=dumb`/unset below, since a terminal that
   explicitly sets `COLORTERM=truecolor` alongside an otherwise-minimal `TERM`
   is still telling the truth about its color capability).
3. **`TERM` == `dumb`** -> Ansi16 (unchanged; an explicit, well-established
   low-capability signal used by non-interactive/piped/minimal contexts — a
   HARD FLOOR that nothing below this tier may override, including the
   allowlist in the next tier: a heuristic `TERM_PROGRAM`/`TERM` STRING match
   is weaker evidence than an explicit `TERM=dumb` assertion, so `dumb` must be
   checked, and win, before the allowlist is even consulted).
4. **`TERM` unset or empty** -> Ansi16 (unchanged; conservative default for a
   context that may not be a real interactive terminal at all — likewise a
   hard floor checked before the allowlist).
5. **Known-truecolor terminal identifiers** (new): `TERM_PROGRAM` or `TERM`
   matching a short allowlist of terminals that are ALWAYS truecolor
   regardless of `COLORTERM` forwarding — the same technique widely used by
   other color-detection libraries (e.g. the `supports-color`/
   `has-truecolor`-style checks common in other ecosystems) to compensate for
   exactly this `COLORTERM`-not-forwarded gap. Starting allowlist (extend
   without a spec amendment as new terminals are confirmed; this is a fact
   list, not an invariant): `TERM_PROGRAM` in {`iTerm.app`, `WezTerm`,
   `vscode`, `Hyper`, `ghostty`}; `TERM` containing {`kitty`, `alacritty`,
   `wezterm`, `foot`, `contour`, `ghostty`}. `TERM=xterm-kitty` (Kitty's actual
   default `TERM`) is covered by this.
6. **Otherwise** (new default): **Truecolor** — this is the actual behavior
   change. Any other `TERM` value (including `xterm`, `xterm-256color`,
   `screen`, `screen-256color`, `tmux-256color`, `vt100`, `linux`, etc.) is now
   assumed truecolor-capable by default, since virtually every actively
   maintained terminal emulator supports it and the prior conservative default
   was systematically wrong for the common SSH case. `TERM` containing
   `256color` no longer caps the result at Indexed256 — it is one MORE
   signal that the session is at least reasonably modern, but no longer a
   ceiling.

### Part B — Diff-tint distinctness verified post-quantization

`Theme.cpp`'s `readable()` (`~188-213`) already loops over BOTH
`ColorDepth::Truecolor` and `ColorDepth::Indexed256`, resolving each color
before checking contrast — readability is already depth-aware. `distinct()`
(`~229-257`) does not: it resolves only at Truecolor, with an explicit comment
accepting that Indexed256 quantization "can collapse subtle-but-readable
washes to the same swatch" and asserting "a diff row still reads apart
structurally at 256" (i.e. the row's own foreground/background contrast still
differs from a neighboring plain row, even if the wash itself carries no
visible hue). Empirically (this bug), that assumption does not hold in
practice for the shipped default theme: `modifiedRow`/`addedRow` derived
tints both resolve to visually-identical near-black grays at Indexed256 (xterm
256-color has coarse ~40-unit steps in its 6×6×6 cube but fine ~10-unit steps
in its gray ramp, so nearest-neighbor search on a moderately-saturated dark
color frequently prefers a gray swatch over any cube color — this is a
property of the palette's geometry, not a one-off rounding fluke, and will
recur for most themes with a dark, low-saturation-anchor palette).

Fix: extend `distinct()` to loop over the SAME two depths `readable()` already
does, applying its existing ΔE-based pairwise checks (`kKindDeltaE`,
`kWordDeltaE`, `kRowDeltaE`) to each depth's RESOLVED (quantized) RGB, not just
the pre-quantization theme color. A theme's derived tints must be
`distinct()` at Truecolor AND at Indexed256 to be accepted; if either fails,
the EXISTING fallback-selection chain (`deriveDiffTints`'s
derived -> preferredFallback -> alternateFallback -> derived-as-last-resort,
`~301-316`) already does the right thing with no new mechanism required — it
falls through to `fixedFallback()`'s bolder, cube-aligned colors (e.g.
`{0,0,95}`, `{0,135,0}`), which are chosen close enough to real cube steps that
they are expected to survive Indexed256 quantization distinctly. This is a
minimal, structurally consistent change (mirroring an existing pattern,
`readable()`'s two-depth loop) rather than inventing new interpolation
constants — no ad hoc weight/saturation tuning is needed; making the
ALREADY-EXISTING selection chain correctly detect the failure it was designed
to catch is sufficient.

The per-depth ΔE thresholds MAY need to differ (Indexed256's coarser
palette makes the current thresholds, tuned implicitly against Truecolor's
continuous space, potentially infeasible for many real themes to satisfy at
that depth — this must be validated empirically against the shipped theme and
existing fixture themes during implementation, and a separate, appropriately
looser Indexed256 threshold constant introduced if the Truecolor thresholds
prove too strict to be satisfiable there). This is a mechanism detail to
confirm during implementation, not a design uncertainty — the PROPERTY (verify
distinctness at whatever depth is actually resolved) is the fixed contract.

### Non-goals

- Active terminal capability querying (DECRQSS/XTGETTCAP or similar) is
  explicitly out of scope — it introduces asynchronous I/O with timeout/
  hang risk against a non-responding terminal, a materially bigger feature
  than this fix. `SSG_COLOR_DEPTH` is the escape hatch for cases the
  passive heuristic still gets wrong.
- The browser client is unaffected by either part — it already renders the
  16-color palette and derived tints as full RGB with no quantization step;
  Part B's fix touches `Theme.cpp`, which the browser reads the SAME
  `ThemeSnapshot` from, so the (now stricter) tint derivation benefits it too,
  but nothing browser-specific needs to change.
- No change to the readability gate itself (`kFloorContrast`/
  `kRetainContrast`) — this bug is about distinctness, not readability; the
  two are already independently and correctly gated for readability.
  (Historical note: `deriveSelectionFill`'s readability gate described here
  was later REMOVED — see commit 5319ab5, "Make selectionFill a flat
  Selection-role color, matching diff tints" — in favor of a flat anchor
  color matching `deriveDiffTints`'s model. This spec's own distinctness
  gate, which is about `resolveColor`/depth quantization rather than
  `deriveSelectionFill`, is unaffected by that later change.)

## Invariants

- Themes are the sole source of color (spec.md I22 / `color.h`'s file
  comment) — the override and allowlist in Part A affect DEPTH SELECTION
  (a client/mechanism concern), never mint or substitute a color; `Theme.cpp`
  remains the only place a color value is chosen.
- `resolveColor` remains a pure function of `(color, depth)` — Part B calls it
  more (once per depth in `distinct()`, mirroring `readable()`), it does not
  change its contract or signature.
- Feature-not-mechanism (I25) — `SSG_COLOR_DEPTH` and the detection heuristic
  are CLIENT/mechanism concerns (how a specific terminal process is invoked),
  not a library feature; both live in `apps/ssg_terminal.cpp`/`ssg_main.cpp`,
  never in the library. `Theme.cpp`'s distinctness fix is a library concern
  (color correctness is a library invariant per I22) and belongs there.

## Considerations

- The allowlist (Part A, item 5) is intentionally small and named as a FACT
  list (not exhaustive, extendable without a spec amendment) — the risk of a
  false positive (assuming truecolor for a terminal that doesn't support it)
  is bounded by graceful degradation, VERIFIED (not merely assumed) per Plan
  step 3b below: confirm what modern terminals actually do when they receive
  a truecolor SGR code they don't support (expected: either the terminal
  itself downsamples the requested RGB to its own nearest supported color, or
  it ignores the sequence and keeps the prior/default color — both are a
  readable, if imperfect, degrade, not corruption/garbage output). If a
  genuine garbling risk is found for some real terminal during review, narrow the
  allowlist or make the "otherwise -> Truecolor" default (item 6) more
  conservative instead.
- Extending `distinct()` to Indexed256 must not cause the FALLBACK colors
  themselves to also fail the new check (an exhausted selection chain
  collapsing to the "derived-as-last-resort" case defeats the fix) — verify
  `fixedFallback()`'s dark/light sets pass the new two-depth `distinct()`
  before relying on them as the safety net.
- `distinct()`'s existing near-monochrome-anchor rescue test
  (`nearMonochromeAnchorsUseAReadableDistinctFallback`) and light-theme
  fallback test must continue to pass; if the new Indexed256 check changes
  which path (derived vs. fallback) those fixtures take, the tests' assertions
  may need updating to match the (still correct, still readable+distinct)
  new outcome — this is acceptable as long as the PROPERTY (readable, distinct
  at both depths) still holds, not a regression to paper over.
- tmux/screen multiplexers historically strip or require explicit
  configuration to pass through truecolor codes. This spec does not add
  multiplexer-specific detection (no existing precedent in this codebase to
  extend, and it is its own can of worms — recent tmux/screen have improved
  pass-through significantly). `SSG_COLOR_DEPTH` is the correct escape hatch
  for a user running inside an under-configured older multiplexer.

## Risks and Mitigations

- Defaulting to Truecolor for previously-Ansi16-or-Indexed256 cases could
  regress a genuinely limited terminal (rare, but real: minimal containers,
  very old emulators, some CI harnesses that set a generic `TERM`) ⇒
  mitigated by `SSG_COLOR_DEPTH` (users/scripts in known-constrained
  environments set it explicitly) and by the graceful-degradation property
  investigated above.
- Indexed256-aware `distinct()` could force MOST or ALL real themes into the
  fixed fallback set, making the "derived, theme-hued" path effectively dead
  code ⇒ validate empirically against the shipped default theme and existing
  fixture themes during implementation; if the Truecolor-tuned ΔE constants
  are simply too strict for Indexed256's coarser palette, introduce a
  separate, looser Indexed256-specific threshold rather than let every theme
  fall back.
- A typo'd `SSG_COLOR_DEPTH` value must never crash or produce undefined
  behavior ⇒ unrecognized values fall through to the existing heuristic chain
  (stated explicitly in Design, Part A item 1).

## Acceptance (Definition of Done)

- Observable: `TERM=xterm-256color COLORTERM=` (this bug's exact reproduction)
  now resolves to Truecolor by default; `SSG_COLOR_DEPTH=256` on the same
  environment forces Indexed256 and the diff tints are visibly distinct
  (not identical grays) in that forced mode; a real git diff opened in the TUI
  shows clearly colored (not grey) added/removed/modified rows in both the
  new default (Truecolor) and the forced-256 path. Visual signoff required
  (user-visible rendering fix).
- Gates: `bash scripts/check.sh` green.
- Oracles:
  - detection: hand-case table covering every precedence tier in Design Part A
    (override valid/invalid, `COLORTERM` positive, allowlist `TERM_PROGRAM`
    and `TERM` matches, `dumb`, unset, and the new "otherwise" default) —
    extending the existing `detect_color_depth` test table
    (`tests/test_ssg_app.cpp:126-136`), not replacing its still-valid cases.
  - distinctness: a property test that `deriveDiffTints`'s output, for the
    shipped default theme AND the existing near-monochrome/light-theme
    fixtures, is `distinct()` at BOTH Truecolor and Indexed256 — this is the
    oracle that would have caught this bug; write it to FAIL against current
    code first (red before green), per this project's convention.
  - regression: `diffTintsMeetResolvedReadabilityAndDistinctnessGates` and the
    two fallback-path tests continue to pass (possibly with updated
    assertions reflecting a fallback-path outcome where the property still
    holds, per Considerations).
  - degradation safety (NEW, required, not a hand-wave): before shipping the
    "otherwise -> Truecolor" default flip, concretely verify (documented in
    the implementation's PR/commit, checked in review, not merely asserted in
    this spec) that at least one terminal representative of each depth class
    this default could now mis-target — a 256-color-only terminal (e.g. a
    minimal `xterm-256color` build or documented behavior thereof) and a
    16-color-only terminal — degrades gracefully (downsamples or ignores
    unsupported truecolor SGR) rather than corrupting output. If no real
    terminal exhibiting genuine incapability is available to test directly,
    this MUST be confirmed from the terminal's own documented behavior/
    changelog (not assumed), and the finding recorded in the Plan step 3
    commit message.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Red-before-green: add the Indexed256 `distinct()` property test against the shipped theme (must fail against current code) | `tests/test_theme.cpp` | fails pre-fix | - |
| 2 | Extend `distinct()` to loop over Truecolor + Indexed256 (mirror `readable()`'s existing pattern); tune/introduce a separate Indexed256 ΔE threshold if the Truecolor constants prove too strict empirically; verify `fixedFallback()`'s sets ALSO pass the new two-depth check (a MUST precondition, not optional — if the fallback itself fails, the selection chain can still exhaust to the original bug) | `src/Theme.cpp`, `tests/test_theme.cpp` | step 1's oracle now passes; existing near-monochrome/light-theme fallback tests still pass (updated if the taken path legitimately changes); new explicit assertion that `fixedFallback()`'s dark AND light sets independently pass `distinct()` at both depths | I22, color-derivation invariants above |
| 3 | Add `SSG_COLOR_DEPTH` override + `TERM_PROGRAM`/allowlist signals + reordered precedence (override > `COLORTERM` > `dumb` > unset > allowlist > otherwise-Truecolor) in `detect_color_depth`; perform and record the degradation-safety verification above | `apps/ssg_terminal.cpp`, `apps/ssg_terminal.h` if the signature needs `TERM_PROGRAM`/env accessors passed in | extended hand-case table in `tests/test_ssg_app.cpp`, including this bug's exact repro case AND a case proving `TERM=dumb`/unset are never upgraded by an allowlist match | I25 (mechanism stays client-side) |
| 4 | Wire `SSG_COLOR_DEPTH`/`TERM_PROGRAM` reads at the one call site in `apps/ssg_main.cpp` (currently reads only `COLORTERM`/`TERM`) | `apps/ssg_main.cpp` | manual smoke test against `/tmp/gittest` in the real reported environment (`TERM=xterm-256color`, `COLORTERM=` empty) — diff tints visibly colored | I25 |
| 5 | Regenerate any golden/fixture files whose rendered output embeds resolved diff-tint hex values, affected by step 2's changed derivation output | `tests/fixtures/tui/*.txt` (regen via `SSG_REGEN_GOLDEN=1`), any protocol/snapshot goldens embedding theme colors — search beyond just `tests/fixtures/tui/` for any other fixture embedding `diff_tints`/theme hex values (protocol round-trip goldens, snapshot delta goldens) | dual-gate green | snapshot/delta symmetry (existing) |

## Rationale (skippable)

Both parts of this bug share a root cause: a heuristic tuned for a "prove it's
capable, else assume the worst" posture, in a world where the "worst case" is
now rare and the failure to prove capability is usually just an environment
artifact (SSH not forwarding `COLORTERM`) rather than genuine incapability. The
detection fix flips that posture for the common case while keeping explicit,
well-established negative signals (`dumb`, unset) as real floors, and adds a
zero-risk escape hatch for whichever direction the heuristic still gets wrong.
The distinctness fix is narrower and more mechanical: `readable()` already knew
to check both depths; `distinct()` simply never got the same treatment, and the
existing fallback-selection machinery was already built to handle exactly this
failure — it just needed the check that triggers it.
