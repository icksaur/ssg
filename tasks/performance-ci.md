# performance-ci

- Spec: `doc/spec.md` Budgets and Gates
- Depends: `end-to-end-parity`
- Branch: `performance-ci-task`

## Scope

Add the pinned corpus, deterministic operation script, benchmark executable,
sanitizer jobs, required browser gates, and Linux/Windows CI workflows.

## Files

`benchmarks/corpus/`, `benchmarks/editor_benchmark.cpp`,
`.github/workflows/`, `cmake/components/performance-ci.cmake`

## Oracle

Corpus hash verification, five-run warm benchmark limits, idle-CPU and bounded
queue checks, plus green sanitizer/platform/browser matrices.

## Done

The mandatory workflow is complete and every project Acceptance gate is
automated.
