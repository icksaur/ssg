# spec-large-files-loading — Large-file open under budget (measurement-first)

Status: PLANNED. Milestone 13 (large files, phase 2). Follow-up to Milestone 12
(viewport projection), which made the 10 MiB *first frame* viewport-bounded
(~9 ms) and identified the document **read/open** as the new dominant cost.

**LF-1 measured baseline (2026-07; supersedes the 868 ms premise).** The 868 ms
`file_open` figure recorded at M12 (files/m12-first-frame-measurement.txt) is NOT
reproducible on the current build/machine — it was environmental (CPU governor /
load). Two independent harnesses now agree the 10 MiB open costs **~106 ms**
(startup_benchmark `file_open` p50 = 106 ms; the in-process `open_path_benchmark`
wall p50 = 101 ms), and the in-process OpenPhase seam attributes it as:

| phase | p50 | note |
|-------|-----|------|
| read (unbuffered `istreambuf_iterator`) | 14.8 ms | buffered size-hinted read is **1.1 ms** |
| nul_scan | 0.16 ms | negligible |
| **decode_validate** | **56.5 ms** | `decode_utf8` builds a `std::vector<Scalar>` (16 B/codepoint, ~160 MB for 10 MiB ASCII) |
| **eol_scan** | **18.8 ms** | `normalized()` re-walks that vector to build utf8 + terminators |
| document_build | 6.3 ms | Document ctor (redundant 2nd validate) + PieceTree build |
| state_dirty_check | 1.5 ms | `snapshot().text` materialize + persisted compare (pass I/J) |

`utf8_validation_calls = 2` and `piece_tree_text_calls_on_state = 1` are
confirmed. **The dominant cost is DECODE, not the read:** decode_validate +
eol_scan = **75 ms (~75% of the open)** is a two-pass scalar-vector decoder — a
whole-document intermediate the original audit did not anticipate. The read is
only 15 ms (1.1 ms buffered), so **mmap (LF-5) is UNWARRANTED**. See
files/m13-lf1-open-attribution.txt for the full report. The plan below is
re-ranked accordingly: the new dominant lever is fusing decode+normalize into one
pass (LF-2b), ahead of the buffered read and the copy/validation removals.


## Goal

Opening a large text file is not dominated by avoidable whole-document work — and
stays that way, because the document/text abstractions make efficient use the easy
path and wasteful use hard to write by accident. Concretely, for the 10 MiB
benchmark fixture the `file_open` phase (exec-path `post_attach → post_open`, i.e.
the `file.open` dispatch: read + decode + document construction; syntax and viewport
are already deferred off this path — M10/M12) drops from ~868 ms toward the
intrinsic minimum: **one** buffered disk read + **one** UTF-8 validation + the
piece-tree build, with no redundant re-validation, no extra whole-document copies,
and no needless re-materialization.

The organizing principle (post-measurement) is **correct-by-design**: the reason
the 868 ms is full of redundant reads/copies is that the abstractions leak — reads
hand out owned `std::string`s by value, construction re-validates because it cannot
receive proof, and separate subsystems each keep their own full copy because there
is no shared buffer. The fix is not to delete copies one by one (they grow back)
but to reshape the abstractions so wasteful operations are hard/impossible to
express and the cheap ones are the default.

- **Pit-of-success:** a whole-document `std::string` copy requires an explicit,
  named, greppable call (e.g. `materialize_to_string()`); the default read paths
  (iterate bytes, read a line, extract a range) allocate no whole-document buffer,
  and a `Document` cannot be constructed from unvalidated bytes nor made to validate
  the same bytes twice (validation is proven in the type system, not by convention).
- **SLO derivation (fixed in LF-1, before any optimization):** the tripwire is
  `intrinsic + headroom`, where `intrinsic` = the sum of the LF-1-measured phases
  that are NOT removed (buffered read + one validation + one EOL/status scan + one
  piece-tree build + the retained `persisted_text`/`raw_bytes` handling), and
  `headroom` = a fixed measurement margin (e.g. p99/p50 spread + a stated slack).
  Writing the formula and the retained-phase list in LF-1 BEFORE optimizing keeps
  `INV-open-under-budget` non-circular (the target is not chosen post-hoc from the
  optimized result).
- No correctness change: the opened document is byte-identical, UTF-8 is still
  validated (rejecting malformed files exactly as today), encoding/EOL/BOM
  detection and dirty-tracking are unchanged, and editing/undo/save behave
  identically. This is a performance + architecture refactor, not a behavior change.

Non-goals (this milestone): sub-linear open (reading N bytes is intrinsically
O(N)); a fundamentally different editor data structure (the piece tree stays); a
streaming read/write rewrite; changing the wire/snapshot contract; making
`word_wrap=ON` huge documents fast (M12 covered wrap gating). mmap is the FINAL
lever (LF-5), reframed as an alternate backing of the new shared buffer handle, and
pursued only if measurement shows the read still dominates — see the Decision.

