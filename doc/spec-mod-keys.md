# spec-mod-keys

Research/reference doc: modifier-key reliability across SSG's dual host
targets (raw terminal + browser client), what SSG does today, what other
editors do, and what an eventual configurable-modifier design should
account for. This is background for `doc/spec-config.md`'s M2
(configurable keybindings) — not itself a Plan to implement; fold anything
actionable into `spec-config.md` when it's ready to build.

> HISTORICAL NOTE: SSG has since removed the Escape leader entirely
> (`doc/spec-remove-leader.md`, implemented).  Frequent actions are now single
> `Alt+<key>` chords and Escape is a plain one-press cancel.  The reliability
> analysis below is why the leader was chosen originally and why Alt (not Ctrl)
> became the replacement primary; the "What SSG does today" section describes the
> pre-removal state.

## What SSG did (pre-removal)

SSG's leader key was `Escape`. Every global (`*`-context) command chord was
`Escape` followed by one or more further strokes (`Escape S` → `file.save`,
`Escape P` → `palette.open`, etc.). This was a deliberate
choice, not an accident: `Escape` is the one input that is simultaneously
reliable in both of SSG's host environments, for two independent reasons
(see below) — everything else considered (Ctrl-primary, Alt-primary) fails
one host or the other.

### Alt already works as an alternate leader, for free, by coincidence of terminal encoding

`apps/ssg_terminal.cpp`'s `decode_input` has no code path that recognizes
"Alt". When it sees the byte `0x1b` (`Escape`) followed by a byte that
ISN'T `[` or `O` (i.e. not the start of a CSI/SS3 sequence), it decodes
`Escape` as one complete, standalone stroke and decodes the following byte
as a SEPARATE stroke on the next call:

```cpp
if (second != '[' && second != 'O') {
    // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke;
    // the next byte is decoded on the following call.
    consumed = 1;
    return {DecodeStatus::key, ssg::KeyStroke{"Escape"}, {}, 0};
}
```

Most terminals (the xterm-descended majority) encode `Alt+<key>` using the
classic "meta sends escape" convention: they send the raw byte `0x1b`
immediately followed by the plain key byte, with no separate encoding to
say "this was Alt, not two separate keystrokes." So `Alt+S` arrives over
the wire as `0x1b 0x73` — the EXACT same two bytes as pressing `Escape`
then `S` as two separate keystrokes. SSG's decoder can't tell them apart,
and doesn't try to; it just decodes both as `[Escape, KeyS]`, which
resolves against the keymap identically either way.

**Consequences of this being incidental, not designed:**
- It works for any SINGLE-stroke-after-leader chord (`Escape S`, `Escape
  P`, ...) but not multi-stroke ones (`Escape F T` for `settings.open`) —
  there's no way to hold Alt through a whole 3-stroke chord this way.
- It's untested. No test asserts this equivalence anywhere in the
  codebase; it's purely an emergent property of terminal encoding
  conventions plus the decoder's own CSI/non-CSI disambiguation logic.
- It is fragile to any change that swaps the leader stroke away from
  literal `Escape` (see `doc/spec-config.md`'s `keymap.set_leader`): the
  free Alt-equivalence exists ONLY because `Escape`-prefix and
  Alt-prefix collide at the byte level for THAT ONE specific stroke.
  Setting the leader to, say, `Ctrl+Space` would not carry this property
  forward automatically — `Ctrl+Space` sent as a real terminal byte
  sequence looks nothing like `Alt+<key>`'s encoding.

## Alt as a deliberately-configured modifier (first case: word navigation)

Word navigation surfaced a case where the free Escape/Alt collision
above does **not** apply: `Escape+ArrowLeft`/`Escape+ArrowRight` (bound to
`cursor.word_left`/`cursor.word_right`, see `doc/spec-keymap.md`) work
today, but `Alt+ArrowLeft`/`Alt+ArrowRight` — the conventional editor
shortcut for the same action — did **not**, even though `Alt+<letter>`
chords work "for free" per the section above. This is not a bug in the
free-Alt trick; it is the trick's documented boundary (Consequences,
above) meeting a real case for the first time.

