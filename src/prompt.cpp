#include "ssg/prompt.h"

#include <utility>

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
    return !request.matchCount ||
           validIdentity(request.matchCount->id,
                          request.matchCount->accessibleLabel);
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
    request_ = std::move(request);
    return {};
}

PromptCommandResult PromptSurface::submit() {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to submit");
    }
    PromptSubmission submission;
    submission.kind = request_->kind;
    for (const auto& input : request_->inputs) {
        submission.values.push_back(input.value);
    }
    for (const auto& toggle : request_->toggles) {
        submission.toggles.push_back(toggle.value);
    }
    request_.reset();
    return {std::nullopt, std::move(submission)};
}

PromptCommandResult PromptSurface::cancel() {
    if (!request_) {
        return failure(PromptErrorCode::NoActivePrompt,
                       "no active prompt to cancel");
    }
    request_.reset();
    return {};
}

PromptLayoutResult computePromptLayout(const PromptSurface& surface,
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

    PromptViewState view{request.kind, request.accessibleLabel, reservation, {}};
    for (std::size_t i = 0; i < request.inputs.size(); ++i) {
        const auto& input = request.inputs[i];
        view.controls.push_back(
            {PromptControlKind::Input, input.id, input.accessibleLabel,
             input.value, false,
             Rect{reservation.x, reservation.y + static_cast<int>(i),
                  reservation.width, 1}});
    }

    if (request.matchCount) {
        const int optionsY = reservation.bottom() - 1;
        int x = reservation.x;
        for (const auto& toggle : request.toggles) {
            if (toggle.width > reservation.right() - x) {
                return {PromptError{PromptErrorCode::InvalidReservation,
                                    "prompt controls exceed reservation width"},
                        std::nullopt};
            }
            view.controls.push_back(
                {PromptControlKind::Toggle, toggle.id,
                 toggle.accessibleLabel, {}, toggle.value,
                 Rect{x, optionsY, toggle.width, 1}});
            x += toggle.width;
        }
        const int remaining = reservation.right() - x;
        if (remaining <= 0) {
            return {PromptError{PromptErrorCode::InvalidReservation,
                                "prompt count has no visible width"},
                    std::nullopt};
        }
        view.controls.push_back(
            {PromptControlKind::Count, request.matchCount->id,
             request.matchCount->accessibleLabel,
             request.matchCount->value, false,
             Rect{x, optionsY, remaining, 1}});
    }
    return {std::nullopt, std::move(view)};
}

} // namespace ssg
