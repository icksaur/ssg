# spec-terminal-capabilities

Status: implemented; pending real-terminal signoff

Every capability answer has been verified against a simulated terminal in a pty
harness, never against a real terminal emulator. The Acceptance clause below
asks for a capability report that correctly identifies kitty locally; until
someone runs `ssg --capabilities` in a real terminal and confirms the answers,
this stays open. The risk is narrow but real: the query bytes and the reply
grammar are both taken from research rather than from a normative document, and
a simulated responder answers exactly what the harness was told to answer.

## Goals

Discover at runtime what the attached terminal can actually do, in one place, so
that features can be adopted without guessing from `$TERM` and without a startup
stall.

After this work SSG asks the terminal a fixed set of questions at startup, never
blocks waiting for the answers, and exposes the results through one object with
one precedence rule. A terminal reply can never reach the document as text — a
defect that exists today and is demonstrated below.

Nothing in this spec turns a new feature on. It is the seam that makes styled
underlines, synchronized output, OSC 52 and the keyboard protocol adoptable
later, each on its own merits.

## Current state (verified in code and by measurement)

- Capability handling today is one function: `detect_color_depth`
  (`apps/ssg_terminal.cpp`), which resolves truecolor from an override env var,
  then `COLORTERM`, then `TERM` hints, then a default. It sends nothing and asks
  nothing. It is the precedent this spec generalises.
- The main loop already `select()`s over stdin, the signal self-pipe, the
  git-diff wake descriptor and the init-script wake descriptor
  (`waitReadiness`, `apps/ssg_main.cpp:177`). **Terminal replies arrive on
  stdin**, which that loop already watches.
- Input decoding is a pure function, `decode_input(bytes, inputExhausted,
  consumed) -> Decoded` with a `DecodeStatus` of
  `none | incomplete | key | scroll | pointer`.
- SSG's startup budget is **250 ms** to first viewport (`doc/spec-fast-startup.md`).

### Measured: the query costs almost nothing

Three speculative queries plus a DA1 fence, against a responder that answers
only DA1:

```
ELAPSED_MS=0.093
REPLY='\x1b[?62;22c'
```

**0.093 ms locally** — 0.04% of the startup budget. Over SSH it is one round
trip, so link latency. This is why the result is **not cached**: a cache file
open and parse would cost more than the query it replaces. There is also nothing
durable to key a cache on — `$TERM` is famously unreliable, capabilities are a
property of *this connection* rather than this machine, and they change under a
fixed key (Windows Terminal gained the keyboard protocol in v1.25 without
`$TERM` changing).

### THE HAZARD: replies are currently typed into the document

`decode_input` handles an unrecognised CSI by consuming **only the three-byte
introducer** and reporting `DecodeStatus::none`
(`apps/ssg_terminal.cpp`, the `default:` arm). The remaining bytes are then
decoded on the next call as ordinary printable text.

Verified by running real replies through the shipping decoder:

| Reply | Sequence | Leaked into the document as text |
|---|---|---|
| DA1 | `ESC [ ? 6 2 ; 2 2 c` | `62;22c` |
| DECRQM (mode 2026) | `ESC [ ? 2 0 2 6 ; 2 $ y` | `2026;2$y` |
| kitty keyboard | `ESC [ ? 1 u` | `1u` |
| cell pixel size | `ESC [ 6 ; 1 7 ; 8 t` | `8t` |

So **if SSG sent a single capability query today, the answer would be inserted
into the user's file.** This is not a theoretical concern about a future feature;
it is the reason this spec exists before any query is sent. It is also latent
rather than live only because SSG currently sends no queries — a terminal that
volunteers a report unprompted (a focus event, a resize report) would hit the
same path.

## Design

**One owner: `TerminalCapabilities`, in `ssg::app`.** It holds the questions,
the answers, the precedence rules and the overrides. It is a terminal-client
concern and stays out of the library (**I1**, **I12**), alongside
`detect_color_depth`, which it absorbs.

### The DA1 fence

The naive approach — send a query, block until it answers — hangs forever on a
terminal that ignores unknown queries, which is exactly the terminal you most
need to detect.

Primary Device Attributes (`ESC [ c`) is answered by *every* terminal. So the
speculative queries are written **first** and DA1 **last**. Replies arrive in
order, so the DA1 reply is a fence: anything that was going to answer has
answered by the time it arrives.

