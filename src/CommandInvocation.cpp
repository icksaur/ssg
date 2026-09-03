#include <ssg/CommandInvocation.h>

namespace ssg {

void CommandContext::setActiveWorkspace(WorkspaceId workspace) noexcept {
    workspaceChanged_ = true;
    activeWorkspace_ = workspace;
}

void CommandContext::setActiveView(ViewId view) noexcept {
    viewChanged_ = true;
    activeView_ = view;
}

CommandHandlerResult CommandHandlerResult::success() {
    return {true, {}, std::nullopt};
}

CommandHandlerResult CommandHandlerResult::failure(std::string message) {
    return {false, std::move(message), std::nullopt};
}

CommandHandlerResult CommandHandlerResult::requireView(ViewAction action) {
    return {true, {}, std::move(action)};
}

}  // namespace ssg
