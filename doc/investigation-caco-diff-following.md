# Investigation: caco "files" Applet — Diff Following

*How caco implements agent-generated diff following for SSG editor design reference.*
*No caco files were modified. All citations are read-only observations.*

---

## Files / Location

**Frontend — `../caco/applets/files/`**
- `meta.json` — manifest (`slug: "files"`, name "Files")
- `script.js` — 3970-line orchestrator: tab strip, `followEdits` state machine, diff rendering, selection bridge
- `diff-viewer.js` — `DiffViewer` ViewerInstance: git diff rendering, scroll guard, `_prevHunkWorkRanges`
- `content.html` — DOM shell (`feFollow`, `feTabs`, `fePane`, ...)
- `style.css` — all `.fe-*` styling

**Backend — `../caco/src/`**
- `git-edit-poller.ts` — `GitEditPoller`: diffs working tree vs HEAD, emits `caco.edit` events
- `file-watcher.ts` — chokidar watcher → `triggerPoll('fs-event')`
- `file-edits-store.ts` — per-session persisted `CardList` (schema v2)
- `routes/file-edits.ts` — REST endpoints under `/api/sessions/:sessionId/file-edits`

**Design docs — `../caco/docs/`**
- `spec-files-applet-edits.md`, `spec-files-applet-cards.md`, `spec-files-applet-viewers.md`, `files-applet-roadmap.md`

---

## How Diffs Arrive

### Server-side: `GitEditPoller` (`git-edit-poller.ts`)

The poller diffs working tree vs git HEAD per-session and fires on three triggers (git-edit-poller.ts:477):
- **`fs-event`**: chokidar debounced ~300ms after any file write (file-watcher.ts:2–8)
- **Timer backstop**: `WATCHED_BACKSTOP_MS = 30_000` while watching; `IDLE_CADENCE_MS` (~5s) otherwise (git-edit-poller.ts:87–90)
- **`event`**: explicit `triggerPoll` call

Each poll builds `EditEntry[]` and broadcasts over WebSocket (git-edit-poller.ts:519–523):
```ts
broadcastEvent(sessionId, {
  type: 'caco.edit',
  data: { edits, cleared, cleanedEdits, pollSource: source },
});
```
- `edits` = dirty `EditEntry[]`
- `cleared` = paths that went dirty → clean
- `cleanedEdits` = synthetic `clean` `EditEntry` (full HEAD content) so the UI re-renders immediately on cleanup

### Data structures (`spec-files-applet-edits.md:44–57`)

```
EditEntry  { path, relativePath, status, diff?, fullFile?, isBinary?,
             mtimeMs?, renamedFrom?, truncated?, timestamp }
FileStatus = 'modified' | 'untracked' | 'deleted' | 'renamed' | 'clean'
FullFile   { headLines: string[]|null, workLines: string[], hunks: DiffHunk[] }
DiffHunk   { headStart, headLen, workStart, workLen }  // 1-indexed
```

`fullFile` is the primary V2 payload. Large, binary, deleted, or staged files fall back to unified `diff` text.

### Client-side: subscription (`script.js:3858–3872`)

```js
if (event.type === 'caco.edit' && event.data) {
  var all = [].concat(d.edits || [], d.cleanedEdits || []);
  all.forEach(function(e, i) {
    openOrUpdateTab(e, { suppressFollowSelect: i < all.length - 1 });
  });
}
```

**Only the last edit in a burst triggers active-tab selection** — prevents tab flicker on multi-file writes.

Initial state comes from REST (`GET /file-edits/snapshot`) on attach/session-change (script.js:3592–3618). Per-tab updates call `DiffViewer.update(newEdit)` (diff-viewer.js:96–110), which saves `_prevHunkWorkRanges` before replacing content.

---

## Following Behavior

### Core model (script.js:5–9, 27)

> A `followEdits` boolean decides whether incoming edits auto-switch tabs; user gestures turn it off; the "Follow edits" button turns it back on and jumps to the most recent edit.

```js
var followEdits = true;          // script.js:27 — default ON
var lastEditedTabId = null;      // script.js:36-38 — most recent content-changing edit
var badgeCounter = new Set();    // script.js:39-41 — paths changed while follow is OFF
```

### Auto-follow on live edit (`openOrUpdateTab`, script.js:1776–1799)

When `followEdits` is on and it's the last edit of the batch:
```js
setActiveTab(container.id);
if (contentChanged) {
  requestAnimationFrame(() => requestAnimationFrame(() => {
    scrollPaneToFirstDiffRow(container.id);
  }));
}
```
New tabs scroll to top first. Two rAFs let `activate()` swap content and restore scroll state before positioning to the hunk.

