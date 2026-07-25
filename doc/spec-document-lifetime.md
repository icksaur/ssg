# spec-document-lifetime

Status: DRAFT v2 — ownership B; undo-loss-on-close accepted; handles weak. Spec
review folded (5 MUST / 3 SHOULD / 1 NIT, codex session, grounded in code).

## Goals

Closing a tab destroys its document and, with it, every document-level resource
(undo history, syntax highlighting, and any future per-document extension). No
document or document-scoped state outlives the last tab that referenced it. The
correctness of this follows from ownership and RAII, not from remembering to call
cleanup in each feature.

## Design

The ownership chain is: **tab → document → resources.**

- A **tab** is a view. It holds a *weak handle* to a document (`FileDocumentId`),
  never ownership. **The model is one tab per document**: `TabManager` dedups by
  `documentKey` (`src/TabManager.cpp:249-258`) — opening an already-open document
  re-activates its existing tab rather than adding a second. So closing a tab maps
  DIRECTLY to destroying its document; there is no refcount-by-referencing-tabs
  case to handle. (If a future change allows >1 tab per document, destruction must
  become refcounted; today it is not, and this spec does not add that.)
- A **document** is the ownership root for everything scoped to that document. It
  owns its text/piece-tree, and it owns its document-level resources: undo
  history, the syntax-highlighting model, and any resource a future feature
  attaches at document granularity. These resources are members (direct or
  pimpl), constructed with the document and destroyed with it — RAII, no
  `init()`/`teardown()` pairs, no external lifetime bookkeeping.
- A **resource** (history, syntax, …) never has an independent lifetime. It is
  reachable only through its owning document and dies when the document dies.

Closing the last tab that references a document destroys the document; its
resources destruct in the document's destructor. There is exactly one owner and
one destruction point.

**Current violation (what this change fixes).** Document-level resources are
owned by the runtime in `FileDocumentId`-keyed side maps
(`EditorRuntime::Impl::histories`, `::syntaxModels`), parallel to the `Workspace`
that owns the `Document`. Nothing coordinates their teardown, and nothing is torn
down on close at all: the `Workspace` never removes a closed document, so the
document and both side maps grow for the session. Each new document-level feature
adds another parallel map another close path must remember to clear — implicit
coupling that trends toward leaks and desync. Option B removes the parallel maps:
resources move under the document so a single erase destroys everything.

**Handles are weak.** Any stored `FileDocumentId` (a tab's `document`, the
find-state `findDocumentId`, diff file identities, recently-closed entries,
in-flight command payloads) is a weak reference. Resolving a handle to a live
document is a lookup that MAY return "absent"; "absent" is a normal, first-class
result meaning the document is gone, handled by a no-op / empty view / benign
failure — never a dangling pointer, never a crash, never a silent wrong document.

REQUIRED SEAM (review MUST — the current resolver throws). `Workspace::document(id)`
throws `std::out_of_range` on a missing id (`src/Workspace.cpp:526-531`). Under
destroy-on-close a stale id would become an exception, not a benign miss. Add a
non-throwing resolver — `Workspace::tryDocument(FileDocumentId) -> const Document*`
(nullptr on absent) — and route EVERY handle consumer through it (or through
`activeDocument()`, which already returns nullptr). No consumer may call the
throwing `document()` with an id that could be stale, and no consumer caches a
`Document*` across an operation that could close a tab.

REQUIRED (review SHOULD — resolution must not resurrect state). `historyFor` /
`syntaxFor` currently `try_emplace` on resolve (`src/EditorRuntime.cpp:620-628`) —
that IS the leak vector and would let a stale-id resolution recreate per-document
resources. Under the single-owner model, resolving a document's resources is a
PURE LOOKUP that returns absent for a gone document; a resource is created exactly
once, when the document is opened, not lazily on first access.

**Reopen.** `tab.reopen_closed` restores from the recovery journal / disk through
the normal open path and yields a NEW document with a NEW `FileDocumentId` and
fresh history + syntax. The reopened TAB's `document` handle MUST be rebound to the
new id (review MUST — current reopen reinserts the prior tab state/id at
`src/TabManager.cpp:431-443` / `EditorRuntime.cpp:359-366`; a reopened tab pointing
at the old, now-destroyed id would resolve to absent). This spec's reopen change
therefore includes the TabManager rebinding. Undo history does not survive a close.
No in-memory document is cached to make reopen cheaper — documents open fast;
caching would reintroduce the lifetime ambiguity this change removes.

