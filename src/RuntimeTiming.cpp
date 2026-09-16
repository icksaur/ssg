#include <ssg/RuntimeTiming.h>

#include <algorithm>

namespace ssg {
namespace {

constexpr auto kEscapeDisambiguation = std::chrono::milliseconds{30};
constexpr auto kEdgeScrollCadence = std::chrono::milliseconds{40};

void shorten(std::optional<std::chrono::milliseconds>& timeout,
             std::chrono::milliseconds candidate) {
    candidate = std::max(candidate, std::chrono::milliseconds{0});
    if (!timeout || candidate < *timeout) timeout = candidate;
}

} // namespace

std::optional<std::chrono::milliseconds>
selectRuntimeWaitTimeout(RuntimeTiming timing) noexcept {
    if (timing.ordinaryTimeout) {
        timing.ordinaryTimeout =
            std::max(*timing.ordinaryTimeout, std::chrono::milliseconds{0});
    }
    if (timing.escapeSequencePending) {
        shorten(timing.ordinaryTimeout, kEscapeDisambiguation);
    }
    if (timing.edgeScrollActive) {
        shorten(timing.ordinaryTimeout, kEdgeScrollCadence);
    }
    if (timing.workspaceSearchPending) {
        shorten(timing.ordinaryTimeout, std::chrono::milliseconds{0});
    }
    return timing.ordinaryTimeout;
}

} // namespace ssg
