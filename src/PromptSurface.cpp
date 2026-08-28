#include "ssg/PromptSurface.h"

#include "ssg/Layout.h"
#include "ssg/WholeScreenAssembly.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ssg {
namespace {

PromptCommandResult failure(PromptErrorCode code, std::string message) {
    return {PromptError{code, std::move(message)}, std::nullopt};
}

bool validIdentity(std::string_view id, std::string_view label) {
    return !id.empty() && !label.empty();
}

bool validRequest(const PromptRequest& request) {
    if (request.accessibleLabel.empty()) {
        return false;
    }

    const std::size_t expectedInputs =
        request.kind == PromptKind::Replace ? 2U : 1U;
    if (request.inputs.size() != expectedInputs) {
        return false;
    }
    for (const auto& input : request.inputs) {
        if (!validIdentity(input.id, input.accessibleLabel)) {
            return false;
        }
    }

    const bool hasOptions =
        request.kind == PromptKind::Find ||
        request.kind == PromptKind::Replace;
    if (hasOptions != request.matchCount.has_value()) {
        return false;
    }
    if (!hasOptions && !request.toggles.empty()) {
        return false;
    }
    for (const auto& toggle : request.toggles) {
        if (!validIdentity(toggle.id, toggle.accessibleLabel) ||
            toggle.width <= 0) {
            return false;
        }
    }
    if (request.matchCount &&
        !validIdentity(request.matchCount->id,
                       request.matchCount->accessibleLabel)) {
        return false;
    }

    // The layout looks controls up by id (solveLayout + SolvedLayout::find), so
    // ids must be distinct across every control; a collision would map a control
    // to the wrong rect.
    std::vector<std::string_view> ids;
    ids.reserve(request.inputs.size() + request.toggles.size() + 1);
    for (const auto& input : request.inputs) ids.push_back(input.id);
    for (const auto& toggle : request.toggles) ids.push_back(toggle.id);
    if (request.matchCount) ids.push_back(request.matchCount->id);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) {
                return false;
            }
        }
    }
    return true;
}

LayoutNode layoutNodeFor(const UiNode& node) {
    LayoutNode layout{node.id.value(), std::nullopt, node.size};
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        layout.axis = container->axis;
        layout.inset = container->inset;
        layout.children.reserve(container->children.size());
        for (const auto& child : container->children) {
            layout.children.push_back(layoutNodeFor(child));
        }
    }
    return layout;
}

const UiNode* controlNode(const UiNode& node, std::string_view controlId) {
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content);
        leaf && leaf->widget.id == controlId) {
        return &node;
    }
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (const auto* found = controlNode(child, controlId)) return found;
        }
    }
    return nullptr;
}

} // namespace

std::uint8_t promptRowCount(PromptKind kind) noexcept {
    switch (kind) {
    case PromptKind::Find: return 2;
    case PromptKind::Replace: return 3;
    case PromptKind::Path:
    case PromptKind::Settings:
    case PromptKind::CommandArgument: return 1;
    case PromptKind::Palette: return 0;  // Query renders in the header; results project into the pane.
    }
    return 1;
}

PromptCommandResult PromptSurface::open(PromptRequest request) {
    if (!validRequest(request)) {
        return failure(PromptErrorCode::InvalidRequest,
                       "prompt request does not match its kind");
    }
    // Deterministic active-input reset on every open/kind transition: Replace
    // focuses its replacement (the second input, preserving today's routing of
    // Replace text to the replacement); Find and the single-input prompts focus
    // their sole input.
    activeInput_ = request.kind == PromptKind::Replace ? 1U : 0U;
    request_ = std::move(request);
    return {};
}

PromptCommandResult PromptSurface::focusInput(std::string_view controlId) {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to focus");
    }
    const auto input =
        std::find_if(request_->inputs.begin(), request_->inputs.end(),
                     [&](const PromptInput& candidate) {
                         return candidate.id == controlId;
                     });
    if (input == request_->inputs.end()) {
        return failure(PromptErrorCode::UnknownInput,
                       "focus id does not address an input");
    }
    activeInput_ = static_cast<std::size_t>(
        std::distance(request_->inputs.begin(), input));
    return {};
}

PromptCommandResult PromptSurface::focusNextInput() {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to focus");
    }
    const std::size_t count = request_->inputs.size();
    if (count == 0) {
        return failure(PromptErrorCode::UnknownInput, "prompt has no inputs");
    }
    activeInput_ = (activeInput_ + 1) % count;
    return {};
}

