#pragma once

#include <ssg/ClientInput.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

enum class GridActionStatus : std::uint8_t {
    Applied,
    TransitionRequired,
    Rejected,
};

struct GridActionResult {
    GridActionStatus status = GridActionStatus::Rejected;
    std::optional<ClientInput> transition;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return status != GridActionStatus::Rejected;
    }
};

}  // namespace ssg
