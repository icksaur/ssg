# Browser rendering architecture

Status: A′ ratified, fork closed. This file exists only until the first runnable
slice lands; on merge its residue becomes types, tests, and CONTRACT lines (see
the plan), and the file is deleted in the same commit.

## The decision

A browser client consumes the same semantic `SessionSnapshot`/`SessionDelta` the
in-process and TUI hosts do — the wire carries the whole `document.text`,
`SyntaxSpan` ranges, selection byte offsets, an sRGB `ThemeSnapshot`, `Style`
dimensions, and a cell-geometry `ViewportViewState`. It never carries a
`CellGrid`: `ssg::Renderer` runs host-side. The library remains the sole
authority for geometry — grapheme cell widths (UAX #11 via
`GraphemeLayout::computeRun` → `CellRun`), tab stops, wrap points, horizontal
scroll snapping, and `firstVisualRow`. A browser may restyle cells for
readability but must not re-derive where a glyph sits.

Given that, the browser paints per-line native text runs snapped to the
authoritative cell grid — not per-character DOM (unscalable) and not a blitted
cell bitmap (discards web type, scroll, and input). Selection, caret, diff
tints, and syntax colors are overlay layers driven by byte offsets mapped
through cell geometry.

The one open fork is **where the visible lines' cell geometry comes from**. It is
now **closed in favor of A′** (see the measured evidence below); B is retained
only as the fallback for a hard fully-offline requirement.

- **A′ — server publishes visible cell runs (CHOSEN).** Extend the snapshot with
  a viewport-bounded section: for each visible logical line, the `CellRun` the
  library already computes (grapheme → cell-column spans), run-length packed. All
  layout stays in C++; the wire stays semantic; no client-side layout code ships.
- **B — WASM the layout core.** Compile the layout code to WebAssembly so the
  browser, which holds the whole `document.text`, lays out any line locally
  against the server's policy. The geometry code is literally the same code, so
  the two clients cannot diverge, and no protocol change is needed. Scope caveat
  (load-bearing): `GraphemeLayout` alone is not enough — it deliberately excludes
  wrapping, visual-row projection, scrollbars, and viewports. To derive arbitrary
  visual rows, `totalVisualRows`, wrap boundaries, and diff phantom rows from
  `document.text`, B must also compile the shared projection boundary that owns
  them (`Viewport` visual-row projection over `CellRun`s, and the diff phantom-row
  source). B's real cost is that boundary, not one module; the decision must price
  it as such.

Both honor single-geometry-authority. The choice is decided by scroll, below.

## The deciding factor: touch scroll, native scrollbars, and the fling

The cell model, painted naively, loses two things browsers give for free: native
scrollbars and momentum/touch scrolling. Both return by placing the virtualized
cell layer inside a real browser overflow container whose scroll height is the
published full extent (`totalVisualRows × rowHeight`), rendering only visible
rows and translating them to the scroll position. The platform then supplies the
scrollbar, trackpad/touch momentum, rubber-band, and sub-row smoothness; the
client implements no inertia.

Scroll offset today is server-authoritative and row-quantized (`firstVisualRow`),
driven by `view.scroll_to_fraction` / `tree.scroll_to_fraction`. The palette
already scrolls client-side with no round-trip (`ClientScroll`, reconciled on the
next snapshot). The browser generalizes that precedent to the editor and tree:
the client owns a pixel offset locally, and on scroll maps the top pixel to a
target `firstVisualRow`, nudging the server (throttled) via `scroll_to_fraction`.
Momentum runs entirely client-side; the authoritative row still lands on the
server, so reveal and cross-client consistency hold. The tree is the easy case —
fixed-height rows, no wrap.

Input plumbing needs no protocol change: `route_pointer`/`route_wheel` are pure
functions; the browser adds gesture recognition on top, feeding either the new
client-owned offset (scroll) or the existing commands (taps, drags).

**The fling is what decides A′ vs B.** A fast fling scrolls faster than snapshots
arrive, past rows whose geometry the client may not have.

