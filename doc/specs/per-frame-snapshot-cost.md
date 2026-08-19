# Per-frame snapshot cost

## Problem

Moving the cursor one line in the TUI rebuilds the entire screen model from
scratch. `EditorRuntime::snapshot(clientId, dimensions, paletteReport)`
unconditionally recomputes the shell layout, every semantic section, the
viewport, the prompt projection, and the tree windows, then assembles a fresh
`SessionSnapshot`; the host then lays out the full grid and encodes a full frame.
The host makes this worse by calling the snapshot builder once per buffered input
event in its input-drain loop, so a fast key-repeat multiplies the cost.

A profiler run on an optimized build, driving cursor navigation, attributes the
steady per-frame CPU (not I/O — file reads were measured to be a background-thread
red herring) to three CPU-bound sources, largest first:

- `visibleNodes` (in `TreeModel.cpp`) — recomputes the whole tree's visible-node
  list on every snapshot, building a fresh parent-to-children `std::map` and a
  recursive `std::function` walk each call. It is invoked from many section and
  window builders per frame.
- Grapheme layout — `GraphemeLayout::computeRun` reshapes every visible line of
  text each frame, including lines that did not change and were merely scrolled.
- Allocation churn — the map, vectors, and tree nodes rebuilt for each of the
  above, visible as `malloc`/`free`/`_Rb_tree_insert`/`memcmp` in the profile.

The cost is proportional to the whole tree plus the whole viewport, while the
actual per-keystroke delta is the cursor position and the viewport window. At a
realistic key-repeat rate an optimized build spends a modest fraction of a core;
the objection is that this is disproportionate to the work a cursor move
represents, and it saturates under bursts.

## The design choice

Three independent levers reduce this. Each has a workable cheaper alternative
whose consequence differs, which is why this is specced rather than transcribed.
They can land in separate phases and be reverted independently.

### Lever 1 — memoize tree visibility by revision

`visibleNodes(snapshot, expanded)` is a pure function of the provider snapshot's
node set and the expanded-id set. The provider snapshot already carries a
`TreeRevision` that changes only when the tree's structure changes (a refresh, a
provider swap) — never on cursor movement. The expanded set is mutated
independently (expand/collapse) and must participate in the key.

Approach: give the tree provider a cached `visibleNodes` result tagged with the
`(TreeRevision, expanded-set version)` it was computed for, and return the cache
when both still match. The expanded-set version is a monotonically increasing
counter bumped by the sole mutators of the expanded set, so the key is two
integers and a pointer-stable vector, not a content hash.

Alternative considered and rejected: hashing the expanded vector's contents each
call. It removes the need for a version counter but re-walks the vector every
frame, trading one cheap map build for one cheap hash — a smaller win and a
second source of truth for "did the expanded set change". A version counter owned
by the mutators makes the invalidation impossible to get wrong.

Consequence to hold: the cache must invalidate on *both* a new tree revision and
an expanded-set change; a cache keyed on revision alone would show stale
visibility after an expand/collapse, which is a correctness bug, not a perf
regression. This is the load-bearing invariant and belongs in a test named for
it.

### Lever 2 — per-line grapheme-layout cache

`computeRun` is stateless and constructed fresh (`GraphemeLayout{}`) at every call
site. Scrolling re-shapes unchanged lines because the render path shapes whatever
line text currently occupies each visible row. Document lines are shaped **twice**
per frame today, on two independent paths that must both be addressed or the cache
misses its goal: the library's `EditorRuntime::Impl::activeCellRuns` (feeding
`Viewport`) and the host's `Renderer::visibleLogicalLines`. The first has a
frame-surviving owner (the persistent `EditorRuntime::Impl`); the second is
reconstructed every frame because the host `Renderer` is per-frame.

Approach: the cache must live on a frame-surviving owner behind a **library-only
rendering seam** — it never crosses the cross-client wire. There are two distinct
shaping paths with different access patterns, and they need different cache
strategies:

- The visible-only render path. `Renderer::render` shapes only the viewport's rows
  (the existing `renderSegmentationCalls` seam already proves it segments at most
  viewport-row-many lines), and `Viewport::computeUnwrapped` is a *second*
  visible-only shaping pass in the default no-wrap mode. These two sites sit on
  opposite sides of the `snapshot()` boundary — `computeUnwrapped` is called inside
  `EditorRuntime::Impl` while `Renderer::render` is invoked by the host afterward —
  so they cannot share one cache without threading a borrowed cache through the hot
  `snapshot()` API. They are therefore two independently owned viewport-bounded
  `(text, tabWidth) -> CellRun` LRUs, each surviving frames: `EditorRuntime::Impl`
  owns the one `computeUnwrapped` consults, and the host owns the one it passes to
  `Renderer::render` as an optional parameter (omitting it preserves today's exact
  behavior). Each LRU is bounded to viewport rows plus a margin — the visible
  working set fits the bound — so the small duplication is two viewport-sized caches,
  not two document-sized ones.

