# spec-kitty-keyboard

Status: DRAFT (spec only — do NOT implement; collaborate on the open questions
and the Tier-A/Tier-B scope decision first).

## Goals

Adopt the Kitty keyboard protocol as a **detected, capability-gated opt-in** so
that, on a terminal that supports it, SSG decodes modified key events from an
exact modifier bitmask instead of inferring modifiers from legacy byte tricks.
The user-visible outcome:

- The `Alt+Shift+<letter>` bindings (palette `Alt+Shift+P`, redo `Alt+Shift+Z`,
  settings `Alt+Shift+T`) and the `Alt+<letter>` / `Ctrl+<letter>` bindings
  become **caps-lock-immune and unambiguous**: shift is read from the protocol's
  modifier bit, not derived from letter case, so caps-lock no longer swaps
  `Alt+P` and `Alt+Shift+P`. (This is the concrete backlog bug this spec fixes.)
- On a terminal without the protocol, behaviour is exactly as today (legacy
  decoding), with no regression — the protocol is never enabled unless the
  terminal answered the capability query.

Explicit non-goal for this spec (Tier B, deferred): a true at-rest **CAPS LED**
footer indicator. The minimal protocol flag this spec enables (disambiguate,
`CSI > 1 u`) reports caps-lock only as a modifier bit **on a key event**, never
at rest, and only surfaces on modified keys. A genuinely always-current CAPS
indicator needs the invasive flags (report-all-keys + associated-text) that
reroute the plain-text input hot path; that is called out below but not designed
here.

## Design

### The one property that keeps legacy and kitty from spreading

A Kitty key event is **self-identifying on the wire**: it is `CSI <number> [;
<mods> [; …]] u` with **no** `?` private-prefix. A legacy terminal never sends a
non-private `CSI … u` as *input* (`CSI u` is an output cursor-restore, never
keyboard input), and the only `?`-prefixed `CSI … u` SSG ever sees is the
capability *reply*, already handled by the reply path. Therefore:

- **The decoder stays stateless with respect to protocol.** `decode_input` does
  NOT branch on "are we in kitty mode?" anywhere. It dispatches on the bytes it
  sees. Kitty support is exactly **one new byte-triggered case** (`case 'u'` for
  a non-private CSI) beside the existing legacy cases (`A/B/C/D/H/F`, `~`, `<`,
  `M`, C0, ESC-coalescing). Both encodings coexist as sibling cases in one
  function, never as a flag checked in many places.
- **Enabling the protocol only changes what the terminal SENDS**, not how the
  decoder decides what a byte means. The single stateful decision — whether to
  write the enable sequence so the terminal starts speaking Kitty — lives in one
  place (the terminal-mode setup), gated by one capability check.

This is the mechanism chosen specifically to honour the constraint that legacy
and Kitty branches must not be scattered across classes: there is **one decode
seam** (a dedicated `decodeKittyKey` helper invoked from that single case) and
**one enable seam**, and nothing else in the codebase learns the protocol
exists.

### Ownership