Mechanism note (not pinned, but the read path IS pinned — review SHOULD): the
resource bundle may be realized by giving `Document` the history/syntax members
directly, or by a per-document aggregate owned in one runtime map keyed by
`FileDocumentId` that holds the document (or a reference to the Workspace-owned
document) plus its resources. Whichever placement is chosen, the READ path is
fixed: `activeSyntaxView()` (`src/runtime/snapshot.cpp:150-153`) and the snapshot
builder resolve the active document's resources by ABSENCE-SAFE lookup and fall
back to a well-defined empty/plain-text view when the document (or its resource) is
absent — never assume a map entry exists. Moving resources out of the side maps
must not change this behavior. The result must be ONE owning container destroyed
once on close, not N parallel maps.

## Invariants

- One owner: every document-level resource is owned by exactly one document; it is
  not reachable or destroyable except through that document.
- Destruction is total and automatic: destroying a document destroys all its
  resources (RAII); no resource requires a separate cleanup call, and no close
  path enumerates resource kinds.
- No document outlives its tabs: when the last referencing tab closes, the
  document is destroyed. (An unreferenced live document is a leak.)
- Handles are weak: a stored `FileDocumentId` confers no ownership and no
  liveness; every resolution tolerates absence as a normal result.
- No new parallel per-document map: adding a document-level feature attaches its
  resource under the document, gaining destruction for free; it does not add a
  runtime-level `FileDocumentId`-keyed map with its own teardown obligation.

## Considerations

- Enumerate every `FileDocumentId` holder and every raw `Document*`/reference and
  confirm each resolves through the weak-lookup boundary: `tabs` (TabState),
  `findDocumentId`, `diffFileIdentity`, recently-closed stack, snapshot assembly,
  and any command payload carrying a document id. Any site holding a `Document*`
  across a potential close is a bug. (Note: `FollowTarget` is keyed by diff-file
  identity/path, not `FileDocumentId` — it is a diff-subsystem handle, folded into
  the deferred diff-source design, not a document handle here.)
- Multi-tab is NOT possible today: `TabManager` dedups by `documentKey`
  (`src/TabManager.cpp:249-258`), so exactly one tab references a document and
  close destroys it directly — no refcount. Enforce/assert one-tab-per-document so
  a future regression to multi-tab can't silently make close a use-after-free.
- close() JOURNALS THE WRONG DOCUMENT (review MUST — pre-existing, becomes data
  loss under destroy-on-close). `EditorRuntime::Impl::close()`
  (`src/EditorRuntime.cpp:341-357`) builds the recovery journal from
  `activeDocument()` but stores it under the CLOSING tab's `state->key`. Closing a
  non-active dirty tab today persists the ACTIVE document's text under the closed
  tab's key; once close also destroys the document, that content is lost/wrong on
  reopen. FIX FIRST: journal the CLOSING tab's own document content (resolve
  `*tab.document` via the non-throwing resolver), not the active document.
- Do NOT reuse the delete-file flow for close-destroy (review MUST). The existing
  `Workspace` removal (`src/Workspace.cpp:936-968`) is gated to saved documents and
  performs on-disk DELETE side effects. Close must not touch the file on disk. Add
  a DISK-NEUTRAL `Workspace::removeDocument(FileDocumentId)` that erases only the
  in-memory `Entry` (and its `Document`), leaving the file untouched, and call it
  AFTER the recovery-journal write.
- `activeSyntaxView()` and the snapshot builder must return a well-defined view
  when the active document is absent (already do via plain-text fallback / empty
  sections); verify no path assumes a resource map entry exists after the ownership
  move.
- Deferred diff subsystem: diff file identities are weak handles too; fold this
  weak-handle rule into the later diff-source design (a stale diff identity =
  no overlay, already the intended behavior).

## Risks and Mitigations

- Dangling `Document*` after destruction → audit every `Document*`/reference for
  cross-close lifetime; resolve through the id→document lookup at point of use,
  don't cache the pointer. Covered by the weak-handle invariant + tests.
- Losing unsaved content on close → close must journal the CLOSING tab's own
  document (fix the active-document bug first), and destruction happens only AFTER
  the recovery-journal write via the disk-neutral remove. Test close-then-reopen
  restores the correct content, including closing a non-active dirty tab.
- Stale reopened tab handle → reopen assigns a new id and rebinds the tab; test
  that the reopened tab resolves to a live document, not the destroyed id.

