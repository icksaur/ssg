#pragma once

// Lowering a composed chrome row
// to accessibility nodes over a rect, reusing the shipped `WidgetStack`. This is
// the server-side bridge from a validated `RowDescriptor` to the SAME
// `(kind,id,label,rect,role,content,commandId)` nodes the built-in status-field
// projection emits, so a composed region is a transparent replacement.

#include <ssg/ChromeComposition.h>  // RowDescriptor, ChromeProviderResolver
#include <ssg/ShellState.h>  // AccessibilityNode, ShellNodeKind, Rect
#include <ssg/Style.h>
#include <ssg/UiTree.h>  // UiRegion

#include <optional>
#include <string>
#include <vector>

namespace ssg {

// Lower `row` into accessibility nodes over `rect`, APPENDED to `out`. Builds a
// `WidgetStack` from the descriptors (each widget's content resolved from its
// literal or its provider), resolves it, and emits one node per placed widget in
// left → center → right order. `nodeKind`/`defaultRole` are the region's node
// kind and fallback role (a widget's own `role`, when a valid SemanticRole name,
// overrides). `style` measures field cells and supplies checkbox glyphs. A
// provider widget resolving to empty (or an unknown provider) is skipped, and a
// `Field`/`Label` whose resolved text is empty is skipped, matching the built-in
// drop of empty-value fields.
//
// Returns the absolute right edge (`rect.x + consumed width`) of the RESOLVED
// row, INCLUDING space consumed by node-less `Spacer`s -- so a caller that must
// place something after the composed group (the header input line) advances past
// spacer cells, not merely past the last emitted node. `rect.x` when nothing is
// placed.
int lowerChromeRow(const RowDescriptor& row, const Rect& rect,
                   ShellNodeKind nodeKind, SemanticRole defaultRole,
                   const Style& style,
                   const ChromeProviderResolver& resolveProvider,
                   std::vector<AccessibilityNode>& out);

// The result of lowering a medium-agnostic chrome region tree: on a malformed
// tree shape, a named error and no nodes emitted (fail-loud, never a plausible
// partial); otherwise the row's consumed right edge, as lowerChromeRow returns.
struct UiChromeLowerResult {
    std::optional<std::string> error;
    int rightEdge = 0;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Lower a medium-agnostic chrome region (built by uiChromeRegionFromRow) DIRECTLY
// into accessibility nodes over `rect`, reading the left/center/right groups, the
// separator (the left group's gap), and the center width policy (the center
// leaf's Size) from the tree itself -- no RowDescriptor reconstruction. The tree
// must be the canonical chrome shape (a root container of exactly three group
// containers); a malformed tree returns a named error and emits nothing.
[[nodiscard]] UiChromeLowerResult lowerUiChromeRegion(
    const UiRegion& region, const Rect& rect, ShellNodeKind nodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out);

}  // namespace ssg