PromptCommandResult PromptSurface::updateValue(std::size_t index,
                                               std::string value) {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to update");
    }
    if (index >= request_->inputs.size()) {
        return failure(PromptErrorCode::UnknownInput,
                       "prompt has no input at that index");
    }
    request_->inputs[index].value = std::move(value);
    return {std::nullopt, std::nullopt};
}

PromptCommandResult PromptSurface::submit() {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to submit");
    }
    PromptSubmission submission;
    submission.kind = request_->kind;
    submission.commandId = request_->commandId;
    for (const auto& input : request_->inputs) {
        submission.values.push_back(input.value);
    }
    for (const auto& toggle : request_->toggles) {
        submission.toggles.push_back(toggle.value);
    }
    request_.reset();
    activeInput_ = 0;
    return {std::nullopt, std::move(submission)};
}

PromptCommandResult PromptSurface::cancel() {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to cancel");
    }
    request_.reset();
    activeInput_ = 0;
    return {};
}

std::vector<PromptControl> resolvePromptControls(const PromptRequest& request) {
    // The command that OPERATES an input: find/replace inputs have dedicated
    // update commands; every generic prompt input takes the shared
    // prompt.update_value. A toggle's operating command is its own id (the
    // registered find.toggle_* command); the match count is not operable.
    const auto inputCommand = [](std::string_view id) -> std::string {
        if (id == "find.query") return "find.update_query";
        if (id == "replace.replacement") return "replace.update_replacement";
        return "prompt.update_value";
    };
    std::vector<PromptControl> controls;
    controls.reserve(request.inputs.size() + request.toggles.size() + 1);
    for (const auto& input : request.inputs) {
        controls.push_back({PromptControlKind::Input, input.id,
                            input.accessibleLabel, input.value, false,
                            inputCommand(input.id)});
    }
    if (request.matchCount) {
        for (const auto& toggle : request.toggles) {
            controls.push_back({PromptControlKind::Toggle, toggle.id,
                                toggle.accessibleLabel, {}, toggle.value,
                                toggle.id});
        }
        controls.push_back({PromptControlKind::Count, request.matchCount->id,
                            request.matchCount->accessibleLabel,
                            request.matchCount->value, false, {}});
    }
    return controls;
}

PromptLayoutResult computePromptLayout(const PromptSurface& surface,
                                       Rect reservation) {
    return computePromptLayout(surface, assembleFooterPrompt(surface),
                               reservation);
}

PromptLayoutResult computePromptLayout(const PromptSurface& surface,
                                       const UiNode& promptTree,
                                       Rect reservation) {
    if (!surface.request()) {
        return {PromptError{PromptErrorCode::NoActivePrompt,
                            "no active prompt to lay out"},
                std::nullopt};
    }
    const auto& request = *surface.request();
    if (reservation.width <= 0 || reservation.height !=
            static_cast<int>(promptRowCount(request.kind))) {
        return {PromptError{PromptErrorCode::InvalidReservation,
                            "prompt reservation does not match its kind"},
                std::nullopt};
    }

    PromptViewState view{request.kind, request.accessibleLabel, reservation, {},
                         surface.activeInput()};

    const LayoutNode root = layoutNodeFor(promptTree);
    const auto solved = solveLayout(root, reservation);
    if (!solved) {
        return {PromptError{PromptErrorCode::InvalidReservation,
                            "prompt controls exceed reservation width"},
                std::nullopt};
    }

    // The grid controls are the ONE resolver's controls plus a Rect each, in the
    // same order -- never a second content resolution.
    for (const auto& control : resolvePromptControls(request)) {
        const UiNode* node = controlNode(promptTree, control.id);
        const SolvedBox* box = node ? solved->find(node->id.value()) : nullptr;
        if (!box) {
            return {PromptError{PromptErrorCode::InvalidReservation,
                                "prompt tree does not contain a control"},
                    std::nullopt};
        }
        const Rect rect = box->rect;
        if (control.kind == PromptControlKind::Count && rect.width <= 0) {
            return {PromptError{PromptErrorCode::InvalidReservation,
                                "prompt count has no visible width"},
                    std::nullopt};
        }
        view.controls.push_back({control.kind, control.id,
                                 control.accessibleLabel, control.value,
                                 control.checked, rect});
    }
    return {std::nullopt, std::move(view)};
}

} // namespace ssg
