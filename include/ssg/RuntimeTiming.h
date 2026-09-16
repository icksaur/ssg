#pragma once

#include <chrono>
#include <optional>

namespace ssg {

struct RuntimeTiming {
    std::optional<std::chrono::milliseconds> ordinaryTimeout;
    bool escapeSequencePending = false;
    bool edgeScrollActive = false;
    bool workspaceSearchPending = false;
};

[[nodiscard]] std::optional<std::chrono::milliseconds>
selectRuntimeWaitTimeout(RuntimeTiming timing) noexcept;

} // namespace ssg
