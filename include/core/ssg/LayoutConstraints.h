#pragma once

// The medium-agnostic layout constraint vocabulary.
//
// A layout tree is a tree of containers whose children carry these constraints:
// an axis to arrange children, a size (an exact extent or a flex share), and an
// inset. These types name NO medium: the extent is a plain integer the CONSUMER
// interprets in its own unit (cells for the terminal grid, a ch/px unit for a DOM
// client), and there is no Rect and no grid node kind here. The grid solver in
// Layout.h consumes these to produce cell rectangles; a native client maps the
// same constraints to its own layout. Keeping this header free of grid types is
// what lets a medium-agnostic tree be built without pulling in the grid.

#include <cstdint>
#include <stdexcept>

namespace ssg {

// How a container arranges its children. Row: children share the cross extent,
// placed along the main (leading-to-trailing) axis. Column: placed top-to-bottom.
enum class Axis : std::uint8_t { Row, Column };

// Whether a node is an independent scroll viewport. None: the node sizes to its
// content and does not clip. Vertical: the node is a viewport -- its content is
// laid out at its natural extent and CLIPPED to the node's own size, with the
// overflow reachable only by scrolling within the node. The scroll OFFSET is
// client-owned interaction state, never carried here; this only declares that a
// region is a viewport, so every client derives which regions scroll from the
// tree instead of inventing it per medium. Unit-neutral like the rest of this
// header: a grid client reserves a scrollbar gutter and a native client provides
// its equivalent overflow presentation.
enum class ScrollAxis : std::uint8_t { None, Vertical };

enum class SizeKind : std::uint8_t { Exact, Flex, Auto, Responsive };

// A node's size along its PARENT's axis. Exact reserves `extent` units of the
// consumer's medium; Flex takes an equal share of whatever remains after the
// Exact/Auto siblings are placed; Auto sizes to the node's own content (its
// intrinsic size), never growing to fill. `extent` is unit-neutral by design: the
// grid reads it as cells, a DOM client as its own unit. Int (like Rect) so a large
// authored dimension cannot wrap before it is solved. A Size cannot be constructed
// with a negative extent (the solver would emit a negative rectangle): the
// factories are the only construction path and reject it.
class Size {
public:
    Size() = default;  // Flex, 0 -- valid

    [[nodiscard]] static Size flex() noexcept { return Size{SizeKind::Flex, 0}; }
    [[nodiscard]] static Size autoSize() noexcept {
        return Size{SizeKind::Auto, 0};
    }
    [[nodiscard]] static Size exact(int extent) {
        if (extent < 0) {
            throw std::invalid_argument("Size::exact: negative extent");
        }
        return Size{SizeKind::Exact, extent};
    }
    [[nodiscard]] static Size minimumFlex(int minimum) {
        if (minimum < 0) {
            throw std::invalid_argument(
                "Size::minimumFlex: negative minimum");
        }
        return Size{minimum, minimum, 1, false};
    }
    [[nodiscard]] static Size optionalPreferred(int preferred, int minimum) {
        if (preferred <= 0 || minimum < 0 || minimum > preferred) {
            throw std::invalid_argument(
                "Size::optionalPreferred: invalid preferred range");
        }
        return Size{minimum, preferred, 0, true};
    }

    [[nodiscard]] SizeKind kind() const noexcept { return kind_; }
    [[nodiscard]] int extent() const noexcept { return extent_; }
    [[nodiscard]] int minimum() const noexcept { return minimum_; }
    [[nodiscard]] int growth() const noexcept { return growth_; }
    [[nodiscard]] bool grows() const noexcept { return growth_ > 0; }
    [[nodiscard]] bool optional() const noexcept { return optional_; }

    bool operator==(const Size&) const = default;

private:
    Size(SizeKind kind, int extent) noexcept : kind_(kind), extent_(extent) {}
    Size(int minimum, int preferred, int growth, bool optional) noexcept
        : kind_(SizeKind::Responsive),
          extent_(preferred),
          minimum_(minimum),
          growth_(growth),
          optional_(optional) {}

    SizeKind kind_ = SizeKind::Flex;
    int extent_ = 0;
    int minimum_ = 0;
    int growth_ = 0;
    bool optional_ = false;
};

// Units reserved inside a node's frame before its children are laid out, in the
// consumer's medium. Zero in the V1 shell; the border mechanism (a bordered box
// is inset + a child, the border ring being the frame minus the child's rect).
// A negative edge would enlarge the frame rather than reserve within it, so it is
// rejected at construction; the default is the valid all-zero inset.
class Inset {
public:
    Inset() = default;  // all zero -- valid

    [[nodiscard]] static Inset of(int left, int right, int top, int bottom) {
        if (left < 0 || right < 0 || top < 0 || bottom < 0) {
            throw std::invalid_argument("Inset::of: negative edge");
        }
        return Inset{left, right, top, bottom};
    }

    [[nodiscard]] int left() const noexcept { return left_; }
    [[nodiscard]] int right() const noexcept { return right_; }
    [[nodiscard]] int top() const noexcept { return top_; }
    [[nodiscard]] int bottom() const noexcept { return bottom_; }

    bool operator==(const Inset&) const = default;

private:
    Inset(int left, int right, int top, int bottom) noexcept
        : left_(left), right_(right), top_(top), bottom_(bottom) {}

    int left_ = 0, right_ = 0, top_ = 0, bottom_ = 0;
};

// Nonnegative spacing between a container's adjacent children on its axis. A
// value type so a negative gap is unrepresentable, not merely rejected downstream.
// Default is zero; Gap::of rejects a negative.
class Gap {
public:
    Gap() = default;  // zero -- valid

    [[nodiscard]] static Gap of(int cells) {
        if (cells < 0) throw std::invalid_argument("Gap::of: negative gap");
        return Gap{cells};
    }

    [[nodiscard]] int extent() const noexcept { return cells_; }

    bool operator==(const Gap&) const = default;

private:
    explicit Gap(int cells) noexcept : cells_(cells) {}

    int cells_ = 0;
};

}  // namespace ssg
