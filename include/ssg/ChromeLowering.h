#pragma once

// Lowering a composed chrome row (doc/spec-lua-widget-composition.md, phase 2)
// to accessibility nodes over a rect, reusing the shipped `WidgetStack`. This is
// the server-side bridge from a validated `RowDescriptor` to the SAME
// `(kind,id,label,rect,role,content,commandId)` nodes the built-in status-field
// projection emits, so a composed region is a transparent replacement.

#include <ssg/ChromeComposition.h>
#include <ssg/ShellState.h>  // AccessibilityNode, ShellNodeKind, Rect
#include <ssg/Style.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// What a live provider resolves to for a composed widget: the displayed value,
// its accessible label, and any inherited click command -- mirroring a built-in
// status field's (value, accessibleLabel, commandId). A provider that has no
// value returns nullopt, and the widget is dropped (as the built-in drops an
// empty-value field).
struct ResolvedProvider {
    std::string value;
    std::string accessibleLabel;
    std::optional<std::string> commandId;
};

using ChromeProviderResolver =
    std::function<std::optional<ResolvedProvider>(std::string_view id)>;

// Lower `row` into accessibility nodes over `rect`, APPENDED to `out`. Builds a
// `WidgetStack` from the descriptors (each widget's content resolved from its
// literal or its provider), resolves it, and emits one node per placed widget in
// left → center → right order. `nodeKind`/`defaultRole` are the region's node
// kind and fallback role (a widget's own `role`, when a valid SemanticRole name,
// overrides). `style` measures field cells and supplies checkbox glyphs. A
// provider widget resolving to empty (or an unknown provider) is skipped, and a
// `Field`/`Label` whose resolved text is empty is skipped, matching the built-in
// drop of empty-value fields.
void lowerChromeRow(const RowDescriptor& row, const Rect& rect,
                    ShellNodeKind nodeKind, SemanticRole defaultRole,
                    const Style& style,
                    const ChromeProviderResolver& resolveProvider,
                    std::vector<AccessibilityNode>& out);

}  // namespace ssg
