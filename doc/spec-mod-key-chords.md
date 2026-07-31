# spec-mod-key-chords

Status: SUPERSEDED by `doc/spec-remove-leader.md` (implemented).

This spec migrated the multi-stroke chord engine (folding `{Escape, X}` →
`{Alt+X}` while keeping multi-stroke sequences).  The project instead removed the
multi-stroke engine entirely; see `spec-remove-leader.md`.  Its decode-layer
design (ESC+printable → `Alt+key`, C0 → `Ctrl+letter`, the Shift-semantics rules)
was adopted and is retained here as the record of that reasoning.

## Goals

Replace the Escape leader with a **modifier-prefixed** chord model so that:

- A command chord is a single keystroke, `Alt+S` (default), not the two-stroke
  `Escape` then `S`.
- **Escape is freed** to mean only "get out": one press cancels a prompt, closes
  find, or aborts a pending multi-stroke chord. The two-Escape cost documented in
  `backlog.md` disappears.
- The chord modifier is **configurable** (`alt` | `ctrl`), defaulting to `alt`.
- Where the terminal advertises the keyboard protocol, disambiguation is exact;
  everywhere else it degrades to a defined legacy heuristic.

Nothing about the *set* of commands or their ids changes. This is a change to
how a chord is encoded on the wire, decoded into a `KeyStroke`, and expressed in
the default keymap — not to what the commands do.

## Design

### The one fact that drives everything

In a legacy terminal `Alt+X` is transmitted as the two bytes `ESC X` — the exact
bytes the Escape leader emits for `{Escape, X}` today. So an Alt-primary scheme
is **not a new wire format**; it is a reinterpretation of bytes SSG already
receives. `Ctrl+letter`, by contrast, is a single C0 control byte (`0x01`–`0x1a`)
that `decode_input` currently *ignores* (`apps/ssg_terminal.cpp` final `return`
"Other control byte: ignore"), and it collides with `Tab`(`0x09`),
`Enter`(`0x0d`), `Backspace`(`0x08`), and `Escape`(`0x1b`).

### Option comparison

- **A. Alt-primary (chosen default).** Bytes already arrive (`ESC`+key); binding
  migration is mechanical. Browser-capturable — user-confirmed `Alt+D`/`Alt+`
  letters reach xterm.js in Firefox and Edge and are `preventDefault`ed.
  Escape is freed by coalescing (below). Costs: a same-burst/timeout rule to tell
  `Alt+X` from a bare `Escape`, and the macOS Option→special-character caveat.
- **B. Ctrl-primary.** Escape is untouched (C0 ≠ ESC), but: needs new decode of
  C0 → `Ctrl+letter`; loses `Tab`/`Enter`/`Backspace`/`Escape` and all
  `Ctrl+digit`/punctuation to collisions; cannot carry `Shift`
  (`Ctrl+Shift+X` == `Ctrl+X` in legacy); and `Ctrl+T/N/W/Tab` are seized by
  browser chrome before the page — the documented reason the leader was chosen
  (`doc/spec-mod-keys.md`). Unfit as the browser-shared primary.
- **C. Keyboard protocol / CSI-u (kitty).** Where advertised, `Alt`, `Ctrl`, and
  `Escape` are distinct structured events — no heuristic, `Shift` carried, a bare
  `Escape` unambiguous. Not available on WT 1.24 or xterm.js (probe:
  `keyboard_protocol=no`), so it can only *enhance* A, never stand alone.
- **D. Keep the leader, just also bind a bare `Escape` to cancel.** Rejected, and
  the *reason* is load-bearing: a single `Escape` terminal stroke is a prefix of
  every `{Escape, X}` chord, so `KeymapMatcher::validate` rejects it
  (`KeymapErrorCode::UnreachableBinding`, cf. the clipboard note at
  `EditorRuntime.cpp:133-139`). You cannot free Escape while it remains the chord
  prefix — which is exactly why the leader must move off Escape.

**Chosen mechanism: A as the default, configurable to B. C (kitty/CSI-u) is
explicitly OUT OF SCOPE for this spec** — it is named only to show the legacy
heuristic is a fallback, not a dead end; it will get its own spec once the
protocol is captured from a real terminal. The shipped disambiguation path is A's
heuristic alone. A wins because its bytes already exist, it is the option most
reliable in the browser (the binding target that constrains the whole design),
and it frees Escape. B is offered because the user asked for a configurable
modifier, but its limitations are documented, not hidden.

### Decode (`apps/ssg_terminal.cpp` `decode_input`)

