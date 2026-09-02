#pragma once

#include <ssg/ClientInput.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

enum class ViewActionStatus : std::uint8_t {
    Applied,
    TransitionRequired,
    Rejected,
};

struct ViewActionResult {
    ViewActionStatus status = ViewActionStatus::Rejected;
    std::optional<ClientInput> transition;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return status != ViewActionStatus::Rejected;
    }
};

}  // namespace ssg
