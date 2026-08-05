#pragma once

// Lowering a composed chrome row (doc/spec-lua-widget-composition.md, phase 2)
// to accessibility nodes over a rect, reusing the shipped `WidgetStack`. This is
// the server-side bridge from a validated `RowDescriptor` to the SAME
// `(kind,id,label,rect,role,content,commandId)` nodes the built-in status-field
// projection emits, so a composed region is a transparent replacement.

#include <ssg/ChromeComposition.h>  // RowDescriptor, ChromeProviderResolver
#include <ssg/ShellState.h>  // AccessibilityNode, ShellNodeKind, Rect
#include <ssg/Style.h>

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

}  // namespace ssg
