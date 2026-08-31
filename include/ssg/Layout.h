#pragma once

// The composable box-tree layout engine (the grid solver).
//
// A LayoutNode tree is solved against a bounding Rect into one SolvedGridNode
// per UiNodeId, in tree order. This is the grid interpretation of the
// medium-agnostic UI constraint vocabulary.
//
// Everything richer -- conditional presence, min/max widths, borders -- is
// expressed by HOW THE TREE IS BUILT and rendered, not by the solver. The solver
// is a pure mechanism: it distributes space and FAILS LOUDLY (returns nullopt)
// when a container's Exact children cannot fit, rather than clamping to a garbage
// layout.

#include <ssg/Geometry.h>
#include <ssg/UiTree.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LayoutNode {
    UiNodeId id;
    Size size;
    Axis axis = Axis::Column;
    Inset inset;
    std::vector<LayoutNode> children;
    Gap gap;
    ScrollAxis scroll = ScrollAxis::None;
};

struct SolvedGridNode {
    UiNodeId id;
    Rect rect;
    Rect content;
    ScrollAxis scroll = ScrollAxis::None;

    friend bool operator==(const SolvedGridNode&,
                           const SolvedGridNode&) = default;
};

struct SolvedGridTree {
    std::vector<SolvedGridNode> nodes;

    [[nodiscard]] const SolvedGridNode* find(
        const UiNodeId& id) const noexcept;
};

// Returns nullopt when nonnegative bounds cannot contain an inset, gaps, or
// exact children. Invalid identities and unsupported Auto sizes are misuse and
// throw std::invalid_argument.
[[nodiscard]] std::optional<SolvedGridTree> solveGridTree(
    const LayoutNode& root, Rect bounds);

}  // namespace ssg
