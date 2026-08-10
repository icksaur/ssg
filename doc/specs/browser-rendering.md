# Browser rendering architecture

Status: draft, in review. This file exists only until the decision it records
lands; on merge its residue becomes types, tests, and CONTRACT lines, and the
file is deleted in the same commit.

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

The one open fork is **where the visible lines' cell geometry comes from**:

- **A′ — server publishes visible cell runs.** Extend the snapshot with a
  viewport-bounded section: for each visible logical line, the `CellRun` the
  library already computes (grapheme → cell-column spans) plus each span's syntax
  scope. All layout stays in C++; the wire stays semantic; no client-side layout
  code ships.
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
framing was wrong — B's cost is the projection boundary. The review must weigh
that larger B against A′'s placeholders; the recommendation is deferred to the
prototype-and-price step below rather than asserted here.

## Consequences either way

- The overflow-scroller and client-owned offset for editor and tree are new
  regardless of A′/B. The reconciliation contract — client owns the pixel offset,
  server owns `firstVisualRow`, the client nudges via `scroll_to_fraction` — is
  the load-bearing promise and wants a test. It is incomplete without an explicit
  arbitration rule for a server-initiated reveal that lands mid-fling: otherwise
  a reveal publishes a new `firstVisualRow`, then the browser's next throttled,
  now-stale `scroll_to_fraction` immediately overwrites it, snapping the view
  back. The rule must be pinned, not left implicit. Candidate: every scroll nudge
  carries the snapshot revision it was computed against, and the server ignores a
  nudge whose basis predates its last authoritative reveal (an epoch check), so a
  reveal wins and the browser re-derives its offset from the revealed
  `firstVisualRow` on the next snapshot. The alternative — browser input suppresses
  reveal until the fling goes idle — is worse: it lets a client's momentum defeat
  a semantic reveal (e.g. jump-to-match). Whichever is chosen, it is a seam rule
  with a knowable answer and wants a seam oracle named for it.
- Off-screen rows the client paints itself (B) must reproduce the server's cell
  geometry exactly; that is only safe because it is the same code, and is the
  reason B compiles `GraphemeLayout` rather than reimplementing it. A JS
  reimplementation is out of scope: it would create a second geometry authority.
- Chrome (tabs, tree, palette, prompt) rendering as native widgets vs staying on
  the grid, and how caret/IME input maps back to byte offsets through cell
  geometry, are deferred to the implementation slice and are not part of this
  decision.

## Open questions for review

1. A′ vs B — the fork above, now correctly priced: A′'s fling placeholders and
   wider per-client render band vs fully-scoped B's WASM projection boundary
   (`GraphemeLayout` + `Viewport` projection + diff phantom-row source).
2. If B: the exact export surface of the projection boundary across the WASM
   boundary, and the build cost (Emscripten toolchain, module size, load path).
   Confirm the boundary is a clean, side-effect-free unit before assuming it
   compiles.
3. If A′: the overscan band policy and the placeholder-during-fling behavior, and
   the per-client server render cost of a wider band.
4. The scroll-reconciliation contract's exact shape: the throttle, and the
   reveal-vs-fling arbitration (the epoch/revision-basis rule above), pinned by a
   seam oracle so a red bar means the arbitration promise broke.

## Plan

1. Prototype the overflow-scroller virtualization against a live
   `SessionSnapshot` to measure fling behavior under A′ (placeholder frequency,
   band width needed).
2. Scope and price B honestly: the export surface of the whole projection
   boundary (`GraphemeLayout` + `Viewport` visual-row projection + diff
   phantom-row source), module size, and load — not just grapheme segmentation.
3. Decide A′ vs B from 1–2, record it here, then spec the chosen client's first
   runnable slice (read-only: render one visible screen, scroll it natively).
4. On implementation, promote the reconciliation contract to a test and any
   deliberate refusal to a CONTRACT line; delete this file in that commit.