- A speculative reply seen before the DA1 reply -> the feature exists.
- Only the DA1 reply -> the feature does not exist.
- No DA1 reply at all -> not a terminal, or a broken one; every answer stays at
  its conservative default and nothing waits.

This needs no timeout for the normal case, which is why it is preferred over
"wait 100 ms and see". A timeout remains as the backstop for the third case
only.

### Nothing blocks

Queries are written at startup and the first frame is rendered immediately
against conservative defaults. Replies arrive through the `select()` loop that
already exists, are consumed by the decoder, and are handed to
`TerminalCapabilities`. A capability flips from "unknown" to "present" when its
answer lands, typically before the second frame.

Mechanism chosen: **fold into the existing readiness loop** rather than a
dedicated blocking handshake before the first frame. Blocking would be simpler
and is what most TUIs do, but SSG has a measured startup budget and a loop that
already watches stdin, so the non-blocking version costs one state field and
buys a guarantee that a slow or silent terminal can never delay the first frame.

The consequence, which must be designed for rather than papered over: **a
capability is unknown for the first frame or two.** Features must therefore be
safe to enable late, which is a real constraint on what may be negotiated this
way — see Considerations.

### Replies stop being input

`DecodeStatus` gains a `reply` variant, and `Decoded` carries the reply's bytes.
The decoder recognises the shapes a reply can take and consumes each **whole**:

- CSI with a `?` private prefix ending in a final byte (`c`, `u`, `y`, `$y`)
- CSI ending in `t` (window/cell reports)
- DCS ... ST (`XTVERSION`, `XTGETTCAP` answers)
- OSC ... ST/BEL (`OSC 52` answers, colour reports)

The rule that matters is not the list but the invariant: **a sequence SSG does
not understand is consumed to its terminator, never partially.** Partial
consumption is what turns an unrecognised reply into text, and it is the actual
defect. The list can grow; the invariant must not be violated.

The decoder classifies, and does not interpret. It reports "this was a reply and
here are its bytes"; `TerminalCapabilities` decides what the bytes mean. That
keeps the decoder pure and testable, and keeps protocol knowledge in one place.

#### A missing terminator must not stall input

"Consume to the terminator" is not safe on its own. A sequence whose terminator
never arrives — dropped bytes on a flaky link, a user pasting a bare `ESC [ ?`,
line noise — would leave those bytes at the head of the buffer forever, and
because the buffer is consumed strictly from the front, **every subsequent
keystroke queues behind them and the editor appears hung.** Waiting for a
terminator is only correct when one is still coming.

The mechanism for this already exists and must be preserved, not reinvented. The
app resolves the same ambiguity for a lone `ESC` today: on `incomplete` it waits
`kEscapeTimeoutMs` (30 ms) for more bytes, and if none arrive it re-decodes with
`inputExhausted = true` (`apps/ssg_main.cpp`, the bounded-Escape resolution).

**Measured against the shipping decoder, `inputExhausted` does not mean "no more
bytes ever".** It resolves the lone-`ESC` ambiguity and nothing else. Every other
partial form returns `incomplete` with `consumed = 0` even when exhausted:

```
csi_priv_only  ESC [ ?              exhausted -> none, consumed 3 of 3
csi_1_trunc    ESC [ 1              exhausted -> INCOMPLETE, consumed 0
csi_1_semi_2   ESC [ 1 ; 2          exhausted -> INCOMPLETE, consumed 0
csi_M_trunc    ESC [ M SP           exhausted -> INCOMPLETE, consumed 0
csi_lt_trunc   ESC [ < 0 ; 1 ; 1    exhausted -> INCOMPLETE, consumed 0
utf8_trunc     E2 94                exhausted -> INCOMPLETE, consumed 0
```

This is **deliberate and tested** (`decodeInputEscapeBoundaryIsBounded`: "a
truncated CSI is always incomplete regardless of exhaustion";
`decodeInputModifiedArrowSplitReadsAreIncomplete`;
`decodeInputPointerSplitReadsAreIncomplete`). It is what makes a sequence split
across two reads reassemble correctly, which matters most on exactly the slow
links where splits happen.

So **do not make the decoder total under `inputExhausted`.** Doing so would
discard a legitimate arrow or mouse event whose halves straddle the 30 ms
window — trading a rare deferral for routine input loss over SSH, which is the
wrong direction. An earlier draft of this spec required exhausted-totality; that
requirement is withdrawn, and the three tests above stay as they are.

The liveness property actually needed is weaker and costs nothing:

