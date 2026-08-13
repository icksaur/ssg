#pragma once

// Lowering a composed chrome region tree to accessibility nodes over a rect,
// reusing the shipped `WidgetStack`. The medium-agnostic UiRegion the runtime
// publishes is lowered to the SAME `(kind,id,label,rect,role,content,commandId)`
// nodes the built-in status-field projection emits, so a composed region is a
// transparent replacement.

#include <ssg/ShellState.h>  // AccessibilityNode, ShellNodeKind, Rect
#include <ssg/Style.h>
#include <ssg/UiTree.h>      // UiRegion
#include <ssg/UiWidget.h>    // ChromeProviderResolver

#include <optional>
#include <string>
#include <vector>

namespace ssg {

// The result of lowering a medium-agnostic chrome region tree: on a malformed
// tree shape, a named error and no nodes emitted (fail-loud, never a plausible
// partial); otherwise the row's consumed right edge -- the absolute right edge
// (`rect.x + consumed width`) of the resolved row, INCLUDING space consumed by
// node-less `Spacer`s, so a caller placing content after the group (the header
// input line) advances past spacer cells, not merely past the last emitted node.
struct UiChromeLowerResult {
    std::optional<std::string> error;
    int rightEdge = 0;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Lower a medium-agnostic chrome region (the canonical tree the chrome decoder
// produces) DIRECTLY into accessibility nodes over `rect`, reading the
// left/center/right groups, the separator (the left group's gap), and the center
// width policy (the center leaf's Size) from the tree itself. The tree must be the
// canonical chrome shape (a root container of exactly three group containers); a
// malformed tree returns a named error and emits nothing.
[[nodiscard]] UiChromeLowerResult lowerUiChromeRegion(
    const UiRegion& region, const Rect& rect, ShellNodeKind nodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out);

}  // namespace ssg
