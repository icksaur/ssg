#pragma once

// The composable box-tree layout engine (the grid solver).
//
// A LayoutNode tree is solved against a bounding Rect into a flat list of
// SolvedBoxes -- one rectangle per node, in tree (emission) order. The layout
// CONSTRAINTS the tree is built from (Axis, Size, Inset) are the medium-agnostic
// vocabulary in LayoutConstraints.h; this header is the GRID interpretation of
// them: it binds a node to a grid projection role (ShellNodeKind) and solves to
// cell rectangles (Rect). A native client consumes the same constraints without
// this solver.
//
// Everything richer -- conditional presence, min/max widths, borders -- is
// expressed by HOW THE TREE IS BUILT and rendered, not by the solver. The solver
// is a pure mechanism: it distributes space and FAILS LOUDLY (returns nullopt)
// when a container's Exact children cannot fit, rather than clamping to a garbage
// layout.

#include <ssg/LayoutConstraints.h>  // Axis, Size, Inset (medium-agnostic)
#include <ssg/ShellState.h>         // Rect, ShellNodeKind (grid)

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LayoutNode {
    std::string id;
    // The semantic role for the accessibility/field projection, or nullopt for a
    // STRUCTURAL container (root/body/content) that only groups children and is
    // never projected into the shell's manifest.
    std::optional<ShellNodeKind> kind;
    Size size;
    Axis axis = Axis::Column;  // how THIS node arranges its own children
    Inset inset;
    std::vector<LayoutNode> children;
};

struct SolvedBox {
    std::string id;
    std::optional<ShellNodeKind> kind;  // nullopt for a structural container
    Rect rect;                          // the node's full solved rectangle

    friend bool operator==(const SolvedBox&, const SolvedBox&) = default;
};

struct SolvedLayout {
    std::vector<SolvedBox> boxes;  // one per node, in tree (emission) order

    [[nodiscard]] const SolvedBox* find(std::string_view id) const noexcept;
};

// Solve `root` within `bounds`. Returns nullopt when any container's Exact
// children exceed its available main-axis cells (the layout does not fit).
[[nodiscard]] std::optional<SolvedLayout> solveLayout(const LayoutNode& root,
                                                      Rect bounds);

}  // namespace ssg