## Design

### Measurement boundary (what the 868 ms includes)

The startup benchmark marks `post_attach` (`apps/ssg_main.cpp`) before, and
`post_open` after, the `file.open` dispatch. So `file_open` covers everything from
the dispatch down to the document being active, EXCLUDING:
- **syntax highlighting** — deferred: `refresh_syntax()` early-returns under
  `deferring_enrichment` (the app/probe sets `config.defer_enrichment = true`), so
  syntax is NOT in the 868 ms;
- **the tree scan** — deferred likewise (M10-3);
- **the first frame** — the viewport/render passes are a separate `first_frame`
  phase (~9 ms after M12).

So the 868 ms is purely: disk read + NUL scan + decode (validate + EOL scan) +
document construction (re-validate + piece-tree build) + the retained copies + the
`activate_document` → `Workspace::state()` step, which **materializes the whole
document** (`snapshot().text`) and byte-compares it to `persisted_text` for
dirty-detection (see pass I/J below). All of this is inside the measured phase.

### The whole-document passes on the open path (audited, to be confirmed by LF-1)

Traced from `file.open` (`src/runtime/files.cpp`) → `Workspace::open_file` →
`read_file` → `add_bytes` (`src/workspace.cpp`) → `decode_text`
(`src/text_encoding.cpp`) → `Document` ctor (`src/document.cpp`) → `PieceTree` ctor
(`src/piece_tree.cpp`):

| # | Pass | Location | Nature | Removable? |
|---|------|----------|--------|-----------|
| A | Disk read via `std::istreambuf_iterator` into `std::vector<uint8_t>` | `workspace.cpp:26-34` | **unbuffered byte-by-byte read** (slow) | **Yes** — buffered read |
| B | `contains_nul` scan | `workspace.cpp:363` | O(n) byte scan | Fold into decode (one pass) |
| C | `decode_text` → `decode_utf8`: validate every byte + build the `utf8` string | `text_encoding.cpp` | O(n) validate + **copy** | Intrinsic (one validation) |
| D | `normalized()` EOL scan → `line_terminators[]` | `text_encoding.cpp:187-221` | O(n) scan | Intrinsic-ish (needed for status) |
| E | `persisted = decoded.text->utf8` (non-dirty) | `workspace.cpp:375` | **full copy** kept for dirty-diff | **Yes** — share/move, not copy |
| F | `Document{decoded.text->utf8}` → copy into piece-tree `original_buffer_` | `document.cpp:95`, `piece_tree.cpp:284` | **full copy** | Reducible — move the decoded string in |
| G | `valid_utf8_without_nul(initial_text)` in the `Document` ctor | `document.cpp:91` | **SECOND full UTF-8 validation** | **Yes — redundant** (decode already validated) |
| H | Retained `raw_bytes` (the original `std::vector<uint8_t>`) | `add_bytes` entry | full copy retained | **KEEP** — `reopen_with_encoding` (`workspace.cpp:740,751,755`) re-decodes the ORIGINAL bytes under a different encoding; cannot be derived from decoded text |
| I | `activate_document` → `Workspace::state()` → `entry->document.snapshot().text` | `workspace.cpp:480`, `piece_tree.cpp:303` | **full piece-tree materialization** into a `std::string` | **Yes (surprising)** — the just-built tree is walked back into a whole string purely to dirty-check |
| J | Dirty check `text != entry->persisted_text` | `workspace.cpp:488` | **full O(n) byte comparison** | Reducible — a freshly opened non-dirty file is known-clean without a full compare |

So a 10 MiB open currently does **~2 full UTF-8 validations** (C, G), retains/
creates **~3–4 whole-document copies** (raw bytes, decoded utf8 in the entry,
`persisted_text`, piece-tree buffer), **re-materializes the whole document** back
out of the piece tree (I), and **byte-compares** it against `persisted_text` (J) —
all on top of a slow unbuffered read (A). The intrinsic minimum is: one buffered
read, one validation, one EOL/status scan, one copy into the piece tree, the
`persisted_text` the workspace genuinely needs, `raw_bytes` (kept for re-encoding),
and NO redundant re-materialize/compare for a just-loaded clean file.

### The levers, cheapest/safest first

**Re-ranked by LF-1 measured payoff (the audit's read-dominates assumption was
wrong — decode dominates).**

0. **Fuse decode + EOL-normalize into ONE pass (LF-2b, DOMINANT, ~75 ms):**
   today `decode_utf8` validates AND materializes a `std::vector<Scalar>` for
   every code point, then `normalized()` walks that vector a second time to
   produce the utf8 string + `line_terminators`. For the UTF-8 fast path this
   whole-document intermediate is unnecessary: a single pass can validate,
   append to the utf8 output (BOM-stripped, CR/CRLF-normalized), and record the
   per-line terminator in one walk — no `Scalar` vector. The transcoding paths
   (UTF-16/single-byte, rare and NUL-classified-binary on auto-open anyway) may
   keep the scalar path. This is the biggest single win and gates nothing else.