When `followEdits` is off, the changed path is added to `badgeCounter` and the button badge increments (script.js:1800–1805).

### Turning follow OFF — user gestures

- **Real user scroll**: `DiffViewer._installScrollHandler` (diff-viewer.js:47–75) sets `followEdits = false`. Programmatic scrolls are protected by `consumeProgrammaticScroll` (diff-viewer.js:53) and a `_renderTick` suppression window — two rAFs around innerHTML replacement — so re-renders don't falsely trigger the handler (diff-viewer.js:64–90).
- **Selection null-out**: script.js:1314–1316, 1357–1359
- **Tab click / file picker**: callers flip `followEdits = false` directly before calling `setActiveTab` (which does NOT touch it — script.js:1564–1566).

### Turning follow back ON — the button (script.js:2071–2076)

```js
followBtn.addEventListener('click', function() {
  followEdits = true;
  badgeCounter.clear();
  jumpToMostRecent();
  updateFollowButton();
});
```

### `jumpToMostRecent()` — on-demand jump (script.js:1943–1981)

Target selection priority:
1. `lastEditedTabId` if it's a diff tab and still dirty — script.js:1949–1951
2. Diff tab with highest `mtimeMs` — script.js:1953–1959
3. Last-in-insertion-order dirty diff tab — script.js:1960–1968

Then `setActiveTab(targetId)` + double-rAF `scrollPaneToFirstDiffRow`. Markdown and clean tabs are skipped.

### `scrollPaneToFirstDiffRow()` — cursor positioning (script.js:1995–2031)

1. Queries `.fe-row-add, .fe-row-del` rows in the viewer (script.js:2004).
2. **Prefers a NEW row**: picks the first whose `data-work-line` is NOT in `_prevHunkWorkRanges` via `v._wasInPrevHunks(line)` (script.js:2005–2013, diff-viewer.js:126–135). This ensures a freshly-added bottom hunk is scrolled to, not the topmost pre-existing hunk.
3. Falls back to first add/del row, then scroll-to-top.
4. **Centering at 30%** from top: `target = offsetWithinPane - scrollEl.clientHeight * 0.3` (script.js:2026–2027).

**Summary:** caco does both — live auto-follow when the toggle is on, and an on-demand jump when the user has scrolled away. There is no explicit next/previous-change stepper; "follow" always jumps to the freshest change.

---

## Visual Representation

### Primary mode: full-file inline unified diff (`renderFullFile`, script.js:3498–3527)

Each line is a `.fe-row fe-row-<kind>` CSS-grid row (kind ∈ `add` / `del` / `ctx` / `fold` / `collapse`):
```html
<div class="fe-row fe-row-add" data-work-line="42">
  <span class="fe-gutter fe-gutter-head">…</span>   <!-- head line-number gutter -->
  <span class="fe-gutter fe-gutter-work">42</span>  <!-- work line-number gutter -->
  <code class="fe-line hljs">…</code>               <!-- syntax-highlighted content -->
</div>
```

- **Layout**: `grid-template-columns: gutter gutter content` — dual line-number gutters, inline unified (NOT split side-by-side). `data-clean-only="true"` collapses to one gutter (style.css:199, 209–213).
- **Row color** (style.css:282–295): added rows tinted green (`color-mix(success-bright 18%)`), deleted rows tinted red (`color-mix(error 18%)`).
- **Word-level marks** (`mark.fe-w-add` / `mark.fe-w-del`, style.css:317–338): ~45–50% saturation on changed tokens only, computed by `computeAllWordMarks` (script.js:3503).
- **Selection highlight**: `.fe-row-selected` class painted by `DiffViewer.paintSelection` (diff-viewer.js:147–161).
- **Syntax highlighting**: highlight.js applied per row (script.js:3425–3430).

### Fallback mode: unified patch text (`renderDiff`, script.js:3529–3554)

Plain `<span>` blocks with classes `fe-d-add` (green), `fe-d-del` (red), `fe-d-ctx` (dimmed 0.6), `fe-d-hunk` (accent `@@` headers). Used when `fullFile` is absent (large/binary/deleted files).

---

## Multi-File Navigation

