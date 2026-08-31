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
#include <ssg/ExternalModificationFlow.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Style.h>
#include <ssg/TabManager.h>
#include <ssg/UiNodeState.h>
#include <ssg/UiPresence.h>
#include <ssg/UiProfile.h>
#include <ssg/UiTree.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct NoticeView;

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
    ResolvedUiNodeStyle style;
    std::optional<WidgetDescriptor> widget;
    std::optional<UiLeafState> leafState;

    friend bool operator==(const SolvedGridNode&,
                           const SolvedGridNode&) = default;
};

struct SolvedGridTree {
    std::vector<SolvedGridNode> nodes;

    [[nodiscard]] const SolvedGridNode* find(
        const UiNodeId& id) const noexcept;
};

struct SolvedNoticeAction {
    std::string id;
    std::string text;
    Rect rect;

    friend bool operator==(const SolvedNoticeAction&,
                           const SolvedNoticeAction&) = default;
};

struct SolvedNoticeSurface {
    Rect rect;
    std::vector<SolvedNoticeAction> actions;

    friend bool operator==(const SolvedNoticeSurface&,
                           const SolvedNoticeSurface&) = default;
};

// CONTRACT: This is the single authoritative action placement within a solved
// notice band; rendering and hit testing consume the same result.
[[nodiscard]] SolvedNoticeSurface solveNoticeSurface(
    const NoticeView& notice, Rect rect);

struct SolvedExternalModificationAction {
    DiffFileId fileId;
    std::string command;
    std::string text;
    Rect rect;

    friend bool operator==(const SolvedExternalModificationAction&,
                           const SolvedExternalModificationAction&) = default;
};

struct SolvedExternalModificationRow {
    std::optional<DiffFileId> fileId;
    std::string text;
    Rect rect;
    bool selected = false;
    std::vector<SolvedExternalModificationAction> actions;

    friend bool operator==(const SolvedExternalModificationRow&,
                           const SolvedExternalModificationRow&) = default;
};

struct SolvedExternalModificationSurface {
    Rect rect;
    Rect header;
    std::vector<SolvedExternalModificationRow> rows;

    friend bool operator==(const SolvedExternalModificationSurface&,
                           const SolvedExternalModificationSurface&) = default;
};

[[nodiscard]] GridSize measureExternalModificationSurface(
    const ExternalModificationViewState& external);
// CONTRACT: Rendering and hit testing consume this single authoritative row and
// action placement within the solved external-modification band. Actions are
// stored right-to-left in visual packing order, not semantic declaration order.
[[nodiscard]] SolvedExternalModificationSurface
solveExternalModificationSurface(
    const ExternalModificationViewState& external, Rect rect);

struct SolvedTab {
    std::size_t index = 0;
    TabId id;
    std::string text;
    Rect rect;
    bool active = false;

    friend bool operator==(const SolvedTab&, const SolvedTab&) = default;
};

struct SolvedTabSeparator {
    std::string text;
    Rect rect;

    friend bool operator==(const SolvedTabSeparator&,
                           const SolvedTabSeparator&) = default;
};

struct SolvedTabBar {
    Rect rect;
    std::vector<SolvedTab> tabs;
    std::vector<SolvedTabSeparator> separators;

    friend bool operator==(const SolvedTabBar&, const SolvedTabBar&) = default;
};

[[nodiscard]] std::string gridTabTitle(const TabState& tab,
                                       const TabGlyphs& glyphs);
[[nodiscard]] std::string gridTabDisplay(std::string_view title, bool dirty,
                                         const TabGlyphs& glyphs);
// CONTRACT: This is the single authoritative visible tab window and chip
// placement for grid rendering and hit testing.
[[nodiscard]] SolvedTabBar solveTabBar(const TabViewState& tabs,
                                       const TabGlyphs& glyphs, Rect rect);

struct SolvedPaletteRow {
    std::size_t windowIndex = 0;
    std::uint32_t absoluteIndex = 0;
    Rect rect;
    bool selected = false;

    friend bool operator==(const SolvedPaletteRow&,
                           const SolvedPaletteRow&) = default;
};

struct SolvedPaletteSurface {
    Rect rect;
    Rect rows;
    Rect scrollbar;
    std::vector<SolvedPaletteRow> visibleRows;

    friend bool operator==(const SolvedPaletteSurface&,
                           const SolvedPaletteSurface&) = default;
};

// CONTRACT: Picker rendering, row hits, inert padding, and scrollbar hits derive
// from this one solved viewport projection.
[[nodiscard]] SolvedPaletteSurface solvePaletteSurface(
    const PaletteReport& palette, Rect rect, int scrollbarWidth);

// Returns nullopt when nonnegative bounds cannot contain an inset, gaps, or
// exact children. Invalid identities and unsupported Auto sizes are misuse and
// throw std::invalid_argument.
[[nodiscard]] std::optional<SolvedGridTree> solveGridTree(
    const LayoutNode& root, Rect bounds);

struct GridIntrinsicSize {
    UiNodeId id;
    GridSize size;

    friend bool operator==(const GridIntrinsicSize&,
                           const GridIntrinsicSize&) = default;
};

struct SolveUiFrameResult {
    std::optional<SolvedGridTree> tree;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept { return tree.has_value(); }
};

// Solves one corresponding schema/state/presence frame. Auto leaves require one
// caller-measured intrinsic size; Auto containers derive theirs from children.
[[nodiscard]] SolveUiFrameResult solveUiFrame(
    const ValidatedSchema& schema, const UiStateSection& state,
    const UiPresenceSection& presence, const ClientUiProfile& profile,
    const std::vector<GridIntrinsicSize>& intrinsicSizes, Rect bounds);

}  // namespace ssg