Two changes, both to how a leading `ESC` is interpreted; CSI (`ESC [`) and SS3
(`ESC O`) sequence handling is untouched (INV-decode-terminates).

1. **Coalesce `ESC`+printable → `Alt+key`.** Today `ESC` followed by a non-CSI
   byte returns a standalone `Escape` stroke and defers the next byte. New: when
   a printable/keycode byte follows `ESC` in the same decode buffer, emit one
   `KeyStroke{code, alt=true}` consuming both bytes. The `shift` flag is derived
   from letter case exactly as the plain-printable path does today
   (`first>='A'&&first<='Z'`, `ssg_terminal.cpp:932`), so `ESC P` → `Alt+Shift+
   KeyP` and `ESC p` → `Alt+KeyP` are distinct — the step-1 oracle pins both.
   - **`[` and `O` are excluded from coalescing and illegal as `Alt` strokes.**
     `ESC [` and `ESC O` are the CSI/SS3 introducers, so `Alt+BracketLeft`
     (`ESC [`) can never be produced — it is byte-identical to a CSI prefix. The
     keymap generator therefore MUST reject any `Alt+<key>` whose byte encodes to
     `[` or `O`. `Alt+BracketRight` (`ESC ]`) is fine and coalesces normally, so
     the collision is asymmetric — see the tab.previous rebind below.
2. **A lone `ESC` is `Escape`, emitted only after the disambiguation window.**
   The existing `inputExhausted` path yields a bare `Escape` when no byte
   follows. Because a bare `Escape` now *fires an action* (cancel/abort) instead
   of merely starting a chord, that action is emitted only after the
   disambiguation window closes with no following byte — otherwise a split-read
   `Alt+X` (see Considerations M3) would fire a spurious cancel before its letter
   arrives. This adds a small, bounded latency to Escape, which is below
   perception for a cancel.

Additionally, decode gains **C0 → `Ctrl+letter`**: control bytes `0x01`–`0x1a`
**except** `0x08` (Backspace), `0x09` (Tab), `0x0a` (LF/Enter), `0x0d`
(CR/Enter), and `0x1b` (Escape) map to `KeyStroke{letter, control=true}`. This
makes Ctrl chords decodable so option B is real; it is unconditional (not
config-gated) because decode should report what was pressed and let the keymap
decide what is bound. `0x00` and `0x1c`–`0x1f` are out of the letter range and
stay untouched.

C (kitty/CSI-u) is out of scope; the coalescing heuristic is the only shipped
disambiguation path.

### Default keymap (`src/EditorRuntime.cpp` `defaultTerminalKeymap`)

The default map is **generated from the configured modifier**. Every current
leader binding `{Escape, X, …}` becomes `{Mod+X, …}` — the leader `Escape` is
folded into the modifier of the first stroke; trailing strokes stay plain. So
`{Escape, KeyS}` → `{Alt+KeyS}`, and `{Escape, KeyF, KeyT}` (settings.open) →
`{Alt+KeyF, KeyT}`. `Mod` is `Alt` by default, `Ctrl` if configured.

**Exceptions to the mechanical fold (the fold is NOT purely mechanical):**

- **tab.previous** is `{Escape, BracketLeft}` today; `Alt+BracketLeft` is
  undecodable (`ESC [` = CSI). It is rebound to `Alt+Comma`, and tab.next
  (`{Escape, BracketRight}`, which folds cleanly to `Alt+BracketRight`) is moved
  to `Alt+Period` for symmetry. The bracket strokes remain available as
  user bindings only once a CSI-u path exists.
- **quit** is NOT in the keymap — it is an app-local literal at
  `apps/ssg_main.cpp:1455-1457` matching `{Escape, KeyQ}`. Freeing Escape makes a
  single Escape resolve immediately, so `{Escape, KeyQ}` can never accumulate.
  Quit is rebound to `Alt+Q` and that literal is deleted; an oracle asserts quit
  stays reachable.

Escape gains explicit bindings in place of `{Escape, Escape}`: a single `Escape`
resolves to `prompt.cancel` (prompt), `find.close` where a find/replace prompt is
focused, and an editor-context abort (clear multi-cursor/selection). A bare
`Escape` also aborts a *pending* multi-stroke chord (e.g. after `Alt+F` while
waiting for `KeyT`); because the app loop, not the keymap, owns the pending-chord
buffer, this abort is handled there (`apps/ssg_main.cpp` chord loop), not as a
keymap terminal — which also keeps Escape from being a keymap prefix of the
surviving multi-stroke chords. Multi-stroke sequences keep a pending state; the
former "leader hint" is now a "chord pending" hint (same `leaderPending`
plumbing, renamed).

