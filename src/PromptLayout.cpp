#include <ssg/PromptLayout.h>

#include <ssg/Layout.h>
#include <ssg/WholeScreenAssembly.h>

#include <utility>
#include <variant>

namespace ssg {
namespace {

LayoutNode layoutNodeFor(const UiNode& node) {
    LayoutNode layout{node.id, node.size};
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        layout.axis = container->axis;
        layout.inset = container->inset;
        layout.gap = container->gap;
        layout.scroll = container->scroll;
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

}  // namespace

std::uint8_t promptRowCount(PromptKind kind) noexcept {
    switch (kind) {
    case PromptKind::Find: return 2;
    case PromptKind::Replace: return 3;
    case PromptKind::Path:
    case PromptKind::Settings:
    case PromptKind::CommandArgument: return 1;
    case PromptKind::Palette: return 0;
    }
    return 1;
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
    if (reservation.width <= 0 ||
        reservation.height != static_cast<int>(promptRowCount(request.kind))) {
        return {PromptError{PromptErrorCode::InvalidReservation,
                            "prompt reservation does not match its kind"},
                std::nullopt};
    }

    PromptViewState view{request.kind, request.accessibleLabel, reservation, {},
                         surface.activeInput()};
    const LayoutNode root = layoutNodeFor(promptTree);
    const auto solved = solveGridTree(root, reservation);
    if (!solved) {
        return {PromptError{PromptErrorCode::InvalidReservation,
                            "prompt controls exceed reservation width"},
                std::nullopt};
    }

    for (const auto& control : resolvePromptControls(request)) {
        const UiNode* node = controlNode(promptTree, control.id);
        const SolvedGridNode* box = node ? solved->find(node->id) : nullptr;
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

}  // namespace ssg
