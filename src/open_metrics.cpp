#include <ssg/open_metrics.h>

#include <array>
#include <chrono>
#include <cstddef>

namespace ssg {
namespace {

[[nodiscard]] std::int64_t monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t phase_count = static_cast<std::size_t>(OpenPhase::Count);

thread_local std::array<std::uint64_t, phase_count> g_phase_ns{};
thread_local std::uint64_t g_utf8_validation_calls = 0;
thread_local std::uint64_t g_piece_tree_text_calls = 0;

}  // namespace

OpenPhaseTimer::OpenPhaseTimer(OpenPhase phase) noexcept
    : phase_(phase), start_ns_(monotonicNs()) {}

OpenPhaseTimer::~OpenPhaseTimer() {
    g_phase_ns[static_cast<std::size_t>(phase_)] +=
        static_cast<std::uint64_t>(monotonicNs() - start_ns_);
}

std::uint64_t openPhaseNs(OpenPhase phase) {
    return g_phase_ns[static_cast<std::size_t>(phase)];
}

void resetOpenPhaseTiming() { g_phase_ns.fill(0); }

void noteUtf8Validation() { ++g_utf8_validation_calls; }
std::uint64_t utf8ValidationCalls() { return g_utf8_validation_calls; }
void resetUtf8ValidationCalls() { g_utf8_validation_calls = 0; }

void notePieceTreeText() { ++g_piece_tree_text_calls; }
std::uint64_t pieceTreeTextCalls() { return g_piece_tree_text_calls; }
void resetPieceTreeTextCalls() { g_piece_tree_text_calls = 0; }

}  // namespace ssg
