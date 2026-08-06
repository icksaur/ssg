# spec-r4-runtime-collaborators

Status: draft

## Goals

Make `src/runtime/*.cpp` read as thin command-binding tables whose handler
bodies live on the object that owns the relevant state, per cpp-values ("a
*_commands file is a thin table; the behavior lives on the domain object it acts
on"). Do it in small behavior-preserving commits, each green, without inventing
ceremony wrappers that own no state.

## Design

Today `EditorRuntime::Impl` is a god-object: the four handler files
(navigation/editing/files/presentation, ~2670 lines) are anonymous-namespace
free functions `fn(EditorRuntime::Impl& runtime, ...)` that a `registerXxx`
table binds to command ids, plus `bindRuntimeXxx` entry points.

The investigation (files/arch-review-least-surprise.md + this session) found the
handlers fall into THREE kinds, not one, and only two of them warrant change:

1. **Stateless value transforms** — pure functions over values, no `Impl`
   dependency: `wordUnderCaret(text, Selection)` (editing.cpp:23),
   `historyKind(TextInputCommand)` (editing.cpp:46). These belong on the value
   they transform. Unambiguous cpp-values win, trivially oracle-tested, lowest
   risk. **DO THESE.**

2. **Genuine new-owner extractions** — cohesive state currently living loose in
   `Impl` that a named object should own, OR logic for one concern SPLIT across
   files (e.g. tree-panel coordination lives in BOTH presentation.cpp
   `syncTreeProviderToPanel` and navigation.cpp `treeCommand`/
   `panelProviderTreeId`). Extracting these removes a real smell. **DO THESE
   where a cohesive owner genuinely exists — one per commit, each spec'd if the
   surface is wide.**

3. **Thin coordinators with no cohesive state of their own** — handlers that
   read/act on an ALREADY-owned model (`DiffModel`, `FollowEditsModel`,
   `SearchController`, `FindReplaceController` already exist as owners) and then
   call `Impl`'s SHARED navigation machinery (`revealDiffTarget`,
   `openOrRevealFollowTargetProgrammatic`, `activateDocument`,
   `openOrFocusLiveDiffTab`). `revealDiffTarget`/
   `openOrRevealFollowTargetProgrammatic` are called from EditorRuntime.cpp too
   (2042, 2152), so they cannot move into a per-concern controller — they are
   Impl's shared reveal layer. A `DiffController`/`FollowController` holding only
   `Impl&` would move a free function into a class method and own nothing. That
   is ceremony, and cpp-values forbids ceremony as loudly as it forbids free
   functions carrying state. **DO NOT do these as controller-wrappers.** If the
   goal is only to shrink the file, that is not sufficient justification.

