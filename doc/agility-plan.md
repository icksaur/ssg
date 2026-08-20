# Agility plan

Two independent analyses (GPT-5.6-sol, Claude Opus 5) diagnosed why SSG slowed
down. This is the consolidated, actionable result. Their raw reports were folded
into this file and deleted — a 1,956-line pair of analyses would have reproduced
the exact problem they identified.

## The finding, in one line

**Nothing is slow. Everything is duplicated.** The fast gate is 5.7s and a
one-file rebuild is 4.6s. The cost is that one behaviour change must be
re-expressed in 8–13 places, and each re-expression is an agent round-trip.

## Measured evidence (last 40 commits)

```
src+include+apps  4,287 lines
tests             3,722 lines   (0.87x src)
docs              3,915 lines   (0.91x src)   <-- nobody was counting this
```

Top-churned files. Note that the top three are documents, not code:

| Lines churned | Commits | File |
|---:|---:|---|
| 603 | 8 | `doc/spec-style.md` |
| 528 | 2 | `code-review.md` (a review artifact, committed twice) |
| 482 | 1 | `doc/spec-diff-tint-hue-fidelity.md` |
| 462 | 5 | `tests/test_name_clash.cpp` |
| 329 | 5 | `tests/test_ui_layout.cpp` |
| 328 | 6 | `tests/test_render.cpp` |

Churn of the artifacts that were *suspected* of causing the slowdown:

| File | Lines | Commits touching | Churn |
|---|---:|---:|---:|
| `tests/reference_editor.cpp` (the oracle) | 1,187 | **0** | **0** |
| `tests/test_reference_editor.cpp` | 1,296 | **0** | **0** |
| `tests/test_end_to_end.cpp` | 797 | 1 | 2 |
| `tests/test_cell_layout.cpp` | 1,066 | **0** | **0** |
| `tests/test_recovery.cpp` | 1,037 | **0** | **0** |

The oracle and the end-to-end tests are the **cheapest artifacts in the
repository**. They are large but inert. The expensive tests are the
*presentation* tests, which are re-blessed on every chrome change.

## Verdicts on the five theories

| # | Theory | Verdict |
|---|---|---|
| 1 | Oracle + end-to-end tests cause rigidity | **Refuted for the named artifacts** (0 churn). Confirmed for a category the theory did not name: presentation assertions (657 lines across 11 commits). |
| 2 | Too many tests; slow; high-value ones undiscoverable | "Slow" **refuted** (5.7s / 5,884 assertions). "Undiscoverable" **confirmed** — there is no taxonomy telling a reader what kind of test a file holds. |
| 3 | "Oracle-first" is harmful | **Confirmed as a default policy.** The harm is *scope*, not the technique: the word "strongest" applied to domains with no external truth. |
| 4 | Goldens bake in unapproved style | **Confirmed for 10 of 81 fixtures.** The other 71 encode real external contracts (wire bytes, Unicode, encodings) and must stay. |
| 5 | Class-level tests + first-order composition | **Confirmed as direction**, currently blocked: rendering cannot be tested without a filesystem because there is no `SessionSnapshotBuilder`. |

Both analysts independently reached the same correction: **the diagnosis of
"too many tests" is wrong; the diagnosis is "one fact stored in many places."**

## Phase 1 — stop the bleeding (~1.5 days, no production code touched)

Do these in order. The first item is the generator of everything else.

1. **Rewrite the `Unit tests` section and the workflow line in
   `copilot-instructions.md`** (text below). Everything else is cleanup of what
   the current wording produced.
2. **Generate the command catalog from `data/required-commands.json`.** Delete
   `kExpectedCommands`, `kExpectedCategoryCounts`, both count `static_assert`s,
   and the hand-written body of `tests/session/command_cases.h`. Use
   `configure_file`, already proven in `cmake/components/editor-session-assembly.cmake:11`.
   Adding a command drops from **13 edit sites to 2**. Removes ~410 duplicated lines.
3. **Delete the 10 arbitrary-appearance fixtures** (`fixtures/tui/*` 363 lines,
   `fixtures/ui_layout/*` 42, `fixtures/prompt_status/*` 11) and replace with
   layout/theme invariants. Delete the orphaned `fixtures/ui_layout/geometry.txt`.
4. **Delete the feature-doc command-union rule**
   (`tests/test_required_commands.cpp:329-393`). It turns prose typography into
   executable product state.
5. **Delete source-text-scraping tests** in `test_ssg_app.cpp`; renames should
   not break tests.
6. **Stop committing `code-review.md`** — 528 lines of churn from a working file.

## Phase 2 — structural (~1 week)

- **Add `SessionSnapshotBuilder`** so presentation is unit-testable without a
  filesystem. This is the prerequisite for theory 5. Rewrite `test_render.cpp`
  (1,170 → ~700) and the presentation half of `test_ui_layout.cpp`.