### Configuration (`doc/config.md`, init.lua)

One new config command, `keymap.set_chord_modifier` with `{ modifier = "alt" |
"ctrl" }`, added to the init.lua allow-list. It regenerates the default map with
the chosen modifier before user `keymap.bind` calls apply. `alt` is the default
when unset. Existing `keymap.bind` grammar already accepts `Alt+`/`Ctrl+`
prefixes, so user bindings need no grammar change.

## Invariants

- **INV-browser-reachable** (`doc/spec-mod-keys.md`): the *primary* chord
  modifier must be one the browser host delivers to the page. Alt qualifies as a
  class where Ctrl does not, but **not every `Alt+` combo is page-reachable** —
  mainstream browsers reserve `Alt+Left`/`Alt+Right` (history), `Alt+Home`,
  `Alt+D` (address bar), and `Alt+F` (menu). The default map must not *depend* on
  a browser-reserved `Alt` combo for a browser-shared action; where it does
  (e.g. `Alt+ArrowLeft/Right` word-nav, `{Alt+F, KeyT}` settings), that combo is
  verified per-browser (the capture matrix) or accepted as terminal-only. This
  invariant is **manually verified** against Firefox/Edge/Chrome, not gated by
  the C++ suite (which has no browser host).
- **INV-decode-terminates / INV-reply-never-input**: the Alt coalescing must not
  alter CSI/SS3/OSC/mouse parsing; a sequence is still consumed whole or not at
  all. `[`/`O` after `ESC` stay sequence introducers, never `Alt` strokes.
- **INV-settings-hatch**: a `settings.open` binding must always exist and be
  reachable; the migrated `{Alt+KeyF, KeyT}` inherits the protected-binding rule
  (keyed by command, not by the literal sequence).
- **INV-keymap-valid**: the generated map is prefix-free and unambiguous under
  `KeymapMatcher::validate` for both modifier choices, and the generator rejects
  strokes that cannot be decoded (`Alt+[`, `Alt+O`; illegal Ctrl strokes).
- **INV-one-keymap**: the same keymap resolves in both the terminal and browser
  hosts; no host-specific fork. **Manually verified** (no browser host in the
  suite); `validate()`-empty proves validity, not browser delivery.

## Considerations

- **Escape vs Alt split-read is a real robustness regression, not a wash.**
  Today the leader degrades gracefully: a lone `ESC` that times out becomes a
  bare `Escape` stroke that starts a **Pending** chord and waits *indefinitely*
  for the next stroke, so `Escape`⏸`S` resolves `file.save` no matter the gap
  (`ssg_terminal.cpp:688-690`, `kEscapeTimeoutMs≈30` at `ssg_main.cpp:204`). The
  new model needs `Alt+S` as one *coalesced* stroke from `ESC s` in a single
  window; if the two bytes split across reads with a gap beyond the window,
  decode commits to a bare `Escape` action AND routes `s` as text — the command
  is lost and a spurious cancel fires. This is strictly worse than the leader for
  the split-read case, and it is inherent to removing Escape's prefix role.
  Mitigations: (1) emit Escape's action only *after* the window (above), so a
  short split still coalesces; (2) make the window larger/adaptive for
  high-latency links, tuned via the same override surface as other terminal
  knobs. A transcontinental gap beyond any sane window still breaks — that
  residual is accepted and documented, not hidden. The split-read path gets its
  own app-loop oracle (Plan step 5a).
- **`Alt+Escape` and `Escape Escape`.** Define `ESC ESC` as a bare `Escape`
  (abort), not `Alt+Escape`; there is no useful `Alt+Escape` binding and this
  keeps double-tap-Escape harmless.
- **macOS Option.** `Option+letter` emits a composed character unless the
  terminal maps Option→Meta. Documented caveat, not solvable in SSG; note it in
  config.md next to the modifier setting.
- **Ctrl fidelity and the tty driver.** SSG puts the terminal in raw mode with
  `ISIG`/`IXON` disabled (`apps/ssg_main.cpp:114-115`), so `Ctrl+C/S/Q/Z` reach
  SSG as raw bytes rather than signals/flow-control — the tty-driver interception
  worry does NOT apply to the terminal host. It still applies in the browser
  (chrome eats `Ctrl+W/T/N/Tab`). Under a Ctrl-primary config, `Ctrl+H/I/J/M/[`
  are unbindable (they are Backspace/Tab/LF/Enter/Escape) and `Shift` cannot
  combine; the generator must reject or skip such bindings rather than emit an
  invalid map.
