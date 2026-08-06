#include <ssg/CommandInvocation.h>


#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ssg {

CapabilityId::CapabilityId(std::string value) : value_{std::move(value)} {
    if (value_.empty()) {
        throw std::invalid_argument{"capability ID must not be empty"};
    }
}

InvocationPrincipal::InvocationPrincipal(
    ClientId clientId, InvocationOrigin origin,
    std::vector<CapabilityId> capabilities)
    : clientId_{clientId},
      origin_{origin},
      capabilities_{std::move(capabilities)} {
    std::sort(capabilities_.begin(), capabilities_.end());
    capabilities_.erase(
        std::unique(capabilities_.begin(), capabilities_.end()),
        capabilities_.end());
}

bool InvocationPrincipal::hasCapability(
    CapabilityId const& capability) const {
    return std::binary_search(capabilities_.begin(), capabilities_.end(),
                              capability);
}

void CommandContext::setActiveWorkspace(WorkspaceId workspace) noexcept {
    workspaceChanged_ = true;
    activeWorkspace_ = workspace;
}

void CommandContext::setActiveView(ViewId view) noexcept {
    viewChanged_ = true;
    activeView_ = view;
}

CommandHandlerResult CommandHandlerResult::success() {
    return {true, {}};
}

CommandHandlerResult CommandHandlerResult::failure(std::string message) {
    return {false, std::move(message)};
}

}  // namespace ssg