- Under **A′**, the client only has cell runs for the published window. A fling
  outruns the data, so A′ must publish an overscan band above and below the
  viewport and show placeholder rows during a fling until the next snapshot fills
  them — the standard native-virtualized-list behavior, but with momentary blanks
  on large flings, and a server that renders a wider band per scroll for every
  client.
- Under **B**, the client projects any line locally from `document.text` and so
  never outruns its data — *provided* B compiled the whole projection boundary
  (`GraphemeLayout` plus `Viewport` visual-row projection plus the diff
  phantom-row source), not just grapheme segmentation. Compiling only
  `GraphemeLayout` leaves the client unable to derive visual rows or wrap
  boundaries, and a fling outruns geometry exactly as under A′. The fling
  advantage is real only for the fully-scoped B.

So the fork is genuinely: A′ ships no client layout and accepts fling
placeholders plus a wider server render band per client; fully-scoped B ships and
loads a WASM projection boundary and gets placeholder-free native momentum with
no per-scroll server work. The earlier "B is cheap, just `GraphemeLayout`"
framing was wrong — B's cost is the projection boundary.

**Prototype finding (A′ fling, measured by kinematic simulation).** A native
fling travels a bounded distance — a few screens at most — and its landing point
is computable at flick release from velocity and the friction time-constant. A
client that requests the whole fling *envelope* up front (predictive banding),
rather than chasing the scroll with throttled requests, is placeholder-free
across the whole tested latency range up to wide-area RTT, at an overscan under
one screen. Gentle and medium flings never placeholder under either strategy;
only a hard fling under naive chase at wide-area RTT shows a small fraction of
placeholder frames, and predictive banding closes even that. Per-fling bandwidth
is the envelope sent once and delta-reducible against rows already held — modest
on any broadband link. The one case predictive banding cannot help is a
scrollbar random *seek* (not a fling): it shows a single-RTT blank under A′ or
any server-authoritative model, exactly as every virtualized remote list does.

This substantially retires the fling risk that was A′'s main liability, and with
it most of B's justification for momentum scrolling.

**Bandwidth is now measured, not modeled.** The naive form of the A′ cell-run
section — one span per grapheme — is tens to hundreds of bytes per row and is the
trap to avoid. Run-length packing collapses it to a couple of bytes per row for
code, a handful for tab-indented or wide/CJK text, because a line's cell geometry
is a few uniform runs (exceptions-only packing is smaller still for ASCII but
degenerates on all-wide text, so the encoder run-length-packs, or picks the
smaller of the two per line). A hard-fling envelope therefore costs on the order
of a kilobyte, sent once and delta-reducible — negligible on any link. Syntax is
not an A′ cost: whole-document spans already ride the semantic snapshot for every
client, and the browser intersects the spans it holds with the visible window (a
range query, not geometry re-derivation); folding resolved scope into the cell
section is an option, not a requirement.

With both the fling-placeholder risk and the bandwidth cost retired by
measurement, **the fork is closed in favor of A′** — no WASM toolchain,
self-describing wire, simpler client. The standing reviewer ratified A′ and
confirmed a B WASM size/load measurement is not required before choosing. B
remains the fallback only for a hard fully-offline requirement, which is not a
current product goal.

## Input latency: local echo decouples typing from RTT

Spikes 0–3 built `ssg --http PORT` in the same binary and drove real keystrokes
through `EditorRuntime::dispatch` from a browser. Two measured facts settle the
input model:

- The edit itself is trivially cheap (`dispatch` ~0.1 ms); the round-trip cost is
  the snapshot/delta ship, which on loopback is ~10 ms and on a tunnel is the
  network round trip.
- Per-stroke *blocking* — showing a character only when its server frame returns
  — makes perceived typing latency equal to the round trip: measured ~70 ms at
  40 ms one-way and ~150 ms at 80 ms one-way. A tunnel is unusable this way.

The resolution is **client-side local echo with server reconciliation**: the
client renders a predicted keystroke immediately and reconciles against the
authoritative snapshot/delta when it arrives. Measured, this decouples perceived
typing latency from RTT entirely (~0 ms at every latency) while the final text
stays correct. The server remains the sole authority — it overwrites the
prediction on reconciliation; the client's prediction is transient and visual,
the same shape as the scroll offset (client owns a transient value, server owns
the authoritative one).