- The wrap-mode whole-document path. `EditorRuntime::Impl::activeCellRuns` shapes
  *every* line of the active document to compute wrap positions, and runs only when
  word wrap is on. A viewport-sized LRU would thrash here — a full-document pass
  cyclically evicts earlier lines and misses again next frame. This path must be
  retained at *document scale*, keyed by the document revision: the shaped runs for
  a document are stable until an edit bumps its revision, so shape once per edit and
  reuse across every frame and scroll, invalidating wholesale when the document
  revision changes. This cache is owned by the persistent `EditorRuntime::Impl`.

A line's shaping is a pure function of its bytes and the tab width, so both caches'
keys are exact and neither needs semantic invalidation beyond the document-revision
bump that the wrap-mode cache uses to drop a stale generation. The LRU bound and the
document-revision key live in code, named, not in this prose.

Alternative considered and rejected: threading one shared cache through the
`snapshot()` API so `computeUnwrapped` and `Renderer::render` consult the same
instance. It removes the two-viewport-cache duplication but widens the hot snapshot
signature with a borrowed-cache parameter and couples the host's cache lifetime into
the runtime; two independently owned viewport-sized LRUs are cheaper to reason about
than that coupling.

Alternative considered and rejected: one viewport-sized LRU serving both the
visible and the wrap-mode whole-document paths. It thrashes on the wrap-mode
whole-document pass, delivering no hit there while adding eviction cost; the two
paths have different working-set sizes and need different strategies.

Alternative considered and rejected: publishing shaped `CellRun`s on
`SessionSnapshot`. Geometry is not a cross-client contract; putting runs on the
snapshot would change the protocol, duplicate document text/layout on every
snapshot, and raise delta and old-client-compatibility questions for a benefit a
purely internal rendering-path cache already delivers. Keep it off the wire.

Alternative considered and rejected: caching at the `GraphemeLayout` object by
making it stateful and threading one instance through every call site. That turns a
currently-pure, freely-constructed value type into a shared mutable cache with
lifetime and thread questions, and most measurement sites shape short one-off
strings that would pollute a line cache. Keep `GraphemeLayout` a pure value type and
cache at the two render/shaping seams above.

Consequence to hold: a cached `CellRun` for a given `(text, tabWidth)` must be
byte-identical to a fresh `computeRun`, a cached render must be byte-identical to an
uncached one, and the wrap-mode document-revision cache must drop its generation on
any edit; the grid goldens pin the render output, and a direct equivalence test plus
an invalidation-on-edit test name the caches.

### Lever 3 — coalesce the per-drain snapshot

The host's input-drain loop rebuilds a snapshot per event. It is tempting to say
keyboard events do not need a fresh snapshot, but that is wrong: `refresh()`
republishes host-owned routing state too — effective focus, the compiled keymap,
palette candidates, and picker mode — so a key that opens a prompt, changes focus,
or registers a command changes where the *next* buffered key must route. And
snapshot geometry is consumed by more than pointer events: a mouse-wheel event
(`DecodeStatus::scroll`) and a pointer event both hit-test against published
geometry. So the coalescing decision cannot be a static "keyboard vs pointer"
table.

Approach: two things must be separated. First, *effect production*: effects are
not annotated per command — they are accumulated at the runtime's mutation
chokepoints (where focus, mode, keymap, palette, or geometry actually change),
unioned across all nested and deferred work a single input triggers (the queued
follow-on dispatches, a command that fails after a partial mutation, and the
non-command routes — direct text entry, paste, prompt-value updates). But the
runtime chokepoints are not the whole story: the host keeps client-local
interaction state in `ssg_main.cpp` — the picker query, its selection and window,
and other state that feeds `buildReport`'s `PaletteReport` and pointer hit
geometry — and mutates it outside any runtime transaction. So the accumulator is a
*host+runtime union per input*: the runtime contributes the effects of its
chokepoint mutations, and the host contributes the effects of its own client-local
mutations (a changed picker query is a routing/geometry change even though no
runtime transaction ran). The runtime accumulator is owned by the transaction that
spans one input and exposed uniformly on the result of *every* runtime input route;
the host folds its own per-input effects into the same dirty accumulator the drain
carries. Manual per-command annotation is rejected: it permits false negatives (a
route, a deferred dispatch, or a host-local mutation nobody annotated) that leave
routing stale.

Second, *loop structure*: snapshot consumption and post-event mutation are separate
axes. A pointer or wheel event must hit-test against a *current* snapshot taken
before it is handled; its resulting dispatch may then dirty geometry or routing for
the *next* event. So the drain maintains a dirty accumulator and the order is:
before handling an event that consumes a snapshot, refresh if the accumulator is
dirty; handle the event; union the event's authoritative effects into the
accumulator. Skipping the intermediate refresh is then simply "the next
snapshot-consuming event finds the accumulator clean". This ordering cannot refresh
one event too late.

Alternative considered and rejected: deriving the gate from the global revision
delta. `CommandResult` reports only the session revision, not which routing or
geometry state changed, so this cannot distinguish a cursor move from a
focus-changing key without taking the snapshot — impossible for the thing whose
cost we are removing.