- **Bounded lookahead.** The decoder never holds more than `kMaxSequenceBytes`
  (256) of buffered bytes waiting for a terminator. Beyond that the run is not a
  sequence; it is consumed and discarded rather than held.

That suffices, because every existing `incomplete` path needs only a *small,
bounded* number of further bytes before it resolves, so the head of the buffer
always clears within a bounded amount of subsequent input. The one path with
genuinely unbounded appetite is the SGR mouse scan, which searches forward for
`M`/`m` with no limit — so a truncated `ESC [ <` today can swallow arbitrarily
much typed text waiting for an `m`. The reply scan this spec adds would be a
second such path. The cap closes both.

Discarding is deliberate. A malformed reply carries no usable answer, and the
capability it would have confirmed correctly stays at its conservative default.
  never return `incomplete`. It resolves with what it has: an unterminated
  reply-shaped run is consumed and discarded as a malformed reply — neither held
  nor emitted as text.
- **A reply-shaped run is bounded in length.** A report is tens of bytes; a
  scan that has not found a terminator within `kMaxReplyBytes` (256) is not a
  reply. It is consumed and discarded without waiting, so a long run of garbage
  cannot pin the buffer for even one timeout.

Discarding is deliberate. A malformed reply carries no usable answer, and the
capability it would have confirmed correctly stays at its conservative default.

#### Replies are only expected while a query is outstanding

Consuming and interpreting are separate decisions, and conflating them is the
trap. Consumption is unconditional — INV-reply-never-input holds for the whole
session, because an unrecognised sequence must never become text no matter when
it arrives. **Interpretation is windowed.**

A **probe window** opens when the queries are written and closes when the DA1
reply arrives — the fence already marks exactly the moment every answer that was
coming has come — or when the backstop timeout expires. Only inside that window
is a reply offered to `TerminalCapabilities`.

This matters because classifying reply shapes for the entire session means any
byte sequence that merely *looks* like a report is permanently reinterpreted,
including a future input protocol whose encoding overlaps a report's. Scoping
interpretation to the window means that outside it those bytes follow ordinary
input rules, and the only behaviour that persists is the safe half: they are
swallowed rather than typed.

It also shrinks the pasted-reply hazard from a permanent property to a
startup-window one, which is the difference between a design flaw and a race a
user has to work at hitting.

### What is asked, and what each answer gates

| Question | Sequence | Gates |
|---|---|---|
| Primary attributes (the fence) | `ESC [ c` | Nothing directly; also advertises OSC 52 via extension `52` |
| Synchronized output | `ESC [ ? 2026 $ p` | Frame tear-free redraw |
| Keyboard protocol | `ESC [ ? u` | Disambiguated keys |
| Cell pixel size | `ESC [ 16 t` | Images only — asked only if images are ever adopted |

Styled underlines are **deliberately absent**: there is no query for them, they
degrade to plain underline where unsupported, and the degradation is harmless.
Asking unanswerable questions is worse than not asking.

### Precedence, and the override

Every capability resolves the same way, generalising what `detect_color_depth`
already does:

1. An explicit override env var (`SSG_TERM_<CAPABILITY>=on|off`) — always wins.
2. The terminal's own answer, if one arrived.
3. An environment convention where one exists (`COLORTERM` for truecolor).
4. The conservative default: **absent**.

The override exists because terminals lie, and because a user hitting a
rendering bug needs a way to switch a feature off without rebuilding. "Unknown"
and "absent" deliberately resolve identically, so a late answer can only ever
turn something on.

## Invariants

- **INV-reply-never-input** (new): a byte sequence the decoder does not
  understand is consumed to its terminator and never emitted as document text.
- **INV-decode-terminates** (new): `decode_input` never holds more than
  `kMaxSequenceBytes` of buffered input waiting for a terminator. Any scan that
  reaches the cap without finding one consumes and discards its run. The head of
  the input buffer therefore always clears within a bounded amount of input,
  whatever bytes are in it.
- **INV-capability-single-source** (new): every capability answer is resolved by
  `TerminalCapabilities` under the precedence above. No feature reads `$TERM` or
  a query reply directly.
- **INV-startup-unblocked** (new): no capability query may delay the first
  frame. The first frame renders against defaults.
- **INV-app-io-only** (`doc/spec-terminal-robustness.md`): this is terminal I/O
  and lives in the app; no editor or layout logic joins it.
