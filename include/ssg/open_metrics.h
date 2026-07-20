#pragma once

// Test/benchmark-only instrumentation for the file-open path (Milestone 13,
// doc/spec-large-files-loading.md).  Every symbol here is a thread-local
// diagnostic counter/timer, reset/read exactly like layout.h's cell_run_calls;
// none of it is production state and no correctness logic reads it.  It lets the
// LF measurement + oracle tests attribute the open cost and prove the
// single-validation (INV-single-validation) and no-fresh-open-materialize
// (INV-no-fresh-open-materialize) invariants dynamically.

#include <cstdint>

namespace ssg {

// The ordered whole-document passes on the open path (the audit A-J collapsed to
// the phases the LF-1 cross-layer seam brackets).  Phases are non-overlapping.
enum class OpenPhase : unsigned {
    Read,               // disk read into the raw byte buffer
    NulScan,           // whole-raw-byte NUL detection
    DecodeValidate,    // UTF-8/UTF-16 validating scan -> scalars
    EolScan,           // EOL normalization -> line_terminators + utf8
    DocumentBuild,     // Document + PieceTree construction
    StateDirtyCheck,  // Workspace::state() snapshot-materialize + compare
    Count
};

// Accumulates elapsed steady-clock nanoseconds into the current thread's total
// for `phase` over the guard's lifetime.  Use only around non-overlapping
// phases; nesting the same phase double-counts.
class OpenPhaseTimer {
public:
    explicit OpenPhaseTimer(OpenPhase phase) noexcept;
    ~OpenPhaseTimer();
    OpenPhaseTimer(const OpenPhaseTimer&) = delete;
    OpenPhaseTimer& operator=(const OpenPhaseTimer&) = delete;

private:
    OpenPhase phase_;
    std::int64_t start_ns_;
};

[[nodiscard]] std::uint64_t openPhaseNs(OpenPhase phase);
void resetOpenPhaseTiming();

// Counts UTF-8 *validating scans* on the current thread (the decoder's scan plus
// any Document-ctor re-validation).  A direct-UTF-8 open is 2 today and 1 after
// LF-3a; a transcoded/binary open is 0 validating UTF-8 scans.
void noteUtf8Validation();
[[nodiscard]] std::uint64_t utf8ValidationCalls();
void resetUtf8ValidationCalls();

// Counts whole-document PieceTree::text() materializations on the current
// thread.  A fresh open attributes 1 to Workspace::state() today and 0 after
// LF-4b.
void notePieceTreeText();
[[nodiscard]] std::uint64_t pieceTreeTextCalls();
void resetPieceTreeTextCalls();

}  // namespace ssg