1. **Buffered read (A, ~13.7 ms):** replace `istreambuf_iterator` with a sized
   read (stat the size, `read()` into a right-sized buffer). Pure speed.
2. **Remove the redundant second validation (G):** the `Document` ctor
   re-validates UTF-8 that `decode_text` already guaranteed. Give the document an
   internal already-validated construction path (public `Document(string_view)`
   keeps validating for external callers). Malformed files are still rejected at
   decode.
3. **Eliminate the extra whole-document copies (E, F):** move the decoded `utf8`
   string into the piece tree instead of copying a `string_view`; share the
   decoded text with `persisted_text`. **`raw_bytes` (H) is KEPT** —
   `reopen_with_encoding` re-decodes the original bytes under a different
   encoding, which decoded text cannot reproduce.
4. **Skip the redundant re-materialize + compare on a fresh open (I, J, ~1.5 ms):**
   a just-opened non-dirty file is known-clean; `Workspace::state()` should report
   its dirty flag without walking the whole piece tree back into a `std::string`.
5. **mmap / lazy original buffer (LF-5): NOT WARRANTED per LF-1.** The read is
   ~15 ms unbuffered and ~1.1 ms buffered; mmap attacks an already-cheap phase
   and cannot touch the dominant decode cost. Retained in the plan only as a
   measurement-gated escape hatch for a future much-larger-file case; the
   Decision now defaults to NOT pursuing it.

### Correct-by-design: the abstractions that make the waste impossible

Each redundant pass above is a *symptom* of an abstraction that leaks. Removing the
pass without fixing the abstraction lets the next caller reintroduce it. The
post-measurement work (LF-2..LF-4) is therefore organized as three abstractions,
each of which makes a class of waste hard or impossible to express:

1. **`SharedBytes` — an immutable, shared, ref-counted buffer HANDLE (interface).**
   The decoded UTF-8 text, the piece tree's original buffer, and the initial
   `persisted_text` are the SAME *decoded* bytes; today each keeps its own
   `std::string`. Introduce a handle type exposing `const char* data()` + `size()`
   over an immutable buffer, ref-counted for sharing. **It is an interface/handle
   from the start** (not a bare `std::shared_ptr<const std::string>`): its default
   backing is an owned heap string, and LF-5 adds an mmap-backed implementation
   BEHIND THE SAME `data()`/`size()` contract, so consumers (piece-tree node offsets
   into `data()`) never re-plumb. **Raw vs decoded are DISTINCT buffers:** the raw
   file bytes (`raw_bytes`, kept for `reopen_with_encoding`) are NOT the same
   `SharedBytes` as the decoded text — BOM removal, CRLF normalization, and UTF-16
   transcoding mean decoded ≠ raw. Only the *decoded* bytes are shared three ways
   (`ValidatedUtf8` ↔ piece-tree original ↔ initial `persisted_text`). Sharing is
   sound because none of the three mutate the buffer in place: the piece tree only
   ever appends edits to a SEPARATE add-buffer and never rewrites the original;
   `persisted_text` is only ever REBOUND to a new `SharedBytes` on save/reload/
   reopen/restore, never edited in place. This collapses passes E and F. (Scope: the
   three-way share is the OPEN-time state; once the document is edited or re-saved,
   each holder rebinds independently — the abstraction does not couple them forever.)

