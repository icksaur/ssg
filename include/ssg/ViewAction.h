#pragma once

#include <ssg/PaneNavigation.h>
#include <ssg/types.h>

#include <cstdint>
#include <variant>

namespace ssg {

enum class ScrollTarget : std::uint8_t {
    Document = 0,
    Tree = 1,
};

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
    LineUp = 0,
    LineDown = 1,
    PageUp = 2,
    PageDown = 3,
};

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
    None = 0,
    Before = 1,
    After = 2,
};

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
