#pragma once

// The composable box-tree layout engine.
//
// A LayoutNode tree is solved against a bounding Rect into a flat list of
// SolvedBoxes -- one rectangle per node, in tree (emission) order. The vocabulary
// is deliberately minimal: a node has a main-axis size (Exact cells or Flex share
// of the remainder), an axis for arranging its own children, an optional inset
// (cells reserved inside the frame before children are placed), and children.
//
// Everything richer -- conditional presence, min/max widths, borders -- is
// expressed by HOW THE TREE IS BUILT and rendered, not by the solver. The solver
// is a pure mechanism: it distributes space and FAILS LOUDLY (returns nullopt)
// when a container's Exact children cannot fit, rather than clamping to a garbage
// layout.

#include <ssg/ShellState.h>  // Rect, ShellNodeKind

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// How a container arranges its children. Row: children share WIDTH, placed
// left-to-right. Column: children share HEIGHT, placed top-to-bottom.
enum class Axis : std::uint8_t { Row, Column };

enum class SizeKind : std::uint8_t { Exact, Flex };

// A node's size along its PARENT's axis. Exact reserves `cells`; Flex takes an
// equal share of whatever remains after the Exact siblings are placed.
struct Size {
    SizeKind kind = SizeKind::Flex;
    int cells = 0;  // main-axis cells when kind == Exact (cell counts are int
                    // everywhere, like Rect, so a large style dimension cannot
                    // wrap before it is solved)

    [[nodiscard]] static Size flex() noexcept { return {SizeKind::Flex, 0}; }
    [[nodiscard]] static Size exact(int n) noexcept {
        return {SizeKind::Exact, n};
    }

    friend bool operator==(const Size&, const Size&) = default;
};

// Cells reserved inside a node's frame before its children are laid out. Zero in
// the V1 shell; the V2 border mechanism (a bordered box is inset + a child, and
// the border ring is the frame minus the child's rect).
struct Inset {
    int left = 0, right = 0, top = 0, bottom = 0;

    friend bool operator==(const Inset&, const Inset&) = default;
};

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