This is bounded by what the client can reproduce exactly. Prediction is
permitted only for operations whose text effect the client can compute
identically to the library — plain insertion and deletion at known caret
positions. Anything the client cannot reproduce (autoindent, multi-cursor
transforms, a command whose text effect is not local) falls back to blocking on
the server, which is acceptable because it is rare relative to typing. A
prediction the client cannot reproduce exactly would flicker or diverge, so the
boundary is a **CONTRACT on merge**: a client may predict only operations it can
reproduce byte-for-byte from the library's own rules, every prediction is
reconciled against and overridden by the authoritative snapshot/delta, and the
client is never a second editor. Reconciliation needs the snapshot/delta to carry
a per-client applied-sequence so the client drops acknowledged predictions and
rebases the unacknowledged ones; a seam oracle pins that a reconciled document
equals the server's exactly and that an unreproducible operation blocks rather
than mispredicts.

## Consequences either way

- The overflow-scroller and client-owned offset for editor and tree are new
  regardless of A′/B. The reconciliation contract — client owns the pixel offset,
  server owns `firstVisualRow`, the client nudges via `scroll_to_fraction` — is
  the load-bearing promise and wants a test. It is incomplete without an explicit
  arbitration rule for a server-initiated reveal that lands mid-fling: otherwise
  a reveal publishes a new `firstVisualRow`, then the browser's next throttled
  `scroll_to_fraction` immediately overwrites it, snapping the view back. The rule
  has two halves and needs both:
  - **Server half:** every scroll nudge carries the snapshot revision it was
    computed against, and the server ignores a nudge whose basis predates its
    last authoritative reveal (an epoch check). This discards nudges already
    in flight when the reveal happened.
  - **Client half:** applying a reveal snapshot cancels the local fling
    generation — it invalidates the current momentum, discards that fling's
    pending scroll events, and re-derives the offset from the revealed
    `firstVisualRow`, emitting no further nudge until new physical input begins a
    fresh fling. Without this, momentum that continues past the reveal would emit
    a nudge stamped with the *new* revision and immediately override the reveal —
    the epoch check alone does not stop it, because that nudge is not stale.
  The alternative — browser input suppresses reveal until the fling goes idle — is
  worse: it lets a client's momentum defeat a semantic reveal (e.g.
  jump-to-match). The chosen rule is a seam rule with a knowable answer; the seam
  oracle must pin both halves, including the client's fling-generation cancel and
  scroll-event suppression across the reveal, so a red bar means the arbitration
  promise broke.
- Off-screen rows the client paints itself (B) must reproduce the server's cell
  geometry exactly; that is only safe because it is the same code, and is the
  reason B compiles `GraphemeLayout` rather than reimplementing it. A JS
  reimplementation is out of scope: it would create a second geometry authority.
- The native scroll *mechanism* and the native scroll *widget* are different
  questions, and only the first is settled by the fork. Native momentum, touch,
  rubber-band, and sub-row smoothness change only *how a row is reached*, not
  which cells the server describes, so they are unambiguously permitted. A
  browser-native **scrollbar**, however, is not free chrome: the library already
  owns the editor/panel/palette scrollbars — `ScrollbarMetrics`
  (`totalRows`, `viewportRows`, `firstRow`, `maximumFirstRow`, `thumbStart`,
  `thumbSize`) ships in the viewport snapshot and the TUI paints it in a reserved
  gutter column. A client-invented scrollbar with its own extent or thumb would be
  a second geometry authority, the same violation the fork forbids for WASM
  layout.
  The resolution is affordance vs authority: a browser-native scrollbar is
  permitted **only as a readability restyle and input affordance of the server's
  scrollbar** — its track extent is the published `totalRows`/`maximumFirstRow`
  and its thumb is the published `thumbStart`/`thumbSize`, and dragging it drives
  the same `scroll_to_fraction` reconciliation as every other scroll input. It
  introduces no independent extent, no independent thumb geometry, and no scroll
  path that bypasses `firstVisualRow` reconciliation. Skinning the platform
  scrollbar in place of painting the gutter column is the one geometry change the
  rule blesses here — the reserved gutter column may be dropped on the browser
  client because the platform chrome replaces it at the same track. This is a
  deliberate refusal (no second scrollbar) that will read like a missing feature
  to someone who expects a free browser scrollbar, so it becomes a **CONTRACT line
  on merge**: a client's scrollbar is a restyle of the published `ScrollbarMetrics`,
  never a second one, and never an independent scroll authority. The seam oracle
  for reconciliation covers the drive path; the CONTRACT covers the refusal a test
  cannot state.