Mechanism for kind 1 (this spec's first commit): move `wordUnderCaret` to a
`Selection` method (it is exactly "the text this selection covers, or the word
the caret sits in") and `historyKind` to a free value-conversion
`historyEditKind(TextInputCommand)`. The two enum types live in uncoupled
sibling headers — `TextInputCommand` in TextInputCommands.h, `HistoryEditKind`
in DocumentHistory.h, neither including the other. To avoid coupling them, the
conversion goes in a NEW dedicated header-only value header
`include/ssg/HistoryEditClassification.h` that includes both enum headers and
declares `[[nodiscard]] inline HistoryEditKind historyEditKind(TextInputCommand)
noexcept` — the exact precedent of `WordClassification.h`'s inline `isWordByte`.
This keeps the enum headers independent and gives the mapping a compiled-nowhere,
test-anywhere home. Rejected alternative: a `TextEditingController` owning both —
it would own no state the models don't already own; ceremony.

The complete mapping (pinned so the oracle and impl are self-contained):
`Insert`, `Newline` → `Typing`; `DeleteBackward`, `DeleteWordBackward` →
`DeleteBackward`; `DeleteForward`, `DeleteWordForward` → `DeleteForward`; any
other → `Other`.

## Invariants

- Behavior-preserving: every migrated handler produces identical results; the
  existing suite is the oracle, no golden regeneration.
- No ceremony: a new type introduced by R4 must EITHER own cohesive state OR
  encapsulate a cohesive cross-call contract (a stateless composition object, as
  cpp-values permits — e.g. R1's CommandReferenceRenderer). What is inadmissible
  is a pure forwarding facade holding only `Impl&` introduced ONLY to shrink a
  file: it neither owns state nor encapsulates a contract, it just relocates a
  free function into a method. Shrinking runtime/*.cpp is not, by itself,
  sufficient justification.
- Impl's shared reveal/navigation layer (`revealDiffTarget`,
  `openOrRevealFollowTargetProgrammatic`, `activateDocument`) stays on Impl while
  more than one caller (including EditorRuntime.cpp) needs it.
- One identifiable change per commit; `bash scripts/check.sh` green at each.

## Considerations

- `wordUnderCaret` uses `isWordByte` (WordClassification.h, a value header), so
  the `Selection` method can include it with no new dependency direction.
- `historyEditKind` maps `TextInputCommand` (TextInputCommands.h) →
  `HistoryEditKind` (DocumentHistory.h), two uncoupled sibling headers. It lives
  in a new `include/ssg/HistoryEditClassification.h` that includes both — the
  enum headers stay independent of each other, and text input does NOT gain a
  dependency on history (nor vice versa).
- Later kind-2 candidates (tree consolidation) have WIDE Impl surface
  (`treeCommand` touches shell/tree/diff/workspace/activateDocument/scroll) — each
  needs its own spec and is NOT part of this first commit.

## Risks and Mitigations

- Risk: relocating `wordUnderCaret` changes selected-text vs word-under-caret
  behavior at a boundary. Mitigation: a direct unit oracle (caret-in-word,
  caret-between-words, non-empty selection, out-of-range) plus the existing
  editing suite (search-word-under-caret path at editing.cpp:360).
- Risk: scope creep into ceremony extractions to "finish R4". Mitigation: the
  no-ceremony invariant; each kind-2 extraction is its own reviewed spec.

## Acceptance (Definition of Done for commit 1)

- Observable: editing.cpp no longer defines `wordUnderCaret`/`historyKind`; the
  call sites read `primary.wordOrCoveredText(text)` and
  `historyEditKind(command)`.
- Budgets: n/a.
- Gates: `bash scripts/check.sh` green (101 tests, 0 warnings).
- Oracles:
  - word-selection unit test on `Selection`: reference cases (caret in a word →
    that word; caret between non-word bytes → empty; a range selection → the
    covered substring; out-of-range → empty), written before the method moves.
  - history-kind mapping unit test: every `TextInputCommand` enumerator maps to
    the pinned `HistoryEditKind` (Insert/Newline→Typing;
    DeleteBackward/DeleteWordBackward→DeleteBackward;
    DeleteForward/DeleteWordForward→DeleteForward).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `Selection::wordOrCoveredText(std::string_view)` (moving the body), keep call site behavior | `include/ssg/Selection.h`, `src/Selection.cpp`, `src/runtime/editing.cpp` | word-selection unit test (fails first) | behavior-preserving; no-ceremony |
| 2 | Add `HistoryEditClassification.h` with inline `historyEditKind(TextInputCommand)`; drop the TU-local `historyKind` | new `include/ssg/HistoryEditClassification.h`, `src/runtime/editing.cpp` | history-kind mapping unit test | value-conversion stays free; enum headers stay uncoupled |
| 3 | Gate + commit as ONE "relocate stateless transforms" change | — | full suite green | one change per commit |

Subsequent phases (each a SEPARATE reviewed spec, not this commit): assess tree
consolidation (kind 2) as the next genuine target; explicitly close out the
diff/follow/search "controller" items from the R1–R4 plan as **won't-do
(ceremony)** unless a cohesive owner is identified.

## Rationale (optional)

The R1–R4 plan named DiffController/FollowController/etc. before the handler
bodies were read closely. Reading them shows the state those controllers would
"own" is already owned (DiffModel/FollowEditsModel/SearchController/
FindReplaceController) and the orchestration they call is Impl's shared reveal
layer that EditorRuntime.cpp also uses. Extracting them would satisfy the letter
of "shrink runtime/*.cpp" while violating the spirit of cpp-values (an object
must own something). The honest R4 is smaller than the plan implied: do the real
value-method relocations, do the genuine split-consolidations, and decline the
ceremony. This spec starts with the safest real win and puts the ceremony
judgment on the record for review.
