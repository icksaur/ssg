# performance-ci

- Spec: `doc/spec.md` Budgets and Gates
- Depends: `end-to-end-parity`
- Branch: `performance-ci-task`

## Scope

Add the pinned corpus, deterministic 10,000-operation script, benchmark
executable, sanitizer jobs, required browser gates, and Linux/Windows CI
workflows. The benchmark must:

- use a Release build, discard the first 1,000 operations, run five isolated
  repetitions, and aggregate all measured samples;
- enforce insertion/deletion below 1 ms p50 and 4 ms p99;
- enforce a command-to-delta cycle with syntax and LSP disabled below 2 ms p50
  and 8 ms p99;
- enforce opening and producing the first viewport for a 10 MiB document below
  250 ms;
- record hardware, compiler, build configuration, and build flags;
- verify that an unchanged viewport emits no cell-run payload and that an idle
  session consumes no polling CPU; and
- verify the existing bounded WebSocket queue oracle rather than implementing a
  second queue.

Absolute latency limits are enforced only on the designated project benchmark
host. Portable Linux and Windows CI execute corpus/script hash verification,
determinism, operation-count/warm-up protocol, idle-session, unchanged-viewport,
bounded-queue, and benchmark smoke checks without applying wall-clock limits.
The browser and sanitizer jobs consume the existing CTest/browser gates and the
existing `SSG_SANITIZE=ON` toggle rather than duplicating their behavior.

## Files

`benchmarks/corpus/manifest.json`, `benchmarks/corpus/mixed-code.txt`,
`benchmarks/corpus/operations.tsv`, `benchmarks/editor_benchmark.cpp`,
`.github/workflows/ci.yml`, `.github/workflows/browser.yml`,
`.github/workflows/benchmark.yml`, `cmake/components/performance-ci.cmake`

## Oracle

SHA-256 verification for the corpus and operation script; exactly 10,000
deterministic operations with 1,000 discarded warm-ups and five isolated
Release repetitions; aggregate edit and command-to-delta p50/p99 limits; the
10 MiB first-viewport limit; provenance output; zero unchanged-viewport
payload; idle-CPU and existing bounded-queue checks; plus green
sanitizer/platform/browser matrices.

## Done

The mandatory workflow is complete. Portable CI executes the aggregate
Acceptance gate set on Linux and Windows, sanitizer and required browser
matrices are explicit, and the designated benchmark-host workflow enforces all
fixed performance limits.
