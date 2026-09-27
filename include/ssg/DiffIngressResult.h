#pragma once

namespace ssg {

enum class DiffIngressError {
    None,
    EmptyBurst,
    DiffRejected,
    FollowRejected,
};

struct DiffIngressResult {
    DiffIngressError error = DiffIngressError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == DiffIngressError::None;
    }
};

} // namespace ssg
