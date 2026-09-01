#pragma once

#include <ssg/PaneNavigation.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>
#include <ssg/types.h>

#include <cstdint>
#include <variant>

namespace ssg {

enum class ViewActionKind : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_VIEW_ACTION_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_VIEW_ACTION_KIND_ENUMERATORS

enum class ViewScrollTarget : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_VIEW_SCROLL_TARGET_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_VIEW_SCROLL_TARGET_ENUMERATORS

struct ViewScrollLines {
    ViewScrollTarget target = ViewScrollTarget::Document;
    std::int64_t rows = 0;

    friend bool operator==(const ViewScrollLines&,
                           const ViewScrollLines&) = default;
};

struct ViewScrollPages {
    std::int64_t pages = 0;

    friend bool operator==(const ViewScrollPages&,
                           const ViewScrollPages&) = default;
};

struct ViewScrollFraction {
    ViewScrollTarget target = ViewScrollTarget::Document;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    friend bool operator==(const ViewScrollFraction&,
                           const ViewScrollFraction&) = default;
};

enum class VisualSelectionDirection : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_VISUAL_SELECTION_DIRECTION_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_VISUAL_SELECTION_DIRECTION_ENUMERATORS

struct MoveVisualSelection {
    VisualSelectionDirection direction = VisualSelectionDirection::LineDown;
    bool extend = false;

    friend bool operator==(const MoveVisualSelection&,
                           const MoveVisualSelection&) = default;
};

struct RevealSelection {
    friend bool operator==(const RevealSelection&,
                           const RevealSelection&) = default;
};

struct CenterSelection {
    friend bool operator==(const CenterSelection&,
                           const CenterSelection&) = default;
};

struct SplitPane {
    SplitAxis axis = SplitAxis::Horizontal;

    friend bool operator==(const SplitPane&, const SplitPane&) = default;
};

struct ClosePane {
    friend bool operator==(const ClosePane&, const ClosePane&) = default;
};

enum class PaneCycleDirection : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_PANE_CYCLE_DIRECTION_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_PANE_CYCLE_DIRECTION_ENUMERATORS

struct CyclePane {
    PaneCycleDirection direction = PaneCycleDirection::Next;

    friend bool operator==(const CyclePane&, const CyclePane&) = default;
};

struct FocusPane {
    PaneDirection direction = PaneDirection::Left;

    friend bool operator==(const FocusPane&, const FocusPane&) = default;
};

enum class PointerEdgeDirection : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_POINTER_EDGE_DIRECTION_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_POINTER_EDGE_DIRECTION_ENUMERATORS

struct ContinuePointerEdge {
    PointerEdgeDirection direction = PointerEdgeDirection::After;

    friend bool operator==(const ContinuePointerEdge&,
                           const ContinuePointerEdge&) = default;
};

using ViewAction =
    std::variant<ViewScrollLines, ViewScrollPages, ViewScrollFraction,
                 MoveVisualSelection, RevealSelection, CenterSelection,
                 SplitPane, ClosePane, CyclePane, FocusPane,
                 ContinuePointerEdge>;

}  // namespace ssg