- **Introduce `describeFields<T>`** and derive `toValue`/`decodePresent`.
  `src/Protocol.cpp` 5,794 → ~1,500. Adding a wire field becomes one edit.
- **Shrink `test_reference_editor.cpp`** 1,296 → ~350. It tests only the oracle
  itself. Keep the oracle; shrink its self-test.
- **Retire delivered `doc/spec-*.md` into `doc/features/*.md`.** A spec is a
  plan; a delivered plan is not a document of record. 31+ specs → ~11 feature docs.

## Phase 3 — architectural (only if 1–2 do not suffice)

- Extract `DocumentEditingSession`, then `PresentationState` / `WorkspaceSession`
  / `ClientRegistry` / `CommandDispatcher` from `EditorSession` (1,911 → ~400 of
  composition). This is what finally makes class-level tests sufficient.
- Split `RecoveryManager.cpp` into `DurableFileOps` / `RecoveryJournal` /
  `RecoveryPolicy`. Do this **carefully and last** — it is the riskiest code.

## Replacement text: `copilot-instructions.md` workflow line

> Match process to risk.
>
> - **Tier 0 — mechanical:** behaviour-preserving refactors, generated metadata,
>   and commands fitting an existing seam need no specification. Review the
>   implementation once.
> - **Tier 1 — local behaviour:** a change inside one established class or seam
>   gets a short acceptance note in its owning feature document and one
>   implementation review. No separate specification review unless a public
>   contract, persistence format, security boundary, or ownership rule changes.
> - **Tier 2 — contract or architecture:** new public APIs, ownership seams,
>   wire/persistence formats, platform adapters, capability changes, and anything
>   that can lose user data get specification, specification review,
>   implementation, and code review.
>
> A specification is a current contract, not a task diary. Cap it at ~80 lines.
> Move delivered history to Git; never append review transcripts, perturbation
> logs, or repeated status sections.

## Replacement text: `copilot-instructions.md` `Unit tests` section

> ## Tests
>
> - Test each behaviour once, at the narrowest stable seam that owns it. Prefer
>   class-level tests with explicit inputs and observable outputs.
> - Every test file declares its kind in a header comment: **contract** (external
>   truth: wire bytes, Unicode, encodings, platform), **algorithm** (independently
>   knowable answer), **seam** (an architectural rule), or **smoke** (production
>   composition works at all).
> - Write an independent oracle before implementation only where the answer is
>   knowable independently of the implementation: Unicode/layout maths,
>   transactions, selection and history state machines, encoding and protocol
>   bytes, recovery, atomic file operations, or a reproduced bug. Prefer
>   properties, round-trips, hand cases and fault injection over writing a second
>   implementation.
> - Do not create an oracle, golden, or full-stack script for presentation taste,
>   internal structure, inventories, or counts. For configurable presentation,
>   assert that output follows configuration and satisfies bounds — never that it
>   equals today's appearance.
> - A golden requires an external or pinned contract, or recorded owner approval.
>   Every golden has a regeneration path. Appearance is not a contract.
> - A behaviour-preserving refactor needs no new test when existing focused tests
>   would fail on a regression. Add a test only for a discovered gap.
> - Facts live in one place. If adding a command, field, or glyph requires editing
>   a list that duplicates another list, delete the duplicate instead.
> - Before review, build the affected target and run its focused suite. Run the
>   fast gate for ordinary changes; run push, sanitizer, platform, or browser
>   gates only when the changed risk requires them.

## What must NOT change

This is a real editor that writes users' files.

1. **`tests/reference_editor.cpp` and its three consumers** (`test_document`,
   `test_selection`, `test_history`). Verified genuinely independent: 0 `ssg::`
   references, own UTF-8 scanners, `std::string` backing. Zero churn. It is why
   editing is trustworthy.
2. **`tests/test_end_to_end.cpp`.** 2 tests, 26 assertions, 2 lines of churn in
   40 commits, and the only executable proof of the single-behaviour-path
   invariant.
3. **All recovery, atomic-file, archive and platform-file-seam tests.** Durable-
   before-unlink ordering and non-clobbering renames are where data loss lives.
4. **`test_protocol.cpp` plus the protocol and encoding `.hex` goldens.** Wire
   compatibility and encoding/BOM/EOL preservation are external contracts.
5. **`test_cell_layout.cpp` and `fixtures/layout/cells/*`.** UAX #29/#11 is
   defined outside SSG. This is the textbook correct use of oracle-first.
6. **Linux/Windows adapter parity** and **`Theme` as the only colour source.**
7. **The `cmake/components/*.cmake` structure.** It is why a rebuild is 4.6s.