- **I1 / I12 — headless core**: the library gains nothing terminal-shaped. A
  WebSocket client negotiates its own capabilities through the protocol, not
  through this.
- **250 ms startup budget** (`doc/spec-fast-startup.md`): preserved, and
  trivially so at 0.093 ms.

## Considerations

- **Late answers constrain what can be negotiated this way.** A capability that
  changes *how input is parsed* — the keyboard protocol above all — cannot flip
  mid-session without a defined transition, because keystrokes in flight were
  encoded under the old rules. This spec makes the keyboard protocol
  *detectable*; adopting it needs its own spec that says what happens to input
  arriving during the switch. Output-only capabilities (synchronized output,
  underline styles) have no such problem and can flip freely.
- **A reply can be split across reads.** A 12-byte DA1 answer may arrive as two
  TCP segments over SSH. The decoder's existing `incomplete` status already
  handles this for CSI sequences and must handle it for replies too — the
  partial-consumption bug and the split-read case are the same bug seen twice.
  They are *not* the same as a reply whose terminator never arrives at all,
  which `incomplete` alone would turn into a hang; that case is closed by
  INV-decode-terminates above.
- **A user can type a reply.** Nothing stops someone pasting `ESC [ ? 1 u` into
  the editor, and at the byte level it is indistinguishable from a real answer.
  It is always *swallowed* rather than typed, which is the correct half of the
  behaviour. It is only *interpreted* as a capability answer if it lands inside
  the probe window, so the worst case is a false positive on a capability during
  the first milliseconds of startup. Bracketed paste closes the rest: inside a
  paste, bytes are content and must never be treated as replies.
- **Overrides need to be discoverable**, or a user hitting a bug has no way to
  find the escape hatch. They belong in `doc/config.md` with the rest of the
  user-facing configuration.
- **DA1 is also an answer, not only a fence.** Extension code `52` in the DA1
  reply advertises OSC 52 support on several terminals. Parse it rather than
  discarding it.
- **tmux and screen change the answers.** A multiplexer answers on its own
  behalf, and its answers are a subset of what the outer terminal can do. That
  is correct behaviour, not a bug to work around: the multiplexer is the
  terminal SSG is talking to.

## Risks and Mitigations

- **Risk: the decoder change breaks input.** It touches the hottest, most
  user-visible path in the application.
  *Mitigation*: the existing input tests plus a new oracle asserting that no
  reply leaks any text; drive the real binary over a pty and confirm typing,
  arrows, mouse and paste are unchanged before committing.
- **Risk: swallowing something that was legitimately input.** Widening what the
  decoder consumes as a reply could eat a sequence a user meant.
  *Mitigation*: only shapes that are unambiguously reports are classified as
  replies; anything else keeps its current handling. The oracle enumerates both
  directions — replies swallowed, ordinary input untouched.
- **Risk: a capability is read before its answer arrives** and the feature is
  silently disabled forever.
  *Mitigation*: "unknown" and "absent" resolve the same, so this can only
  under-enable, never mis-enable; and features are required to re-read rather
  than latch at startup.
- **Risk: a malformed or truncated sequence stalls all input.** The most
  dangerous failure mode of "consume to the terminator" is waiting for a
  terminator that never comes; the symptom is a hung editor, not a garbled one.
  The SGR mouse scan already has this shape, and the reply scan would add a
  second.
  *Mitigation*: INV-decode-terminates (bounded lookahead, not
  exhausted-totality — see the Design note on why totality is the wrong fix),
  pinned by `anUnboundedSequenceScanCannotHoldTheInputBuffer`.
- **Risk: scope creep into adopting the features.** This spec is the seam only.
  *Mitigation*: no feature is enabled here; each gets its own spec.

## Acceptance (Definition of Done)

- Observable: unchanged editor behaviour — same startup, same typing, same
  mouse, same paste — verified against the real binary over a pty, plus a
  capability report that correctly identifies kitty locally.
- Budgets: no measurable startup cost (query is sub-millisecond and does not
  block); no additional per-frame work.