- The remaining chrome — tabs, tree, palette, prompt — rendering as native
  widgets vs staying on the grid, and how caret/IME input maps back to byte
  offsets through cell geometry, are deferred to the implementation slice. Each
  is governed by the same affordance-vs-authority test the scrollbar just
  resolved: a native widget is permitted only as a restyle of server-published
  geometry, never as an element with independent geometry or an independent
  authority path.

## Resolved and remaining

1. **A′ vs B — resolved: A′.** Ratified by review; both liabilities retired by
   measurement. B is the fallback for a hard fully-offline requirement only, and
   needs no size/load measurement unless that requirement appears.
2. The scroll-reconciliation contract's exact shape: the throttle value, and the
   reveal-vs-fling arbitration (server epoch check **and** client fling-generation
   cancel, above), pinned by a seam oracle so a red bar means the arbitration
   promise broke.
3. The A′ overscan band width (sub-screen, from the fling prototype) and the
   RLE cell-run encoder's format and worst case.
4. **Scrollbar — resolved:** a browser-native scrollbar is permitted only as a
   restyle/affordance of the published `ScrollbarMetrics`, never a second scroll
   authority (a CONTRACT on merge). The remaining chrome — tabs, tree, palette,
   prompt — as native widgets vs on the grid, and caret/IME input mapping back to
   byte offsets, are deferred to the implementation slice under the same
   affordance-vs-authority test.

## Plan

The fork is closed (A′). Remaining steps deliver it.

1. Design the A′ cell-run wire section: RLE-packed, per visible line plus a
   sub-screen overscan band, with an encoder oracle (round-trip and worst-case
   size). Predictive banding — request the fling envelope up front — is a required
   element of the client, not an optimization.
2. Build the first runnable slice: a read-only browser client that renders one
   visible screen from a live `SessionSnapshot` as per-line native text runs
   snapped to the cell grid, scrolled natively in an overflow container sized to
   `totalVisualRows`.
3. Add the client-owned pixel offset with server reconciliation (`firstVisualRow`
   nudge via `scroll_to_fraction`) and the reveal-vs-fling arbitration (server
   epoch check + client fling-generation cancel).
4. On implementation, promote the residue and delete this file in that commit:
   - **Tests (seam oracles):** the reveal-vs-fling arbitration (both halves); the
     RLE cell-run encoder round-trip and worst-case bound; the client-owned
     offset ↔ `firstVisualRow` reconciliation; and the local-echo reconciliation
     (a reconciled document equals the server's exactly, and an unreproducible
     operation blocks rather than mispredicts).
   - **CONTRACT lines:** that the library remains the sole geometry authority and
     a client (including any future WASM one) reproduces cell geometry only from
     library code, never a reimplementation — a second geometry authority is a
     deliberate refusal; that the cell-run section is RLE-packed, never one span
     per grapheme; that a client's scrollbar is a restyle of the published
     `ScrollbarMetrics` — same extent and thumb, driving the same
     `scroll_to_fraction` reconciliation — never a second scrollbar with
     independent geometry or an independent scroll authority; and that a client
     may locally predict only operations it can reproduce byte-for-byte from the
     library's rules, every prediction reconciled against and overridden by the
     authoritative snapshot/delta — the client is never a second editor.
   - Everything else — the A′-vs-B history, the measurements, this exposition —
     stays in git, not the tree.