2. **`ValidatedUtf8` — validation proven in the type, carrying full decode output.**
   A move-only value the decoder (`decode_text`) is the ONLY minter of; it carries
   the validated decoded bytes (as `SharedBytes`) PLUS the complete decode result the
   downstream needs for byte-identical save: the encoding/BOM/final-newline status
   AND the per-line `line_terminators` table (mixed-EOL round-trips depend on it, so
   it must travel WITH the validated value, not be recomputed). `Document` gains a
   construction path that CONSUMES a `ValidatedUtf8` and therefore does NOT
   re-validate (pass G gone by construction). The public `Document(std::string_view)`
   still validates for external callers (untitled buffers, tests); the workspace's
   open path moves a `ValidatedUtf8` in. Pit-of-success: the type system, not a
   comment, enforces "the decoder's output is validated exactly once, and a document
   cannot be built from unvalidated bytes except via the public validating ctor."
   **Validation-count semantics (resolving the counter):** a DIRECT UTF-8 decode
   performs exactly ONE validating scan (minting `ValidatedUtf8`); a TRANSCODE from
   another encoding (UTF-16, etc.) produces UTF-8 that is well-formed BY
   CONSTRUCTION, so it mints `ValidatedUtf8` with ZERO additional validation scans.
   So `utf8_validation_calls` counts *validating scans*: 1 for a direct-UTF-8 open,
   0 for a transcoded open, 0 for binary/NUL (rejected pre-decode), and — the point
   of the milestone — NEVER 2 (today's `Document`-ctor re-scan is removed). The
   "intrinsic minimum" is stated per input class accordingly.

3. **`TextView` / cheap read access — no accidental whole-document `std::string`.**
   `Document::snapshot()` returns a `DocumentSnapshot` whose `text` is an owned
   `std::string` materialized from the whole piece tree on EVERY call
   (`piece_tree.cpp:303`) — this is pass I on open, and a per-frame cost elsewhere.
   Introduce a non-owning read handle (`TextView`) over the document/piece-tree that
   supports the operations callers actually need — byte/char iteration, line access,
   range/substring extraction, equality/compare against another byte range — WITHOUT
   materializing the whole document. The rare caller that genuinely needs one
   contiguous `std::string` calls an explicit `materialize_to_string()` (loud,
   greppable, easy to audit).
   **Lifetime contract (snapshot-like, revision-pinned):** a `TextView` borrows the
   document and is valid only against the document revision it was taken at; it is an
   iterator-style handle — ANY mutation (edit/undo/redo/reopen) or the document's
   move/destruction invalidates it, exactly like a container iterator. It must not be
   stored across a mutation or a snapshot boundary; callers that need a stable value
   past an edit call `materialize_to_string()`. The dirty-check migration (LF-4b)
   holds a `TextView` only within a single synchronous `Workspace::state()` call and
   never across an edit, satisfying this by construction. (Optionally the handle
   carries the revision and debug-asserts staleness on use.)
   Migrate the open-path dirty check (passes I, J) and the cheapest hot callers to
   `TextView`; a repo-wide migration of every `snapshot().text` caller is a tracked
   follow-up, NOT gated here.

These three are the "efficient readers / whatever makes sense" the milestone is
really about. The measured redundancies (E/F/G/I/J) are removed as a *consequence*
of adopting them, not as ad-hoc patches — so the waste cannot silently return.
Scope guard: LF-2..LF-4 introduce the abstractions and migrate the OPEN path (and
the cheapest hot callers) onto them; the public `snapshot().text` remains as an
ACKNOWLEDGED escape hatch (a repo-wide migration is a separate tracked follow-up),
so INV-pit-of-success is scoped to the migrated paths + all NEW code, not asserted
globally over untouched callers (see the invariant).

### Decision (RESOLVED by LF-1): how far to go

- **(A) Redundancy-removal + decode fusion (CHOSEN):** levers 0–4 (LF-2b fused
  decode, LF-2 buffered read, LF-3a/3b validation+copy removal, LF-4a/4b TextView
  + skip re-materialize). LF-1 shows these cover the entire measured cost — the
  75 ms decode, the 13.7 ms read, the double validation, the extra copies, and the
  1.5 ms re-materialize.
- **(B) mmap (lever 5): REJECTED by measurement.** The read is ~15 ms unbuffered
  and ~1.1 ms buffered; mmap cannot touch the dominant decode cost. LF-5 is
  retained ONLY as a dormant, measurement-gated escape hatch for a future
  much-larger-file case, not part of this milestone's committed work.

The milestone executes (A); (B) stays parked behind its own future measurement.

## Invariants

- **INV-open-correctness:** the document opened after this change is byte-identical
  to today's, UTF-8 is validated exactly once (for a UTF-8 text open) and
  malformed/NUL-containing files are still rejected/classified identically, and
  encoding/EOL/BOM/final-newline status and initial dirty state are unchanged. (An
  open-equivalence oracle against IMMUTABLE baseline goldens — see LF-1.)
- **INV-single-validation:** the open path performs at most one *validating scan*
  of a document's UTF-8. Per input class (proven by the test-only
  `utf8_validation_calls` counter, reset/read like M12's `cell_run_calls`): a
  **direct UTF-8** open validates **exactly 1** (the decode scan; the `Document`
  ctor no longer re-scans); a **transcoded** open (UTF-16/other) validates **0**
  (the transcode produces well-formed UTF-8 by construction — no separate scan); a
  **binary/NUL** open validates **0** (rejected before decode); a **decode-failure**
  open validates the input **once** (the failing decode). Never 2. (Today an
  accepted direct-UTF-8 open validates **twice** — decode then the `Document` ctor —
  which LF-1 quantifies and LF-3a removes.)
- **INV-bounded-copies:** the **peak count of simultaneously-live whole-document
  byte buffers** created by the open path does not exceed the intrinsic set:
  { one shared *decoded* buffer (the `SharedBytes` shared by the piece-tree original
  ↔ `ValidatedUtf8` ↔ initial `persisted_text`), and the *raw* `raw_bytes` buffer
  (distinct — kept for `reopen_with_encoding`) } — i.e. **2** distinct
  whole-document allocations, down from ~4–5. The redundant transient copies (the
  standalone `decoded.utf8` string currently retained AND copied into both the tree
  and `persisted_text`, and the `snapshot().text` re-materialization) are
  eliminated. Proven by a documented ownership/move audit plus a whole-document
  allocation counter that must not exceed the target.
- **INV-no-fresh-open-materialize:** opening a file does NOT walk the just-built
  piece tree back into a `std::string` to compute its dirty flag (lever 4); a
  freshly opened file's dirty state is known without a whole-document
  materialize/compare. (Proven by an instrumented `piece_tree_text_calls` counter:
  0 tree-materializations attributable to `activate_document`/`Workspace::state()`
  on a fresh open.)
- **INV-save-roundtrip:** saving an unmodified opened file re-encodes to the
  byte-identical on-disk content (so any change to `raw_bytes`/`persisted_text`
  handling does not corrupt round-trips), and editing then saving is unchanged.
- **INV-pit-of-success (correct-by-design, SCOPED):** on the MIGRATED paths (the
  file-open path) and in ALL new code, obtaining a whole-document `std::string`
  requires an explicit, named call (`materialize_to_string` or equivalent); the
  default read handle (`TextView`) exposes no implicit whole-document copy, a
  `Document` cannot be constructed from unvalidated bytes except via the public
  validating constructor, and the decoder's `ValidatedUtf8` cannot be re-validated.
  NOT asserted globally: the public `Document::snapshot().text` remains an
  acknowledged escape hatch for un-migrated callers (repo-wide migration is a
  tracked follow-up). Enforced by (a) types/access control (`ValidatedUtf8`
  unforgeable, `TextView` the default read), (b) a **source grep gate** — a test
  scanning the named open-path TUs (`src/workspace.cpp` open path,
  `src/runtime/files.cpp`) for `.snapshot().text` and known whole-document copy
  helpers, failing on a new occurrence, AND (c) the runtime `piece_tree_text_calls`
  counter (0 attributable to a fresh open) so a copy hidden behind a helper is
  caught dynamically. This scoped invariant keeps the removed redundancy from
  growing back on the hot path.
- **INV-open-under-budget:** 10 MiB `file_open` p50 is under the LF-1-derived
  tripwire (enforced via the startup harness, like M10-5).

## Considerations

- **`persisted_text` is load-bearing:** it is the on-disk snapshot used for
  dirty-detection (`text != persisted_text`), save, and external-modification flow
  (`workspace.cpp` ~488, ~729, ~762, ~975). Any change to how it is populated on
  open must preserve those comparisons exactly. Prefer sharing the decoded string
  (move/ref-count) over copying, not dropping it.
- **`raw_bytes` is load-bearing (KEEP):** `reopen_with_encoding`
  (`workspace.cpp:740,751,755`) re-decodes the ORIGINAL bytes under a
  user-selected encoding — decoded UTF-8 cannot reproduce an alternative
  interpretation of the raw bytes. `raw_bytes` is retained unchanged; it is NOT a
  removal target.
- **Immutable shared bytes vs mutable decoded metadata:** the `SharedBytes` shared
  three ways is the *decoded byte content*. The decode STATUS + per-line
  `line_terminators` are separate, and some paths MUTATE them: `apply_terminator_edits`
  rebuilds `DecodedText::utf8`, and save/reload/reopen/restore rebind the persisted
  state. The three-way byte sharing therefore holds ONLY for the open-time
  (unedited) state; once content changes, each holder rebinds its own `SharedBytes`
  (the piece tree's original is immutable and edits go to the add-buffer;
  `persisted_text` is rebound on save; `ValidatedUtf8` is consumed at construction
  and does not persist). The spec scopes sharing to that open-time window and does
  NOT couple the mutable EOL metadata into `SharedBytes`; `line_terminators` travels
  with `ValidatedUtf8` (for the initial save round-trip) and is owned/rebuilt by the
  document's edit path exactly as today thereafter.
- **`Document::snapshot()` re-materializes** the whole document via
  `PieceTree::text()` on every call with no cache (`piece_tree.cpp:303`). It IS on
  the open path: `activate_document` → `Workspace::state()` calls it (workspace.cpp
  :480) to dirty-check, so a fresh open re-walks the whole tree. Lever 4 removes
  that specific call for a fresh open; a general snapshot/text cache is a separate,
  out-of-scope candidate unless LF-1 shows it elsewhere in the phase.
- **Encoding coverage:** the corpus must include UTF-8 (with/without BOM), CRLF vs
  LF vs mixed, a final-newline and no-final-newline file, an empty file, a
  binary/NUL file, a decode-failure file (malformed UTF-8), and a UTF-16 (BOM)
  file — so the redundancy removal cannot silently change decode/classification/
  status. NUL-before-malformed and malformed-before-NUL cases are included (lever B
  precedence, see Risks).
- **Platform:** mmap (LF-5, if pursued) is Linux-first (`mmap`/`munmap`); the
  piece-tree buffer-handle abstraction MUST keep an owned-`std::string` fallback so
  non-Linux (and a future Windows `CreateFileMapping`/`MapViewOfFile`) builds work
  unchanged. Levers 1–4 are portable.

## Risks and mitigations

- *Removing the second validation lets malformed bytes through* → decode (C) is the
  single validation and already rejects malformed/NUL input; the open-equivalence
  oracle includes malformed/NUL fixtures asserting identical rejection AND
  classification (binary vs decode-failure).
- *Folding NUL detection into a short-circuiting decoder changes classification* →
  today `contains_nul` observes ALL raw bytes, so a NUL anywhere classifies the file
  binary even if malformed UTF-8 precedes it. A decoder that short-circuits on the
  first malformed byte would instead report decode-failure when malformed bytes
  precede a later NUL — a behavior change. Mitigation: NUL detection must be a
  WHOLE-raw-byte observation with the SAME precedence as today (binary wins over
  decode-failure regardless of order); tested with NUL-before-malformed,
  malformed-before-NUL, and BOM-bearing UTF-16 (whose bytes legitimately contain
  0x00) inputs asserting identical classification.
- *Copy-sharing corrupts dirty-detection or save* → INV-save-roundtrip +
  INV-open-correctness oracles over the encoding corpus, plus the existing
  external-modification and save tests, gate it. `persisted_text` is populated with
  the SAME bytes as today (shared, not recomputed), so the `text != persisted_text`
  comparison is unchanged.
- *Skipping the fresh-open dirty compare mis-reports dirty* → a freshly opened
  non-dirty file IS clean by construction (untitled files and dirty-restored files
  are separate paths that keep their current logic); the oracle asserts the initial
  dirty flag matches the pre-change baseline across the corpus (incl. an
  untitled/scratch doc and a dirty-recovery-restored doc).
- *mmap complexity/UB (unmap while referenced, file truncated under us)* → mmap is
  gated behind measurement (only if warranted), is its own reviewed step with a
  buffer-handle lifetime design, keeps the original buffer read-only (edits go to
  the add-buffer, never the mapped region), and retains an owned-`std::string`
  fallback for non-Linux and for files that cannot be mapped.
- *mmap SIGBUS when another process truncates the mapped file* → a read-only mmap
  does NOT prevent SIGBUS if the underlying file is truncated by another process
  while mapped. LF-5 MUST specify a concrete policy — e.g. copy-into-owned-buffer
  for files below a size threshold (so only genuinely large files are mapped), a
  SIGBUS handler or `madvise`, or detabling the map on external-modification detect
  — with an oracle that a truncation-under-map does not crash the editor. This is a
  precondition for activating LF-5, not an afterthought.
- *Measurement noise* → reuse the M10 startup harness protocol (warm cache, fixed
  reps/discard, p50/p99, `CLOCK_MONOTONIC`), and attribute sub-phases with a
  cross-layer timing seam (see LF-1) so the split is data, not estimate.

## Acceptance (Definition of Done)

- LF-1 report attributes the 868 ms across read / NUL / decode+validate / EOL /
  copies / piece-tree BUILD / snapshot-materialize+compare, writes the SLO tripwire
  formula (`intrinsic + headroom`) and the retained-phase list, and shows the
  current open validates UTF-8 twice.
- After LF-2/3/4, 10 MiB `file_open` p50 is under the tripwire; the harness enforces
  it (`--enforce`-style, extending M10-5).
- `utf8_validation_calls` matches the per-input-class expectation (1 for accepted
  UTF-8); `piece_tree_text_calls` attributable to a fresh open is 0; the copy audit
  shows the redundant copies removed.
- Gates: `cmake --build build` clean; `ctest --test-dir build -E
  performance_measurement` green (incl. the open-equivalence + single-validation +
  no-fresh-open-materialize + save-roundtrip oracles); no behavior change in
  existing file/encoding/save/external-modification tests.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| LF-1 | **DONE (see files/m13-lf1-open-attribution.txt).** Cross-layer OpenPhase timing seam (`include/ssg/open_metrics.h` + `src/open_metrics.cpp`) bracketing read / NUL-scan / decode+validate / EOL-scan / document-build / state-dirty-check; test-only `utf8_validation_calls` + `piece_tree_text_calls` counters; isolated buffered-read calibration; immutable open-equivalence goldens (`tests/test_open_equivalence.cpp` + `tests/fixtures/open/golden.txt`, 12-entry corpus); evolving counter oracle (`tests/test_open_metrics.cpp`); attribution benchmark (`benchmarks/open_path_benchmark.cpp`). **Finding: open is ~106 ms (not 868 ms); decode_validate+eol_scan = 75 ms dominate; read is 15 ms / 1.1 ms buffered; validates twice; state materializes once.** SLO + retained-phase list written in the baseline report. | shipped above | the harness prints per-sub-phase p50/p99 and names `decode_validate` dominant; the buffered-read calibration yields ~1.1 ms; `utf8_validation_calls == 2`; the corpus goldens are captured and committed | (measurement + baseline; establishes INV-open-under-budget target and the LF-2/3 reference) |
| LF-2 | **`SharedBytes` handle + buffered read (abstraction 1, lever 1/B):** introduce the immutable ref-counted buffer as an INTERFACE/handle exposing `data()`+`size()` (owned-string backing now; mmap backing added at LF-5 behind the same contract — NOT a bare `shared_ptr<const string>` that LF-5 must re-plumb); replace `istreambuf_iterator` with a size-hinted buffered read that reads to EOF into an owned buffer (handles a file growing/shrinking under us and short reads — never sizes once and trusts it); keep NUL detection as a WHOLE-raw-byte observation with today's binary-wins precedence (do NOT make it depend on decode short-circuiting). The *decoded* text shares one `SharedBytes` with the (later) piece-tree original; the *raw* bytes are a DISTINCT buffer (kept for `reopen_with_encoding`). | `include/ssg/*` (SharedBytes handle), `src/workspace.cpp`, `src/text_encoding.cpp` | re-measure the read sub-phase (drops from ~15 ms toward the ~1.1 ms calibration); the open-equivalence oracle vs the LF-1 goldens is unchanged for every corpus entry; NUL-before-malformed, malformed-before-NUL, and UTF-16-BOM inputs classify identically; a unit test shows two holders of one `SharedBytes` do not double-allocate, and that the handle interface admits an alternate (test) backing without touching consumers | INV-open-correctness, INV-bounded-copies, INV-open-under-budget |
| LF-2b | **Fused single-pass UTF-8 decode+normalize (DOMINANT lever, ~75 ms):** for the UTF-8 fast path, replace the `decode_utf8` → `std::vector<Scalar>` → `normalized()` two-pass with ONE walk that validates, appends to the decoded utf8 output (BOM-stripped, CR/CRLF→LF normalized), and records the per-line `LineTerminator` — no whole-document `Scalar` vector. Preserve exact classification/offsets on malformed input (the failing byte offset must match today). Transcode paths (UTF-16/single-byte) may keep the scalar path. This is independent of the type work (LF-3a) and can precede it. | `src/text_encoding.cpp` (`decode_selected` utf8 branch, `decode_utf8`, `normalized`), `tests/test_text_encoding.cpp` | the open-equivalence + save-roundtrip oracles vs the LF-1 goldens are byte-identical for every corpus entry (esp. mixed-EOL, no-final-newline, BOM, malformed-offset); `decode_validate`+`eol_scan` re-measure to a small fraction of 75 ms; `utf8_validation_calls` still counts 1 validating scan for the fused pass | INV-open-correctness, INV-single-validation, INV-save-roundtrip, INV-open-under-budget |
| LF-3a | **`ValidatedUtf8` — single validation in the type (abstraction 2, lever 2):** the decoder mints a move-only `ValidatedUtf8` carrying the validated decoded `SharedBytes` + the encoding/BOM/final-newline status + the per-line `line_terminators` table (required for byte-identical mixed-EOL saves — it MUST travel with the validated value); `Document` gains an UNFORGEABLE construction path that CONSUMES it and does not re-validate (private ctor + decoder-minted token / `friend`-scoped factory — NOT a comment saying "internal"); the public `Document(string_view)` still validates for external callers | `include/ssg/document.h`, `src/text_encoding.{h,cpp}`, `src/document.cpp`, `src/workspace.cpp` | open-equivalence + save-roundtrip vs goldens unchanged (incl. a mixed-EOL fixture saving byte-identically, proving `line_terminators` survived); `utf8_validation_calls` == the per-input-class expectation (1 direct-UTF-8, 0 transcoded, 0 binary/NUL); a unit test proves the PUBLIC `Document` ctor still throws on malformed UTF-8 / NUL, and that there is no public way to build a `Document` from unvalidated bytes without it; re-measure | INV-single-validation, INV-pit-of-success, INV-save-roundtrip, INV-open-correctness |
| LF-3b | **Move ownership through, share `persisted_text` (abstraction 1 applied, lever 3):** the piece tree's original buffer becomes a `SharedBytes` (move the decoded bytes in, no `string_view` copy); `persisted_text` shares the same `SharedBytes` rather than a second full copy. `raw_bytes` retained (kept for `reopen_with_encoding`). | `src/piece_tree.{h,cpp}`, `src/document.cpp`, `src/workspace.cpp` | open-equivalence + save-roundtrip vs goldens unchanged; the whole-document-buffer allocation/live-count audit shows the redundant transient copy removed (peak live distinct whole-document allocations ≤ the intrinsic set: {shared original/decoded/persisted = 1, raw_bytes = 1}); re-measure | INV-bounded-copies, INV-save-roundtrip, INV-open-correctness, INV-open-under-budget |
| LF-4a | **`TextView` — cheap read handle (abstraction 3, API + lifetime):** introduce the non-owning `TextView` (iterate / line / range / compare-to-byte-range) with an explicit `materialize_to_string()`; specify the revision-pinned iterator-style lifetime (any mutation/move/destruction invalidates it; a debug staleness assert). No caller migration yet. | `include/ssg/document.h`, `src/document.cpp`, `src/piece_tree.{h,cpp}` | unit tests: `TextView` iterate/line/range/compare match `snapshot().text` for the corpus WITHOUT calling `piece_tree.text()` (0 `piece_tree_text_calls`); `materialize_to_string()` equals `snapshot().text`; a debug build asserts on use-after-mutation | INV-pit-of-success, INV-open-correctness |
| LF-4b | **Skip the fresh-open materialize/compare + grep gate (lever 4):** make `Workspace::state()` report a freshly opened file's dirty flag via `TextView` (or a known-clean fast path) WITHOUT `snapshot().text` + a full `persisted_text` compare (a fresh non-dirty open is clean by construction; edited buffers still compute dirty); add the scoped pit-of-success grep gate over the open-path TUs | `src/workspace.cpp`, a grep-gate test | `piece_tree_text_calls` attributable to a fresh open == 0; the initial dirty flag matches the goldens across the corpus (incl. untitled/scratch and dirty-recovery-restored docs); external-modification + save tests still pass; the grep gate fails on a new `.snapshot().text` in the open-path TUs; re-measure `file_open` under the tripwire | INV-no-fresh-open-materialize, INV-pit-of-success, INV-open-correctness, INV-open-under-budget |
| LF-5 | **(Decision B, measurement-gated) mmap as an alternate `SharedBytes` backing** — ONLY if LF-4's measurement leaves the read+build as the dominant residual above target: a read-only mmap-backed `SharedBytes` implementation (Linux) behind the SAME handle interface, with the owned-buffer fallback (non-Linux, unmappable files, AND small files below a size threshold so only genuinely large files are mapped); edits still go to the add-buffer; a concrete **external-truncation policy** (threshold copy-in / SIGBUS handling / drop-map on external-mod) prevents SIGBUS if another process truncates the mapped file; lifetime/ownership reviewed separately. Gated on TOTAL `file_open` residual, not a nominal read sub-phase (node construction still faults pages in). | `include/ssg/*` (SharedBytes mmap impl), `src/piece_tree.{h,cpp}`, a platform mmap wrapper (with fallback), tests | with mmap on, TOTAL `file_open` drops further and the open-equivalence + save-roundtrip + edit/undo oracles all still pass; a fixture edited across the original/add-buffer boundary round-trips; the owned-buffer fallback path is exercised; a truncation-under-map test does not crash; mmap Linux-gated in cmake | INV-open-correctness, INV-save-roundtrip, INV-open-under-budget |

Instrumentation note: `utf8_validation_calls` / `piece_tree_text_calls` (LF-1) and
the cross-layer phase timers are test/benchmark-only mechanisms (thread-local
counters / compiled-out timing scopes), NOT production state — a
full-validation-twice or re-materializing implementation cannot pass the counter
oracles.

## Rationale (skippable)

M12 removed the per-frame O(document) segmentation, exposing the *read/open* as the
10 MiB bottleneck (868 ms). The instinctive fix — mmap — is the deepest, riskiest
change (the piece tree owns its original buffer as a `std::string`), yet the audit
shows the 868 ms is not one intrinsic read: it is an unbuffered byte-by-byte read
plus **two** UTF-8 validations, **three-to-four** retained whole-document copies,
and a needless re-materialize + full compare. Crucially, those are not independent
bugs — they are symptoms of leaky abstractions (reads return owned strings by value;
construction re-validates because it cannot receive proof; subsystems each keep
their own copy for lack of a shared buffer). So the milestone is measurement-first
(as in M10 fast-startup) AND correct-by-design (per the project's pit-of-success
value): attribute the cost (LF-1), then introduce three abstractions —
`SharedBytes` (one shared immutable buffer), `ValidatedUtf8` (validation proven in
the type), and `TextView` (cheap reads; explicit `materialize_to_string`) — that
make the measured redundancy impossible to express rather than merely deleted
(LF-2/3/4), and reach for mmap (LF-5, an alternate `SharedBytes` backing) only if
the numbers still demand it. The correctness oracles (byte-identical open against
immutable goldens + single validation + no-fresh-open-materialize + save round-trip)
keep the hot-path refactor safe by construction, and the pit-of-success grep gate
keeps the waste from growing back.
