#include "ssg/PromptSurface.h"

#include "ssg/Layout.h"

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

    // The prompt is a widget Container (doc/spec-widget-chrome.md): a Column of
    // full-width input rows, plus -- for find/replace -- a trailing options Row
    // of fixed-width toggles (Checkbox widgets) and a flex match-count Label. The
    // box solver assigns every rect and fails loud when the toggles overflow the
    // options row, which is exactly the "controls exceed reservation width" the
    // procedural placement reported. Reading rects back by control id is safe
    // because validRequest enforces control-id distinctness and the structural
    // containers carry empty ids (so they can never shadow a control).
    const auto leaf = [](std::string id, Size size) {
        return LayoutNode{std::move(id), std::nullopt, size, Axis::Row, {}, {}};
    };
    std::vector<LayoutNode> rows;
    rows.reserve(request.inputs.size() + 1);
    for (const auto& input : request.inputs)
        rows.push_back(leaf(input.id, Size::exact(1)));  // full width, one row

    if (request.matchCount) {
        std::vector<LayoutNode> options;
        options.reserve(request.toggles.size() + 1);
        for (const auto& toggle : request.toggles)
            options.push_back(leaf(toggle.id, Size::exact(toggle.width)));
        options.push_back(leaf(request.matchCount->id, Size::flex()));
        rows.push_back(LayoutNode{"", std::nullopt, Size::exact(1),
                                  Axis::Row, {}, std::move(options)});
    }

    LayoutNode root{"", std::nullopt, Size::flex(), Axis::Column, {},
                    std::move(rows)};
    const auto solved = solveLayout(root, reservation);
    if (!solved) {
        return {PromptError{PromptErrorCode::InvalidReservation,
                            "prompt controls exceed reservation width"},
                std::nullopt};
    }

    for (const auto& input : request.inputs) {
        view.controls.push_back({PromptControlKind::Input, input.id,
                                 input.accessibleLabel, input.value, false,
                                 solved->find(input.id)->rect});
    }
    if (request.matchCount) {
        for (const auto& toggle : request.toggles) {
            view.controls.push_back({PromptControlKind::Toggle, toggle.id,
                                     toggle.accessibleLabel, {}, toggle.value,
                                     solved->find(toggle.id)->rect});
        }
        const Rect countRect = solved->find(request.matchCount->id)->rect;
        if (countRect.width <= 0) {
            return {PromptError{PromptErrorCode::InvalidReservation,
                                "prompt count has no visible width"},
                    std::nullopt};
        }
        view.controls.push_back({PromptControlKind::Count,
                                 request.matchCount->id,
                                 request.matchCount->accessibleLabel,
                                 request.matchCount->value, false, countRect});
    }
    return {std::nullopt, std::move(view)};
}

} // namespace ssg
