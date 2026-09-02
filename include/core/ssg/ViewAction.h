#pragma once

#include <ssg/PaneNavigation.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>
#include <ssg/types.h>

#include <cstdint>
#include <variant>

namespace ssg {

enum class ScrollTarget : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_SCROLL_TARGET_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_SCROLL_TARGET_ENUMERATORS

struct ScrollLines {
    ScrollTarget target = ScrollTarget::Document;
    std::int64_t rows = 0;

    friend bool operator==(const ScrollLines&, const ScrollLines&) = default;
};

struct ScrollPages {
    std::int64_t pages = 0;

    friend bool operator==(const ScrollPages&, const ScrollPages&) = default;
};

struct ScrollFraction {
    ScrollTarget target = ScrollTarget::Document;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    friend bool operator==(const ScrollFraction&,
                           const ScrollFraction&) = default;
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

struct ResolvePaneFocus {
    PaneDirection direction = PaneDirection::Left;

    friend bool operator==(const ResolvePaneFocus&,
                           const ResolvePaneFocus&) = default;
};

enum class DocumentPointerEdge : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_DOCUMENT_POINTER_EDGE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_DOCUMENT_POINTER_EDGE_ENUMERATORS

struct ContinuePointerEdge {
    DocumentPointerEdge direction = DocumentPointerEdge::After;

    friend bool operator==(const ContinuePointerEdge&,
                           const ContinuePointerEdge&) = default;
};

using ViewAction =
    std::variant<ScrollLines, ScrollPages, ScrollFraction,
                 MoveVisualSelection, RevealSelection, CenterSelection,
                 ResolvePaneFocus,
                 ContinuePointerEdge>;

}  // namespace ssg