- **Muscle memory.** Users who press `Alt+key` today already hit the new
  bindings (same bytes); users who press `Escape` then a key at human pace will
  now get `Escape` (abort) followed by the key as text. This is the intended
  break, called out in the changelog/README.
- **Shift semantics — what is actually bindable.** Terminals transmit Shift in
  two incompatible ways, and the keymap must respect the split:
  - *Printable keys bake Shift into the glyph.* The wire carries the resulting
    character, never "base key + Shift bit". `decode_input` recovers Shift **only
    for A–Z**, by letter case (`ssg_terminal.cpp:932`). So `Alt+P` and
    `Alt+Shift+KeyP` are the SAME event (`ESC 0x50`), and `Alt+p` (`ESC 0x70`) is
    the distinct lowercase stroke — the case *is* the shift.
  - *Shifted digits/punctuation are NOT bindable as `Shift+base`.* `Shift+8`
    arrives as `*` (`ESC 0x2a`); `asciiKeyCode` returns `None` for everything
    outside `a-z/A-Z/0-9` and the unshifted set `[ ] \ ; ' , . / - = \` space`
    (`ssg_terminal.cpp:398-412`), so the symbol routes as text, not a chord.
    `Alt+Shift+Digit8` is writable but never matches; `Alt+"*"` cannot be named.
    Default chords therefore use letters (reliable case→shift) and *unshifted*
    digits/punctuation only — never `Alt+Shift+<digit>` or a shifted symbol.
  - *Function/navigation keys carry Shift explicitly.* Arrows, Insert, Delete,
    Home/End, PageUp/Dn, F-keys transmit via CSI/SS3 with a modifier bitmask
    (`m = 1 + Shift + 2·Alt + 4·Ctrl`), decoded into real flags
    (`ssg_terminal.cpp:831-836`). For these, `Alt+Shift+Insert` is well-defined
    and combinable. (Numpad keys are terminal-dependent and undecoded today —
    do not bind them.)
  - Oracle: a decode case pins that `ESC *` (Shift+8) yields text with
    `code=None`, not `{Digit8, shift}`, so the limitation is regression-locked
    (Plan step 1).

## Risks and Mitigations

- *A pasted or program-emitted `ESC`+letter is misread as `Alt`.* — Bracketed
  paste already fences pasted content; program output is not keyboard input.
- *Split-read makes Escape feel laggy.* — Bounded window (~tens of ms) is below
  perception for cancel; tune via the same override surface as other term knobs.
- *A Ctrl-primary user hits a wall of unbindable keys.* — Generator validates and
  the config doc states the reduced fidelity up front; Alt remains default.
- *Regression of the settings escape hatch.* — Protected-binding rule is by
  command id; an oracle asserts `settings.open` stays reachable after migration.

## Acceptance (Definition of Done)

- Observable: in the terminal, `Alt+S` saves, `Alt+P` opens the file finder, a
  single `Escape` closes an open find prompt in one press; `ssg --capabilities`
  unaffected. Visual signoff required for the one-press-Escape feel.
- Budgets: added Escape-decision latency ≤ one disambiguation window (tens of
  ms); split-read beyond the window is a documented correctness loss, not just a
  latency cost (Considerations M3), and Alt+X across such a gap is accepted to
  fail.
- Gates: `bash scripts/check.sh` green (build + 93 suites, 0 warnings, -Werror).
- Manual: INV-browser-reachable and INV-one-keymap are signed off against
  Firefox/Edge/Chrome by a written checklist (no browser host in the suite).
- Oracles: see the Plan's Oracle column; each decode and keymap behavior is
  pinned by an independent reference case before its code exists.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Coalesce `ESC`+printable → `Alt+key` (case→shift); exclude `[`/`O`; a lone `ESC` is `Escape` emitted after the window; `ESC ESC`=`Escape` | apps/ssg_terminal.cpp, tests/test_ssg_app.cpp | ref-table: `ESC s`→Alt+KeyS, `ESC P`→Alt+Shift+KeyP, `ESC p`→Alt+KeyP, `ESC *`→text(code=None), `ESC`(exhausted)→Escape, `ESC[1;3D`→ArrowLeft+alt (unchanged) | INV-decode-terminates |