## Acceptance (Definition of Done)

- Observable: opening then closing N documents returns per-document resource
  count to its pre-open level (no growth); the closed document's history and
  syntax are destroyed; reopen yields a fresh document with restored content and
  empty history.
- Budgets: no reopen regression (documents open fast; no cache); no per-render
  cost added.
- Gates: `bash scripts/check.sh` green (0 warnings, all tests).
- Oracles:
  - lifetime (mechanism-independent observability — review SHOULD): destruction of
    the document's history and syntax is proven by an instrumented resource
    destructor or a live-instance count accessor, NOT by "the map has no entry"
    (the owning container is intentionally unpinned). After open+close the live
    count returns to baseline. Fails before the fix (resources retained).
  - weak handle: resolving a `FileDocumentId` for a closed document returns absent
    at every consumer (find, snapshot, activate, nav, reopen) via the non-throwing
    resolver and produces a benign result — not a throw, crash, or another
    document's state.
  - journal-correctness: closing a NON-ACTIVE dirty tab journals THAT tab's
    content (not the active document's); close-then-reopen restores it exactly.
    Fails before the fix (active-document content is persisted).
  - one-tab-per-document: opening an already-open document re-activates its tab
    (no second tab); asserted so a regression that breaks dedup is caught before
    it makes close a use-after-free.
  - reopen: close-then-reopen restores content (from journal) with fresh/empty
    history and a NEW `FileDocumentId`, and the reopened tab's handle resolves to
    the new live document.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Non-throwing resolver: add `Workspace::tryDocument(FileDocumentId) -> const Document*` (nullptr on absent); this is the weak-handle seam every consumer routes through | `include/ssg/Workspace.h`, `src/Workspace.cpp` | unit: tryDocument returns nullptr for absent/never-existed id | handles are weak |
| 2 | Fix close journaling: journal the CLOSING tab's own document content (resolve `*tab.document`), not `activeDocument()` | `src/EditorRuntime.cpp:341-357` | journal-correctness oracle (non-active dirty tab) | (correctness prerequisite) |
| 3 | Establish the single per-document owner: move `DocumentHistory` + `SyntaxModel` ownership out of the runtime side maps into one owning container (Document members or a per-document aggregate); `historyFor`/`syntaxFor` become PURE lookups (no `try_emplace`), created once at open | `src/runtime/editor_runtime_internal.h`, `src/EditorRuntime.cpp`, `include/ssg/Document.h`/Workspace as chosen | build green; history+syntax behavior unchanged; absence-safe reads | one owner; no parallel map; pure-lookup resolve |
| 4 | Disk-neutral removal: add `Workspace::removeDocument(FileDocumentId)` erasing only the in-memory Entry+Document (NO on-disk delete); call it in close() AFTER the journal write, destroying the document and (RAII) its resources | `include/ssg/Workspace.h`, `src/Workspace.cpp`, `src/EditorRuntime.cpp` close | lifetime oracle: resources destroyed after close; file untouched on disk | no doc outlives tabs; total RAII destruction |
| 5 | Route every `FileDocumentId`/`Document*` consumer through the non-throwing resolver; treat absent as normal (find, snapshot, activate, nav, command payloads); cache no `Document*` across a close | `src/runtime/*.cpp`, snapshot/find/nav, TabManager consumers | weak-handle oracle across consumers | handles are weak |
| 6 | Reopen rebinds: reopen yields a NEW document id and REBINDS the reopened tab's `document` to it; content restored from journal, history empty | `src/TabManager.cpp:431-443`, `src/EditorRuntime.cpp:359-366`, `src/runtime/files.cpp` | reopen oracle (new id, live handle, content restored) | handles weak; total destruction |
| 7 | Enforce one-tab-per-document (assert dedup) so a future regression can't make close a use-after-free | `src/TabManager.cpp`, test | one-tab-per-document oracle | no doc outlives tabs |

## Rationale (skippable)

The user framing is the whole design: tabs reference documents; documents own
resources; lifetime does the work. The present code inverts the last step —
resources sit in runtime side maps keyed by document id, so they neither die with
the document nor are they owned by it, and each new document-level feature adds
another map to forget. Making the document the owner turns "don't leak" and "don't
dangle" from things every feature must remember into properties the type system
enforces once. Undo-history loss on close is accepted; documents open fast, so no
cache is warranted, and a cache would only restore the lifetime ambiguity being
removed.