Entirely app-side (terminal I/O is the app's, per the library/app split). The
library's `KeyStroke` is the protocol-neutral contract both encodings already
produce; **no library change is required for Tier A**. The keymap, dispatch, and
every downstream consumer are untouched — they receive the same `KeyStroke`
whether it came from a legacy byte or a Kitty escape.

### Decode seam (one helper, complete for every bound key)

Add `case 'u':` (non-private CSI) in `decode_input`'s `switch(third)`, delegating
to a new `decodeKittyKey(bytes, …)` that owns the whole Kitty key grammar and
returns the neutral `Decoded`/`KeyStroke`. Because enabling the protocol reroutes
**all modified keys and the disambiguated named keys** (notably `Esc`, and
`Ctrl+<letter>`) from their legacy form to `CSI … u`, this helper must be a
**complete** decoder for every key SSG binds or reads — not a partial add-on:

- Base key: the Kitty "unicode-key-code" (the shifted-independent layout key,
  e.g. `112` = `p` regardless of caps-lock) mapped to `KeyCode`. Functional keys
  (Esc `27`, Enter `13`, Tab `9`, Backspace `127`, arrows/Home/End via their
  Kitty codes) map to the existing named `KeyCode`s.
- Modifiers: the `mods` field is `1 + bitmask`; the bitmask bits are
  `shift=1, alt=2, ctrl=4, super=8, hyper=16, meta=32, caps_lock=64,
  num_lock=128`. This is the same `1 + bitmask` shape the legacy CSI cases
  already decode, just with more bits — see the shared helper below.
- Associated text / alternate-key sub-parameters (`:`-separated) are parsed only
  enough to be skipped safely; Tier A does not request them.

### Shared modifier helper (modifier semantics live in one spot)

Extract the `modifier - 1` → `shift/alt/ctrl` bit decoding that the legacy
`case '1'` (arrows/Home/End) and `case '3'|'5'|'6'` (Delete/PageUp/PageDown)
cases duplicate today into one `applyModifierBitmask(KeyStroke&, bitmask,
Extended)` and have `decodeKittyKey` use it too.

**Legacy and Kitty must NOT get identical bit acceptance** (review SHOULD): the
legacy CSI cases today *ignore* any bit outside `shift/alt/ctrl` (e.g. `m=9`
falls back to the plain key). Reusing an "extended" helper naively would silently
change that. So the helper takes an explicit mode: legacy calls mask/reject the
high bits (unchanged behaviour), Kitty calls accept the extended bits
(`super`/`meta` → `meta` per the decision below; `caps_lock`/`num_lock`
out-of-band). The property test encodes this distinction — same low bits both
ways, high bits accepted only for Kitty.

### super/meta/hyper mapping — DECIDED (Tier A)

`KeyStroke` exposes `meta`, which a custom keymap can bind. Tier A: the Kitty
`meta` modifier bit maps to `KeyStroke.meta`; the Kitty `super` and `hyper` bits
are **ignored** in Tier A (no default binding uses them, and collapsing three
distinct physical modifiers onto one `meta` would make them indistinguishable
and unbindable-apart). A later increment may introduce explicit `super`/`hyper`
handling; Tier A neither sets nor depends on them. Tests assert `super`/`hyper`
bits do not alter the resulting `KeyStroke`.

### Neutral-KeyStroke boundary (do not poison keymap matching)

`KeyStroke` participates in keymap matching by value (`operator==` over
`code/control/alt/meta/shift`); a binding is parsed from text into exactly those
fields. Therefore:

- Only the **binding-participating** modifiers set `KeyStroke`:
  `shift`→`shift`, `alt`→`alt`, `ctrl`→`control`, and `super`/`meta`→`meta`
  (single canonical mapping — see open question).
- `caps_lock` and `num_lock` are **never** written into `KeyStroke` (a binding
  can't express them; a stroke carrying one would fail to match every binding).
  If a later CAPS indicator needs them, they are reported **out-of-band** on
  `Decoded` (a separate field the keymap never sees), preserving match
  semantics.

### Enable seam (one late, gated mode)

Kitty enable/disable is a terminal MODE, so it belongs in the existing
`TerminalMode`/`TerminalModes` machinery that already pairs enter+leave bytes,
tears down in reverse order, and publishes crash-undo bytes for the signal
handler. Add:

```
kKeyboardProtocol{ enter: "\x1b[>1u", leave: "\x1b[<u" }   // push flag 1 / pop
```

The wrinkle that forces this to be a **separate, later** enable (not just another
entry in the unconditional constructor list): the app enters all current modes in
the `TerminalMode` constructor, but the capability probe (`beginProbe`) is
written *after* construction and its reply lands **asynchronously during the main
loop**. So the enable cannot be one of the always-on constructor modes — the
answer isn't known yet. Design: the `TerminalMode` wrapper exposes a single
`enableKeyboardProtocol()` that enters `kKeyboardProtocol` through the same
`modes_`/`entered_` machinery (so teardown + crash-undo cover it automatically),
and the loop calls it **once**, latched, the first time
`capabilities.has(KeyboardProtocol)` is true (checked the moment the capability
reply is observed). Entered last ⇒ popped first on teardown, before the alternate
screen leaves — correct order. There is no re-probe after startup, so the
capability is resolved once and the enable never needs to be undone-then-redone.

**Push/pop teardown safety (as built).** The existing crash-undo
(`all_modes_undo_sequence`, ssg_terminal.h) is a **constant compile-time
superset** of every `kAllModes` leave, safe only because *leaving a mode that was
never entered is harmless*. Kitty's leave `CSI < u` is a **stack pop, not
idempotent**, so `kKeyboardProtocol` must **NOT** join `kAllModes`. Teardown is
therefore matched-push-only via its **Guard** alone: the Guard's destructor pops
exactly once on every real teardown path SSG has — normal return, the exception
catch, and the SIGTERM/SIGHUP handler (which drains its self-pipe tag and calls
`mode.restore()` in normal context, dropping the guards). SSG installs no
async-signal crash handler that writes terminal-undo bytes (the constant undo
sequence is defined and unit-tested but not wired to a fatal handler), so a hard
crash (e.g. SIGSEGV) leaves Kitty enabled exactly as it already leaves the
alternate screen and mouse reporting on — no worse than the status quo, and not
in scope to fix here. Do not hand-write the enable/leave bytes anywhere except
the `kKeyboardProtocol` mode definition.

## Invariants

- **INV-key-encoding-one-seam (new, the point of this spec):** every
  protocol-specific key-decoding decision lives in `decode_input`'s byte-
  triggered cases — the legacy cases, the single Kitty `case 'u'`/`decodeKittyKey`
  helper, and the shared `applyModifierBitmask` helper. No other translation unit
  or class turns bytes into a `KeyStroke` or branches "legacy vs kitty". The
  reviewer enforces this against an **explicit whitelist** of the only places
  allowed to reference the protocol at all, so the source-scan is precise rather
  than a vague grep:
  - capability detection: `TerminalCapabilities` (`beginProbe`/`observeReply`);
  - the setting gate: `keyboard.protocol` in Settings + its one read site;
  - the enable/leave: the `kKeyboardProtocol` `TerminalMode` and the single
    `enableKeyboardProtocol()` call site;
  - the decode: `decode_input`'s `case 'u'`, `decodeKittyKey`, and
    `applyModifierBitmask`.
  Any byte-to-`KeyStroke` Kitty/legacy branching outside this list is a
  violation. (Enable/config/capability seams are expected and whitelisted; the
  forbidden thing is decode/semantic duplication elsewhere.)
- **INV-keystroke-binding-modifiers (new):** only `shift/alt/ctrl/meta`
  participate in `KeyStroke` and keymap matching; `caps_lock`/`num_lock` are
  out-of-band and never enter `KeyStroke`.
- **INV-reply-never-input (existing):** the Kitty case consumes a sequence whole
  or defers as incomplete via the shared `scanCsi`; it never guesses a length.
- **INV-decode-terminates (existing):** the Kitty scan is bounded like every
  other CSI scan; a truncated `CSI … u` cannot hold typed input hostage.
- **INV-capability-single-source (existing):** the enable is gated solely by
  `TerminalCapabilities::has(KeyboardProtocol)`; nothing else decides.

## Considerations

- **Enabling reroutes core keys.** With flag 1, `Esc` arrives as `CSI 27 u` and
  `Ctrl/Alt/Alt+Shift + <letter>` arrive as `CSI … u` instead of their legacy
  bytes. The freed-Escape (prompt cancel) and every Ctrl/Alt binding therefore
  depend on `decodeKittyKey` being complete. This is the main risk (below) and
  why the Kitty case is a full decoder, not an additive shim. Unmodified plain
  text still arrives as UTF-8 and flows through the existing text path (flag 1
  does not touch it).
- **Legacy path stays as the fallback.** The legacy cases are unchanged and
  remain the decoder for terminals that never answered the query. Both coexist;
  neither is removed.
- **Re-probe on reattach.** Not applicable in Tier A: SSG probes once at startup
  and does not re-probe on resize/reattach, so the capability does not flip and no
  symmetric leave is needed. (If a future change adds re-probing, revisit.)
- **Multiplexers / browser hosts.** tmux/screen and older xterm.js may swallow
  the query ⇒ `keyboard_protocol no` ⇒ never enabled ⇒ safe legacy fallback.
  xterm.js ≥ 6.1.0 supports it (per spec-mod-keys.md). Validate against: kitty,
  foot, WezTerm, ghostty, Alacritty, iTerm2, Konsole, and xterm.js.
- **Escape hatch.** Capability gating is the primary escape hatch (the protocol
  is enabled only on a terminal that answered the query). The existing
  `SSG_TERM_KEYBOARD_PROTOCOL=off` environment override forces the legacy path if
  a terminal advertises but mis-implements the protocol — no new setting needed
  in Tier A.
- **Tier B (deferred, not designed here):** a true CAPS indicator and fully
  uniform input need flags 8 (report-all-keys-as-escape-codes) + 16
  (report-associated-text). That makes `decodeKittyKey` the PRIMARY path for
  ALL keys including plain text, moving text reconstruction through it — a much
  larger, higher-risk change to the hot path, plus a library-side footer field
  and a way for the app to publish caps state to the library. Out of scope.

## Risks and Mitigations

- **Incomplete Kitty decoder breaks core keys once enabled** (Esc, Ctrl+C,
  Alt+*). Mitigation: the Kitty case is specified as a COMPLETE decoder for every
  bound/read key, gated behind the capability, with oracles covering Esc, Ctrl,
  Alt, Alt+Shift, and the named/nav keys; ship the enable behind the `off`-capable
  setting and take visual signoff on a real kitty terminal before defaulting
  `auto` on.
- **caps_lock leaking into `KeyStroke`** silently breaks all matching. Mitigation:
  INV-keystroke-binding-modifiers + an oracle asserting a stroke with caps set
  `==` the same stroke without it.
- **Stuck mode after crash / mis-implementing terminal.** `CSI < u` is a stack
  pop, not idempotent, so it is matched-push-only via the Guard and is kept OUT of
  the constant `kAllModes` crash-undo superset. SSG wires no fatal-signal handler
  that writes terminal-undo bytes, so a hard crash leaves Kitty enabled exactly as
  it leaves every other mode on — consistent, not a new regression.
  `SSG_TERM_KEYBOARD_PROTOCOL=off` handles a terminal that advertises but
  mis-implements the protocol.
- **Scope creep into Tier B.** Mitigation: flag set fixed at `1` for this spec;
  8/16 explicitly deferred.

## Acceptance (Definition of Done)

- Observable (needs signoff — behavioural): on a kitty-protocol terminal, toggle
  caps-lock ON and confirm `Alt+Shift+P` still opens the palette and `Alt+P`
  still opens the file finder (they no longer swap); confirm `Esc` still cancels a
  prompt and `Ctrl`/`Alt` bindings still fire. On a legacy terminal, confirm
  identical behaviour to today (protocol never enabled).
- Budgets: no measurable input-latency change; the decode path stays allocation-
  free on the hot path as today.
- Gates: `bash scripts/check.sh` green (all suites, 0 warnings), tree-sitter ON
  and OFF.
- Oracles (decode_input is a pure function — each is a hand-computed reference vs
  real output; write before its code):
  - **Default-keymap parity (the load-bearing oracle, review MUST):** for EVERY
    binding in the default terminal keymap, feed the Kitty encoding of that
    `KeyStroke` and assert `decodeKittyKey` returns the identical `KeyStroke` the
    binding parses to — driven off the keymap/`all_command_ids` list so a new
    binding cannot be added without parity coverage. This MUST include the
    non-letter modified bindings flag 1 reroutes: `Alt+Backspace` (delete word),
    `Alt+Slash`, `Alt+Digit8`, `Alt+Period`/`Alt+Comma` (tab next/prev), and any
    shifted-punctuation/digit binding — a partial decoder must fail here even if
    the hand-picked letter cases below pass.
  - `CSI 112 ; <alt+shift> u` → `KeyStroke{code=KeyP, alt, shift}` — and the SAME
    key with the caps-lock bit ALSO set yields the SAME binding stroke (proves the
    caps-lock bug fixed and caps out-of-band).
  - `CSI 99 ; <ctrl> u` → `KeyStroke{code=KeyC, control}`.
  - `CSI 27 u` → `KeyCode::Escape` (freed-escape survives the reroute).
  - `applyModifierBitmask` property test over all bitmask values, asserting the
    legacy mode IGNORES bits outside `shift/alt/ctrl` (unchanged legacy
    behaviour) while the Kitty mode accepts `meta` and ignores `super`/`hyper`,
    with caps/num never touching `KeyStroke`.
  - Self-identifying safety: `CSI ? <n> u` still classifies as a capability reply;
    non-private `CSI <n> u` classifies as a key (never confused).
  - **Malformed / subparameter safety (review SHOULD):** colon-separated
    sub-parameters (associated text, alternate keys), empty sub-params, overlong
    numeric fields, and a truncated/garbage `CSI … u` are each consumed or
    deferred as ONE CSI via the shared `scanCsi`, never partially dispatching a
    key or holding typed input hostage (INV-decode-terminates / INV-reply-never-
    input).
  - Enable/teardown: entering `kKeyboardProtocol` through `TerminalModes` writes
    the flag-1 push and the Guard's destruction writes the matching pop; the
    non-idempotent pop is absent from the constant `all_modes_undo_sequence`
    superset.
  - INV-key-encoding-one-seam: a source scan finds no byte-to-`KeyStroke`
    protocol branching outside the whitelisted sites (mirrors the existing
    `noDcsOrOscQuery…` source-scan test pattern).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Extract `applyModifierBitmask(KeyStroke&, bitmask, mode)`; rewrite the legacy `case '1'` and `case '3'/'5'/'6'` to use it in LEGACY mode (masking high bits — behaviour-preserving) | apps/ssg_terminal.cpp, tests/test_ssg_app.cpp | property: legacy mode reproduces current modifier decode and ignores bits outside shift/alt/ctrl across all bitmasks | INV-key-encoding-one-seam |
| 2 | Add `decodeKittyKey` + the single `case 'u'` (non-private); complete for every bound/read key; `meta`→meta, `super`/`hyper` ignored, caps/num out-of-band on `Decoded`; subparams skipped as one CSI | apps/ssg_terminal.cpp, apps/ssg_terminal.h, tests/test_ssg_app.cpp | default-keymap parity oracle (incl. Alt+Backspace/Slash/Digit8/Period/Comma) + Esc/Ctrl/Alt+Shift + caps-out-of-band + malformed-subparam + self-identifying oracles | INV-key-encoding-one-seam, INV-keystroke-binding-modifiers, INV-reply-never-input, INV-decode-terminates |
| 3 | Add `kKeyboardProtocol` mode (NOT in `kAllModes`) + latched `enableKeyboardProtocol()` on the app `TerminalMode` wrapper (Guard-based teardown on every path); call it from the loop the moment the capability reply is observed | apps/ssg_terminal.h, apps/ssg_main.cpp, tests/test_ssg_app.cpp | Guard round-trips push/pop; the non-idempotent pop is absent from the constant crash-undo superset | INV-capability-single-source |
| 4 | Doc updates: config.md note on the auto-enabled protocol + `SSG_TERM_KEYBOARD_PROTOCOL=off`; annotate spec-mod-keys.md kitty section as implemented (Tier A) | doc/config.md, doc/spec-mod-keys.md | n/a (docs) | - |
| 4 | Doc updates: config.md note on the auto-enabled protocol; annotate spec-mod-keys.md kitty section as now implemented (Tier A) | doc/config.md, doc/spec-mod-keys.md | n/a (docs) | - |

## Rationale (skippable)

The capability is already detected today (`beginProbe` sends `CSI ? u`,
`observeReply` sets `Capability::KeyboardProtocol`), and both encodings already
converge on one neutral `KeyStroke`, so the work is genuinely small and contained
— *provided* the design refuses the tempting-but-wrong shape of a global "kitty
mode" boolean sprinkled through the decoder and app loop. The self-identifying
nature of `CSI … u` input is what lets us avoid that: the decoder dispatches on
bytes, so legacy and kitty are sibling cases in one function, and the only
stateful decision (write the enable sequence) is one gated line. The one real
subtlety is timing — the probe answer arrives after modes are entered — which is
why the enable is a single late, latched, capability-gated call rather than a
constructor mode. Fixing the caps-lock binding ambiguity is the concrete payoff;
the true CAPS LED is honestly out of reach at flag 1 and is left to a Tier-B
increment that pays the hot-path cost of report-all-keys.