| 2 | Decode C0 `0x01`–`0x1a` (excl. 0x08/0x09/0x0a/0x0d/0x1b) → `Ctrl+letter` | apps/ssg_terminal.cpp, tests/test_ssg_app.cpp | ref-table: `0x13`→Ctrl+KeyS; `0x09`→Tab, `0x0a`/`0x0d`→Enter (unchanged) | INV-decode-terminates |
| 3 | Generate `defaultTerminalKeymap` from a modifier param; fold `{Escape,X,…}`→`{Mod+X,…}`; rebind tab.prev→Alt+Comma, tab.next→Alt+Period; bind bare `Escape`→prompt.cancel/find.close/editor-abort | src/EditorRuntime.cpp, tests/runtime/test_runtime_snapshot.cpp | resolveSequence: Alt+S→file.save; {Alt+F,KeyT}→settings.open; Escape→prompt.cancel in prompt; Alt+Comma→tab.previous | INV-settings-hatch, INV-keymap-valid |
| 4 | Property: every former `{Escape,…}` default has a `{Mod+…}` equivalent (bracket exceptions noted), map validates for both `alt` and `ctrl`, generator rejects `Alt+[`/`Alt+O`/illegal Ctrl | src/EditorRuntime.cpp, tests/runtime/test_runtime_snapshot.cpp | property: fold-equivalence; `validate()` empty; rejection cases | INV-keymap-valid, INV-one-keymap |
| 5 | Migrate quit `{Escape,KeyQ}`→`Alt+Q`; delete the app-local literal | apps/ssg_main.cpp, tests/test_ssg_app.cpp | quit fires on Alt+Q; no Escape,Q accumulation path remains | - |
| 5a | One-press Escape + split-read: single `Escape` cancels a focused prompt and aborts a pending chord; `ESC`+letter split across reads within the window still coalesces to `Alt+letter` | apps/ssg_main.cpp, tests/test_ssg_app.cpp, tests/runtime/test_runtime_editing.cpp | app-loop test feeding ESC and the letter in separate reads across/within the deadline; bare Escape closes find in one press | INV-browser-reachable |
| 6 | `keymap.set_chord_modifier` config command + allow-list; regenerate defaults; rename `leaderPending`→chord-pending hint | src/… lua binding, src/runtime/snapshot.cpp, doc/config.md | modifier="ctrl" yields control=true default strokes; "alt" yields alt=true | INV-one-keymap |
| 7 | Docs: config.md (modifier + macOS Option caveat + Ctrl fidelity), spec-mod-keys.md (Alt vs Ctrl browser reachability, per-combo not blanket), README/changelog break note; regenerate commands.md | doc/config.md, doc/spec-mod-keys.md, doc/commands.md | test_commands reference current | - |

## Rationale (optional, skippable)

The leader existed to sidestep two problems at once: browser chrome eating
`Ctrl` combos, and terminals not disambiguating modifiers. Escape-as-leader
solved both by using a single unambiguous byte and a two-stroke sequence — at the
cost of holding Escape hostage and doubling the keystrokes for every command. The
field data changed the calculus: `Alt+letter` both survives browser chrome (the
user's Firefox/Edge test) and is byte-identical to the leader already, so moving
to Alt-primary keeps every property the leader bought, frees Escape, and halves
the keystrokes — with the only new cost being a small, well-understood
Escape-vs-Alt timing rule that every serious terminal app already implements.
Ctrl is kept as a configurable option for terminal-only users who prefer it, with
its limitations stated rather than discovered.

### Where the "leader" goes (recording the fork, not deciding it)

Removing the Escape *prefix* does not have to mean removing the *idea* of a
leader. Three positions stack on this spec's foundation; this spec ships #1 and
leaves the rest open:

1. **Mod-chords only (this spec).** Frequent actions are `Alt+key`; everything
   else is reached through the palette's fuzzy find (`Alt+Shift+P`, type a few
   characters, Enter). Lowest surface area; Escape freed.
2. **Modal leader (additive, future).** `Alt+Esc` enters a persistent command
   *mode* (vim-normal / which-key style, not Emacs's transient `C-x` prefix —
   that transient model is what SSG's multi-stroke chords already are). The key
   architectural point: a modal leader is **one more focus context**, resolved by
   the existing per-context resolver (`editor`/`panel`/`prompt`/`*`), NOT a
   conditional threaded through every binding. `Alt+Esc` sets focus to a `leader`
   context; bare keys resolve there; `Esc` or any command returns to `editor`.
   New parts are only a focus state, its enter/exit transitions, and a sticky
   version of the chord-pending hint. It composes with #1 without conflict.
3. **Palette-primary.** Same as #1 but explicit: rare commands live only in the
   palette. Muscle memory is largely preserved — a couple more keypresses for
   infrequent actions — and it needs no new mode machinery.

Recommendation: ship #1, rely on the palette (#3 is #1 made explicit), and hold
#2 as a clean, context-based addition if palette round-trips ever feel like too
much friction. The decision is deferred; nothing here forecloses it.
