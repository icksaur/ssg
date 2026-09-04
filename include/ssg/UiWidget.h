#pragma once

// The UI-VM leaf vocabulary: the widget descriptor a UiTree leaf carries.
// Structural and static presentation properties only -- a widget's semantic
// value lives solely in UiNode::resolved (see UiTree.h), never here.

#include <ssg/Widget.h>  // WidgetKind, ViewSurface, Overflow

#include <optional>
#include <string>

namespace ssg {

// One widget's structure. `width` is a left/right `Spacer`'s blank width;
// `command` the click target (validated at dispatch, not here) for a widget
// whose command is static rather than carried on its resolved value; `role`
// an authored SemanticRole name, resolved once at snapshot publication into
// `UiLeafState::role` (validated at lowering). `surface` names the
// client-rendered surface of a `View` leaf (required for a View, forbidden
// otherwise), validated at schema validation.
struct WidgetDescriptor {
    WidgetKind kind = WidgetKind::Label;
    std::string id;
    std::optional<int> width;
    std::optional<std::string> role;
    std::optional<std::string> command;
    std::optional<ViewSurface> surface;
    int rank = 0;
    bool keep = false;
    Overflow overflow = Overflow::None;
    std::string sigil;

    friend bool operator==(const WidgetDescriptor&, const WidgetDescriptor&) =
        default;
};

}  // namespace ssg
