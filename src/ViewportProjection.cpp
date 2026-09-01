#include <ssg/ViewportProjection.h>

#include "viewport_projection_state.h"

namespace ssg {

ViewportProjectionState::ViewportProjectionState()
    : impl_{std::make_unique<Impl>()} {}
ViewportProjectionState::~ViewportProjectionState() = default;
ViewportProjectionState::ViewportProjectionState(
    ViewportProjectionState&&) noexcept = default;
ViewportProjectionState& ViewportProjectionState::operator=(
    ViewportProjectionState&&) noexcept = default;

}  // namespace ssg