- **`lastEditedTabId`**: tracks the last content-changing edit (script.js:38, set at 1721 and 1763). Primary `jumpToMostRecent` target (script.js:1949).
- **Tab strip**: each changed file is a tab in `tabs = new Map()` (script.js:35). Map insertion order = tab order; **tabs never reorder** — ordering is by first-open, not by recency.
- **Burst handling**: `suppressFollowSelect` on all-but-last ensures a multi-file burst lands on a single tab (the most recent), not each file in sequence (script.js:3861–3870, 1779–1783).
- **No next/previous stepper**: `jumpToMostRecent` picks among dirty diff tabs by `lastEditedTabId → highest mtimeMs → last insertion order`. Clean tabs are excluded.
- **Dismiss semantics**: `dismissedPaths` / `dismissedSnapshots` prevent redundant poller re-broadcasts from reopening user-closed tabs; genuine new edits can still reopen them (script.js:42–56).

---

## Data / Protocol Shapes

### WebSocket push event (`git-edit-poller.ts:519–523`)
```
type: 'caco.edit'
data: {
  edits:        EditEntry[]   // dirty paths
  cleared:      string[]      // paths that went clean
  cleanedEdits: EditEntry[]   // synthetic clean entries for immediate re-render
  pollSource:   'timer' | 'event' | 'fs-event'
}
```
Delivered via `appletAPI.onSessionEvent` (script.js:3858).

### REST API (`routes/file-edits.ts`, mounted `/api/sessions/:sessionId/file-edits`)
| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/snapshot` | Initial `{ edits, isGit }` on attach |
| `POST` | `/open` | `{ relativePath, diffMode? }` → `{ edit }` — user-picked file |
| `GET` | `/cards` | Persisted `CardList` |
| `PUT` | `/cards` | `{ schemaVersion, cards, dismissed }` — debounced persist + `sendBeacon` |

### Core types
```ts
EditEntry  { path, relativePath, status, diff?, fullFile?, isBinary?,
             mtimeMs?, renamedFrom?, truncated?, timestamp }
FullFile   { headLines: string[]|null, workLines: string[], hunks: DiffHunk[] }
DiffHunk   { headStart, headLen, workStart, workLen }  // 1-indexed line numbers

CardList   { schemaVersion: 2, updatedAt, cards: CardPersist[], dismissed: string[] }
CardPersist { relativePath, defaultViewerType?, activeViewerType?, diffMode?, collapsed? }
```

### Bidirectional selection bridge
Rows carry `data-work-line` attributes. User drag → `envelopeFromRange()` → `{ start, end, text }` (text capped 4096) echoed to the agent as `fileEdits.selection`. Agent-pushed `{ start, end }` → `applyEnvelopeAsRange()`, guarded by `_expectedEnvelope` (prevents re-echo) and `sourceId` (prevents peer-client focus theft). (spec-files-applet-edits.md:71–79; diff-viewer.js:147–161, 206–213)

---

## Lessons for SSG

1. **`followEdits` boolean is the entire state machine.** Live edits auto-follow when on; real user scroll flips it off; one button re-enables and calls the same jump function. Keep it this simple.

2. **Track `_prevHunkWorkRanges` before every re-render.** This lets "scroll to first diff row" target the *newest* change rather than the topmost pre-existing hunk — the difference between the feature feeling right vs. frustrating on multi-hunk files (diff-viewer.js:101–135, script.js:2003–2013).

3. **Guard programmatic scrolls explicitly.** Use a consume-once flag AND a render-tick suppression window (two rAFs around any innerHTML replacement). Without both guards, re-renders fire false `scroll` events that prematurely disable follow (diff-viewer.js:53–90).

4. **Batch multi-file bursts: `suppressFollowSelect` on all but the last.** Only the final edit in a burst activates a tab and triggers scroll. This prevents visible tab-flickering when an agent writes several files in quick succession (script.js:3861–3870).

5. **Push events for freshness, REST snapshot for initial state.** A chokidar fs-watcher with a timer backstop (30s) delivers diffs within ~300ms of a file write. The REST snapshot populates initial state and handles reconnects.

6. **`data-work-line` on every row doubles as both render key and scroll/selection target.** No separate selection model needed; DOM rows are the model.

7. **No next/previous stepper is needed.** "Follow" always jumps to the freshest change (by `lastEditedTabId → mtimeMs → insertion order`). An explicit N-badge on the Follow button communicates how many files were missed, motivating the user to click.

8. **Dual-gutter CSS-grid layout (head line-num | work line-num | content) keeps inline unified diff readable** without side-by-side complexity. Word-level intra-line marks (`mark.fe-w-add/del`) at higher saturation (~45%) cleanly layer on top of row-level tints (~18%).