Alternative considered and rejected: a single boolean "did anything change" or a
per-command annotation table. Both conflate the two axes above or miss non-command
and deferred routes; the chokepoint accumulator plus the refresh-before-consume /
accumulate-after-handle ordering is what keeps every route correct.

Alternative considered and rejected: a blanket "one snapshot per drain for all
events". Simpler, but it hit-tests pointer and wheel events against a snapshot that
predates an earlier event's mutation in the same drain, and routes a key through
pre-mutation focus/mode.

The effects must be mapped from every `DecodeStatus` kind the host decodes —
`none`, `incomplete`, `key`, `scroll` (mouse wheel, which hit-tests geometry),
`pointer`, `paste`, `reply` — and that mapping exhaustively tested, so no kind is
silently mis-coalesced.

Consequence to hold: any event in a drain that consumes geometry or routing state
sees a snapshot reflecting every earlier event in that same drain, and every input
route — command, deferred dispatch, direct text, paste, prompt update, a
partially-applied failed command — contributes its true effects to the
accumulator. This is a seam rule between the host loop and the runtime's
dispatch-effects chokepoint, and belongs in a host-level test named for it.

## Plan

Land the levers in three commits, each independently revertable, each with its own
oracle, gated by `scripts/check.sh push` with grid goldens byte-identical:

1. Lever 1 (library, `TreeModel`): add the revision+expanded-version-keyed cache
   to the tree provider; the mutators of the expanded set bump the version. Oracle:
   a test that visibility is recomputed after a tree-revision change and after an
   expand/collapse, and is served from cache when neither changed (observable via a
   recompute counter behind a test seam, not timing).

2. Lever 2 (library rendering seam): add a viewport-bounded LRU
   `(text, tabWidth) -> CellRun` cache in two independently owned places — one owned
   by `EditorRuntime::Impl` and consulted by `Viewport::computeUnwrapped`, one owned
   by the host and passed to `Renderer::render` as an optional parameter — plus a
   document-revision-keyed whole-document run cache owned by `EditorRuntime::Impl`
   for the wrap-mode `activeCellRuns` path. Nothing new crosses the wire. Oracle:
   cached and fresh `computeRun` agree byte-for-byte across lines with tabs, wide
   graphemes, and combining sequences; a cached render equals an uncached render;
   each visible-line path (unwrapped viewport and `Renderer`) reuses its own cache
   on a scroll over unchanged lines (a segmentation-count oracle per path); the
   wrap-mode cache is dropped on an edit (invalidation test); goldens unchanged.

3. Lever 3 (host+runtime effect union + host loop): add a dispatch-effects
   accumulator owned by the runtime transaction spanning one input, unioning effects
   (routing-changed, geometry-changed) across nested/deferred dispatches and every
   runtime input route, exposed on every route's result (not only `CommandResult`);
   the host folds its own client-local mutation effects (picker query/selection/
   window) into the same accumulator. The host drain maintains a dirty accumulator:
   refresh before a snapshot-consuming event when dirty, then union that event's
   host+runtime effects after handling. Oracle: a library test that a command, a
   deferred follow-on dispatch, a paste, a prompt update, and a partially-applied
   failed command each contribute their true effects; a host test that a client-local
   picker-query change registers as an effect; an exhaustive `DecodeStatus`-kind-to-
   effects mapping test; a host-level test that a burst of pure cursor keys yields
   one snapshot build, that a key opening a prompt forces a re-snapshot so the next
   key routes to the prompt, and that a wheel/pointer event mid-burst hit-tests
   post-mutation geometry; plus a measured before/after CPU note in the commit
   message.

Sequence rationale: Lever 1 is the largest single share and is contained to one
file with a clean existing revision key, so it goes first. Lever 2 is broader
(many call sites shape lines) but semantically simplest. Lever 3 changes host loop
structure and depends on nothing above, so it can land whenever, but is placed
last because its win overlaps Levers 1–2 (fewer, cheaper snapshots).

## Durable residue (promote, then delete this spec)

- Lever 1 invalidation invariant → a test named for "visibility recomputes on tree
  revision change AND on expand/collapse, and is otherwise cached".
- Lever 2 equivalence → a test named for "cached line layout equals fresh
  computeRun", one named for "the wrap-mode run cache drops its generation on an
  edit", and the existing `renderSegmentationCalls` counter guarding "a scroll over
  unchanged lines does not re-segment them"; the grid goldens guard the render
  output. The optional-cache parameter's default (no cache = today's behavior) is a
  type-level guarantee, not a prose one.
- Lever 3 dispatch-effects → the accumulator and its unioned effects value are the
  contract (a type produced at the mutation chokepoint, not prose); tests name "every
  input route — command, deferred dispatch, direct text, paste, prompt update,
  partially-applied failure — contributes its true effects", the exhaustive
  `DecodeStatus`-to-effects mapping, and the host seam "an event that consumes
  geometry or routing in a drain sees the post-mutation snapshot". A `// CONTRACT`
  line only if the chokepoint's union-across-deferred-work rule outlives those names.
- No numeric budgets in prose; the cache bound and any interval live in code, named.

Nothing else about this spec survives merge. Recover it from git history by slug
if a later change needs the rejected alternatives.
