#include "open_metrics.h"

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

constexpr std::size_t kPhaseCount = static_cast<std::size_t>(OpenPhase::Count);

thread_local std::array<std::uint64_t, kPhaseCount> gPhaseNs{};
thread_local std::uint64_t gUtf8ValidationCalls = 0;
thread_local std::uint64_t gPieceTreeTextCalls = 0;

}  // namespace

OpenPhaseTimer::OpenPhaseTimer(OpenPhase phase) noexcept
    : phase_(phase), startNs_(monotonicNs()) {}

OpenPhaseTimer::~OpenPhaseTimer() {
    gPhaseNs[static_cast<std::size_t>(phase_)] +=
        static_cast<std::uint64_t>(monotonicNs() - startNs_);
}

std::uint64_t openPhaseNs(OpenPhase phase) {
    return gPhaseNs[static_cast<std::size_t>(phase)];
}

void resetOpenPhaseTiming() { gPhaseNs.fill(0); }

void noteUtf8Validation() { ++gUtf8ValidationCalls; }
std::uint64_t utf8ValidationCalls() { return gUtf8ValidationCalls; }
void resetUtf8ValidationCalls() { gUtf8ValidationCalls = 0; }

void notePieceTreeText() { ++gPieceTreeTextCalls; }
std::uint64_t pieceTreeTextCalls() { return gPieceTreeTextCalls; }
void resetPieceTreeTextCalls() { gPieceTreeTextCalls = 0; }

}  // namespace ssg
