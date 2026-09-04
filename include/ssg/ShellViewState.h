#pragma once

#include <ssg/UiTree.h>
#include <ssg/UiWidget.h>
#include <ssg/focus.h>
#include <ssg/Geometry.h>
#include <ssg/PaneNavigation.h>
#include <ssg/Style.h>
#include <ssg/Theme.h>
#include <ssg/Scrollbar.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct StatusViewState;

enum class ShellNodeKind : std::uint8_t {
    Header = 0,
    HeaderField = 1,
    Footer = 2,
    FooterField = 3,
    FooterAction = 4,
    TabBar = 5,
    Tab = 6,
    Panel = 7,
    PanelProvider = 8,
    Pane = 9,
    Scrollbar = 10,
    PromptReservation = 11,
    EmptyState = 12,
    NoticeBar = 13,
    NoticeAction = 14,
    FooterHint = 15,
    TabSeparator = 16,
    ExternalModificationBar = 17,
    ExternalModificationRow = 18,
    ExternalModificationAction = 19,
};

struct AccessibilityNode {
    ShellNodeKind kind = ShellNodeKind::Pane;
    std::string id;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::Canvas;
    std::string content;  // Display text for leaves; empty for containers/panes.
    std::optional<std::string> commandId;
    friend bool operator==(const AccessibilityNode&, const AccessibilityNode&) = default;
};

struct PaneGeometry {
    PaneId id;
    Rect frame;
    Rect content;
    Rect scrollbar;
    // The left line-number gutter, carved from the frame's left edge. Empty
    // ({0,0,0,0}) when line numbers are off.
    Rect lineNumbers;

    friend bool operator==(const PaneGeometry&, const PaneGeometry&) = default;
};

// A clickable tab's rectangle plus its index into `sections().tabs.tabs`. Layout
// publishes one per visible tab so pointer hit-testing maps a cell to a tab
// without parsing the stringly-typed `tab.{i}` accessibility-node id.
struct TabHit {
    Rect rect;
    std::uint32_t index = 0;

    friend bool operator==(const TabHit&, const TabHit&) = default;
};

// A clickable external-modification action's rectangle plus the runtime-minted
// file id and the payload-less action command it dispatches (7A-5b). Layout
// publishes one per visible action so the pointer path can select-then-act
// without parsing the stringly-typed accessibility-node ids.
struct ExternalActionHit {
    Rect rect;
    std::string fileId;
    std::string commandId;

    friend bool operator==(const ExternalActionHit&,
                           const ExternalActionHit&) = default;
};

// One projected palette result row.
struct PaletteRow {
    std::string label;
    std::string detail;

    friend bool operator==(const PaletteRow&, const PaletteRow&) = default;
};

// The palette results projected into the active pane while the palette is open.
struct PaletteProjection {
    Rect rect;
    // The reserved 1-column scrollbar gutter (the pane's gutter column), always
    // present so the palette content width is stable as the list grows/shrinks.
    Rect scrollbarRect;
    std::vector<PaletteRow> rows;
    // `selected` and `first_visible` are ABSOLUTE indices into the full ranked
    // order; `rows` is the windowed subset, so the on-screen row for the
    // selection is `selected - first_visible`. `scrollbar` drives the gutter
    // thumb (hidden when the list fits).
    std::optional<std::uint32_t> selected;
    std::uint32_t firstVisible = 0;
    ScrollbarMetrics scrollbar{};

    friend bool operator==(const PaletteProjection&, const PaletteProjection&) = default;
};

struct ShellViewState {
    GridSize viewport;
    std::optional<Rect> header;
    std::optional<Rect> footer;
    std::optional<Rect> tabBar;
    std::optional<Rect> panel;
    // The reserved 1-column scrollbar gutter for the side panel (the tree). Set
    // whenever `panel` is set; the panel content is the panel minus this column,
    // so the content width is stable whether or not a thumb is shown.
    std::optional<Rect> panelScrollbar;
    std::optional<Rect> prompt;
    std::vector<PaneGeometry> panes;
    // One entry per visible tab (in tab-bar order), each carrying the tab's
    // rectangle and its index into `sections().tabs.tabs`.
    std::vector<TabHit> tabHits;
    std::vector<AccessibilityNode> accessibilityNodes;
    std::optional<PaletteProjection> palette;
    // One entry per visible external-modification action, each carrying its rect,
    // file id, and action command, so a pointer press maps a cell to a
    // select-then-act dispatch (7A-5b).
    std::vector<ExternalActionHit> externalActions;

    [[nodiscard]] std::size_t scrollbarCount() const noexcept {
        return panes.size();
    }
};

} // namespace ssg