**Why arrows are different from letters.** The free Escape/Alt collision
exists only because `Alt+<letter>` and `Escape` `<letter>` happen to be
byte-identical (`meta sends escape`: raw `ESC` immediately followed by the
plain key byte). Arrow keys do not have a "plain key byte" to prefix —
they are already multi-byte CSI sequences (`ESC [ D` for `ArrowLeft`).
Modern terminals that support the modifier-parameter CSI form (`ESC [ 1 ;
m D`, the same `modifyOtherKeys`-descended convention `decode_input`
already parses for `Shift`/`Ctrl`+arrow — see `apps/ssg_terminal.cpp`)
encode `Alt+ArrowLeft` as `ESC [ 1 ; 3 D` (`m=3` = `1 + bit1(Alt)`): a
single stroke carrying `alt=true`, not two strokes that happen to look
like an `Escape`-prefixed chord. `decode_input` already parses this form
correctly (`KeyStroke{"ArrowLeft", .alt=true}`) and always has —
`tests/test_ssg_app.cpp`'s `decodeInputModifiedArrows` covers it — but
until this change nothing in the keymap was bound to that stroke, so the
input decoded fine and then simply matched no binding (a silent no-op,
same "unbound stroke" behavior as any other unmapped key).

**The fix**: rather than relying on more byte-level coincidence (there
isn't one to rely on for arrows), `Alt+ArrowLeft`/`Alt+ArrowRight`/
`Alt+Shift+ArrowLeft`/`Alt+Shift+ArrowRight` are now EXPLICIT entries in
the compiled-in default keymap (`defaultTerminalKeymap()`,
`src/EditorRuntime.cpp`), bound to the exact same commands as their
`Escape`-prefixed counterparts (`cursor.word_left`/`cursor.word_right`/
`select.word_left`/`select.word_right`). Two independent sequences
resolving to the same command is not a new mechanism — `KeymapViewState`
already allows any number of bindings per command id (see
`KeymapMatcher::preferredBinding`'s existing "multiple bindings, pick the
shortest" handling) — this is simply the first curated binding to use
that shape deliberately, as a documented design choice rather than a
coincidence: **Alt is graduated from "accidental Escape-collision" to a
first-class, explicitly-bound secondary modifier**, at least for this one
pair of commands.

This does not generalize automatically to other arrow-adjacent keys
(`Home`/`End`/`PageUp`/`PageDown` etc.) or other modifiers — each would
need its own explicit binding decision the same way, since none of them
inherit reliability from the Escape/Alt byte collision either. It also
does not change anything about `keymap.bind`'s Lua surface: `Alt+` was
already a valid stroke-modifier token in `KeyCodec::parseStroke`'s
grammar (`Ctrl+`/`Alt+`/`Meta+`/`Shift+`, any combination) before this
change, and remains the only place a user configures new bindings — no
new Lua API, no new modifier syntax, no new resolution mechanism. What
changed is which sequences the COMPILED-IN default keymap curates, not
what's expressible.



Emacs's terminal input model is functionally identical, but documented
as first-class behavior rather than incidental:

- **Ctrl is Emacs's PRIMARY modifier** (`C-x C-s`, etc.). `Ctrl+letter`
  maps to a literal ASCII control byte (`0x01`-`0x1a`) that every terminal
  sends as one unambiguous byte with zero escape-sequence ambiguity —
  this is the most reliable modifier a raw terminal can express, which is
  why Emacs (and `nano`, and `micro`) lean on it heavily.
- **Meta (Emacs's name for what's usually the physical Alt key) is
  secondary**, and the manual states outright:
  > "You can also type Meta characters using two-character sequences
  > starting with ESC. Thus, you can enter `M-a` by typing `ESC a`...
  > This feature is useful on certain text terminals where the Meta key
  > does not function reliably."

  (`https://www.gnu.org/software/emacs/manual/html_node/emacs/User-Input.html`)

Emacs treats `ESC` + key and `Alt` + key as literally the same input, on
purpose, for the identical underlying reason SSG's decoder produces that
equivalence by accident: terminals conventionally encode Alt as
`ESC`-prefix, so the two are indistinguishable without a modern
extension (see Kitty protocol below). Emacs's docs also note three
further modifiers (`Super`, `Hyper`, `Alt`-as-distinct-from-Meta) that
"few terminals provide ways to use" — reinforcing that anything beyond
Ctrl/Meta is unreliable in a terminal, historically.

## Why Ctrl-primary (Emacs's, `micro`'s, `nano`'s approach) doesn't generalize to SSG

`Ctrl+letter` is maximally reliable in a RAW terminal, but SSG is a
library-first editor with more than one client host (`doc/spec.md`) —
including, eventually, a browser-embedded terminal. In a browser tab,
`Ctrl`-based shortcuts hit a DIFFERENT, structural failure mode that has
nothing to do with terminal byte encoding: many `Ctrl` combinations are
intercepted by the BROWSER CHROME itself, before any page JavaScript ever
receives a `keydown` event. Confirmed common examples across mainstream
browsers: `Ctrl+T` (new tab), `Ctrl+N` (new window), `Ctrl+W` (close
tab/window), `Ctrl+Tab`/`Ctrl+Shift+Tab` (switch tabs). These cannot be
recovered with `event.preventDefault()`/`stopPropagation()` — the
keydown is never dispatched to the page at all. Other Ctrl combos
(`Ctrl+S`, `Ctrl+P`) are SOMETIMES recoverable, inconsistently, across
browsers and contexts.

This is why a Ctrl-primary scheme (what `micro` uses, and what the user
flagged as "trouble ... when the terminal is embedded in a browser") is
fine for a terminal-only editor but cannot be ssg's primary mechanism if
a browser-hosted client is ever expected to share the same keymap:
no amount of app-level cleverness recovers a keystroke the browser chrome
already consumed.

`Escape` has no such reservation in any mainstream browser — there is no
global browser-chrome action bound to a bare `Escape` keydown — so it
reaches page JavaScript reliably in a browser host, and it is a single,
unambiguous byte in every terminal host. It is the one input reliable in
BOTH of SSG's targets, which is why it was chosen as the leader, and
that reasoning generalizes beyond "it works in today's terminal client."

## The modern, more precise answer: the Kitty keyboard protocol (CSI u)

**Status (implemented, Tier A):** SSG now enables the Kitty keyboard protocol
(progressive-enhancement flag 1) automatically on any terminal that answers the
capability query, and decodes `CSI ... u` key events into the same neutral
`KeyStroke` the legacy path produces. This fixes the Caps-Lock ambiguity for
`Alt+Shift+<letter>` / `Ctrl+<letter>` bindings (shift comes from the modifier
bit, not letter case). See `doc/spec-kitty-keyboard.md`. A true at-rest Caps-Lock
*indicator* still needs the higher flags (report-all-keys) and remains deferred
(Tier B).

Legacy terminal encoding cannot express many modifier combinations at
all (e.g. `Shift+Enter` vs. `Enter`, most `Ctrl+Shift+<key>` combos).
The [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/)
(a `CSI ... u`-terminated escape code, standardized further as a
cross-terminal "terminal working group" spec) solves this properly: one
escape code per key event carries an exact modifier bitmask (`shift`,
`alt`, `ctrl`, `super`, `hyper`, `meta`, `caps_lock`, `num_lock`) plus
press/repeat/release state, with no encoding ambiguity at all. It is
OPT-IN — a terminal application requests it via a negotiated escape
sequence (`CSI > 1 u` to enable, with capability/progressive-enhancement
querying) — and is now supported by kitty, foot, WezTerm, iTerm2,
Alacritty, ghostty, and others.

**Browser-host update (verified via web search, early 2026):** xterm.js
(the terminal engine behind most browser-embedded terminals, including
VS Code's web terminal) added Kitty protocol / `CSI u` support as of
v6.1.0. This means the "modern reliable modifier" story is no longer
terminal-only — a browser-hosted SSG client running on a sufficiently
recent xterm.js could, in principle, negotiate unambiguous modifier
reporting too, not just fall back to Escape-prefix tricks. This wasn't
true even a year prior to this doc being written, so it's worth
re-checking xterm.js's adoption status again before committing to any
design that assumes it.

## Aspirational design considerations for a future configurable-modifier feature

None of this is designed or scheduled yet — these are the constraints a
future `spec-config.md` increment (beyond M2's `keymap.set_leader`) would
need to reconcile if SSG ever wants first-class, explicitly-configured
modifier support (as opposed to today's incidental Alt-via-Escape-prefix
overlap):

- **Detecting Kitty-protocol availability** would need a real capability
  probe (the protocol's own query mechanism), not an assumption — not
  every terminal, and not every xterm.js VERSION, supports it. A design
  must degrade gracefully to legacy Escape-prefix behavior when the probe
  fails or times out, exactly as SSG's existing `kEscapeTimeoutMs`
  Escape/CSI disambiguation already degrades gracefully today.
- **A configured leader stroke that ISN'T literally `Escape` loses the
  free Alt-equivalence** documented above — this is not a bug to fix, it
  is a property that only exists for `Escape` specifically, because of
  how terminals happen to encode Alt. `keymap.set_leader` (see
  `doc/spec-config.md`) changing the leader away from `Escape` should be
  expected to ALSO disable "Alt works as an alternate leader" as a side
  effect, since nothing else provides that overlap. If a user rebinds the
  leader to something else and still wants an Alt-based fast path, that
  would need to be a SEPARATE, explicit binding (or a future first-class
  "also accept Alt+X for chords starting with the leader" toggle), not an
  emergent property of byte-level encoding coincidence like today's.
- **Ctrl remains the most broadly reliable RAW-terminal modifier** (one
  control byte, universal), but is the LEAST browser-portable one
  (chrome-level reservations, see above) — any future config surface
  should treat "Ctrl-heavy bindings" as a terminal-only-safe choice a
  user opts into knowingly, not a default, if browser-hosted clients are
  meant to share the same keymap.
- **A capability-negotiated modifier scheme (Kitty protocol) is
  fundamentally a CLIENT-side (terminal-app) concern**, not something
  `EditorRuntime` can detect or control — per the existing keymap
  ownership split (`doc/spec-keymap.md`: "library owns resolution and
  validation, client owns byte→KeyStroke decoding"), any Kitty-protocol
  adoption would live entirely in `apps/ssg_terminal.cpp`'s decoder (and
  an eventual browser client's own decoder), publishing ordinary
  `KeyStroke`s with correctly-populated modifier bits to the SAME
  `KeymapMatcher` resolution engine that already exists — no change to
  the library's keymap model would be needed, only to how faithfully a
  given client can populate `KeyStroke.alt`/`.control`/`.shift`/`.meta`
  from whatever protocol its host terminal actually speaks.

## Open questions (not yet answered, revisit before designing further)

- Does SSG want to formalize (test, document, guarantee) the current
  Alt-as-Escape-prefix equivalence for the shipped `Escape` leader
  specifically, given it is currently a pure byte-encoding coincidence?
  (Partially answered for one case: `Alt+ArrowLeft`/`Alt+ArrowRight` are
  now an explicit, tested, non-coincidental binding — see "Alt as a
  deliberately-configured modifier" above. The general question — whether
  EVERY `Escape`-led chord should also get an explicit `Alt`-prefixed
  alternate binding, vs. deciding this case-by-case as each is requested
  — remains open.)
- Should `keymap.bind`/`keymap.set_leader` (see `doc/spec-config.md`)
  expose any notion of "also accept this alternate stroke" for a chord,
  to let a user deliberately restore an Alt-equivalent fast path after
  changing the leader away from `Escape`?
- Is Kitty-protocol negotiation worth adopting in `apps/ssg_terminal.cpp`
  at all while SSG remains terminal-primary, or should it wait until a
  browser client exists and needs the SAME reliable-modifier story to be
  worth the added negotiation/fallback complexity?

## Sources

- GNU Emacs manual, "User Input":
  `https://www.gnu.org/software/emacs/manual/html_node/emacs/User-Input.html`
- GNU Emacs manual, "Modifier Keys":
  `https://www.gnu.org/software/emacs/manual/html_node/emacs/Modifier-Keys.html`
- Kitty keyboard protocol specification:
  `https://sw.kovidgoyal.net/kitty/keyboard-protocol/`
- xterm.js Kitty-protocol support (verified via web search, early 2026;
  landed around v6.1.0) — re-verify current status before relying on it.
- Browser Ctrl-shortcut chrome-layer interception (verified via web
  search; consistent with common web-development knowledge of
  unpreventable browser-reserved shortcuts like `Ctrl+T`/`Ctrl+N`/
  `Ctrl+W`/`Ctrl+Tab`).
