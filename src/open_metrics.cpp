#include <ssg/open_metrics.h>

#include <array>
#include <chrono>
#include <cstddef>

namespace ssg {
namespace {

[[nodiscard]] std::int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t phase_count = static_cast<std::size_t>(OpenPhase::count);

thread_local std::array<std::uint64_t, phase_count> g_phase_ns{};
thread_local std::uint64_t g_utf8_validation_calls = 0;
thread_local std::uint64_t g_piece_tree_text_calls = 0;

}  // namespace

OpenPhaseTimer::OpenPhaseTimer(OpenPhase phase) noexcept
    : phase_(phase), start_ns_(monotonic_ns()) {}

OpenPhaseTimer::~OpenPhaseTimer() {
    g_phase_ns[static_cast<std::size_t>(phase_)] +=
        static_cast<std::uint64_t>(monotonic_ns() - start_ns_);
}

std::uint64_t open_phase_ns(OpenPhase phase) {
    return g_phase_ns[static_cast<std::size_t>(phase)];
}

void reset_open_phase_timing() { g_phase_ns.fill(0); }

void note_utf8_validation() { ++g_utf8_validation_calls; }
std::uint64_t utf8_validation_calls() { return g_utf8_validation_calls; }
void reset_utf8_validation_calls() { g_utf8_validation_calls = 0; }

void note_piece_tree_text() { ++g_piece_tree_text_calls; }
std::uint64_t piece_tree_text_calls() { return g_piece_tree_text_calls; }
void reset_piece_tree_text_calls() { g_piece_tree_text_calls = 0; }

}  // namespace ssg