- Gates: `bash scripts/check.sh` green — 0 warnings, all tests passing.
- Oracles:
  - the hazard -> `noCapabilityReplyIsEverEmittedAsText`
  - the converse -> `ordinaryInputIsUnaffectedByReplyRecognition`
  - split reads -> `aReplySplitAcrossReadsIsStillConsumedWhole`
  - liveness -> `anUnboundedSequenceScanCannotHoldTheInputBuffer`
  - the fence -> `onlyADa1ReplyMeansTheFeatureIsAbsent`
  - the window -> `aReplyShapeAfterTheFenceIsNotACapabilityAnswer`
  - precedence -> `anOverrideBeatsTheTerminalsOwnAnswer`
  - no stall -> `theFirstFrameIsWrittenBeforeAnyReplyIsRead`

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the hazard oracle FIRST, against today's decoder: feed each real reply through `decode_input` and assert no text is emitted. Add `anUnboundedSequenceScanCannotHoldTheInputBuffer` in the same step, asserting no scan holds more than `kMaxSequenceBytes`. **Both must FAIL immediately**, reproducing the leak table and the unbounded SGR scan. | `tests/test_ssg_app.cpp` | `noCapabilityReplyIsEverEmittedAsText`; `anUnboundedSequenceScanCannotHoldTheInputBuffer` (both fail until step 2) | INV-reply-never-input, INV-decode-terminates |
| 2 | Add `DecodeStatus::reply`; consume every recognised report shape whole, return `incomplete` while a terminator may still arrive, and cap every forward scan (reply and SGR mouse) at `kMaxSequenceBytes`. Do **not** change the exhausted-vs-incomplete contract. | `apps/ssg_terminal.h`, `apps/ssg_terminal.cpp` | step 1; `ordinaryInputIsUnaffectedByReplyRecognition`; `aReplySplitAcrossReadsIsStillConsumedWhole` | INV-reply-never-input, INV-decode-terminates |
| 3 | Add `TerminalCapabilities`: the query bytes, the reply parser, resolved answers, precedence and overrides, and the probe window that opens on write and closes on the DA1 reply or the backstop timeout. Absorb `detect_color_depth` as its first capability. Pure and unit-testable; no terminal required. | `apps/ssg_terminal.h`, `apps/ssg_terminal.cpp`, `tests/test_ssg_app.cpp` | `onlyADa1ReplyMeansTheFeatureIsAbsent`; `aReplyShapeAfterTheFenceIsNotACapabilityAnswer`; `anOverrideBeatsTheTerminalsOwnAnswer` | INV-capability-single-source |
| 4 | Write the queries at startup and route `DecodeStatus::reply` from the existing loop into `TerminalCapabilities`. Nothing waits. Verify with a scripted pty harness whose responder holds DA1 back for 500 ms: assert the first frame's bytes are observed on the pty *before* the responder has written a single byte, so the ordering is decided by the code and not by a race. | `apps/ssg_main.cpp`, `tests/test_ssg_app.cpp` | `theFirstFrameIsWrittenBeforeAnyReplyIsRead`; pty run: unchanged startup, typing, mouse, paste | INV-startup-unblocked |
| 5 | Add a diagnostic that reports what was detected (`--capabilities`, or a settings-screen line), so a user can see what SSG believes. | `apps/ssg_main.cpp` | pty run against kitty reports the keyboard protocol and synchronized output as present | - |
| 6 | Document the overrides and the detection model. | `doc/config.md`, `doc/spec.md` | doc gates green | - |

## Out of scope

- **Adopting any capability.** Styled underlines, synchronized output, OSC 52
  and the keyboard protocol each need their own spec. This one only makes them
  answerable.
- **Image capabilities.** `ESC [ 16 t` is listed for completeness; nothing asks
  it until an image feature exists, and
  `terminal-rendering-capabilities.html` argues that should be never.
- **WebSocket client capabilities**, which are negotiated in the protocol.
- **Caching**, settled above: slower than querying, and nothing durable to key.

## Rationale (optional, skippable)

The question that prompted this was whether capability detection blocks startup
and whether the result can be cached on the machine. Measuring answered both:
0.093 ms is too cheap to cache and too cheap to fear, and the only reason not to
block is that SSG happens to have an event loop that makes not blocking nearly
free.

The more interesting finding was the hazard. Tracing real replies through the
shipping decoder showed that every one of them would be typed into the user's
document, because an unrecognised CSI consumes three bytes and lets the rest
fall through to the printable path. That is a latent data-corruption bug sitting
directly in front of any capability work, and it reframes this spec: the
valuable part is not the query mechanism, which is twenty lines, but making the
decoder total over sequences it does not understand.

It is also a good argument for the seam existing at all. Without one central
owner, the first feature that wanted a capability would have sent its own query,
hit this, and either shipped the corruption or fixed it privately for one case.
