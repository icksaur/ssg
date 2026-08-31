#pragma once

#include "ssg/UiTree.h"
#include "ssg/UiWidget.h"
#include "ssg/focus.h"
#include "ssg/Geometry.h"
#include "ssg/Style.h"
#include "ssg/StatusActionInvocation.h"
#include "ssg/Theme.h"
#include "ssg/Viewport.h"

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
class UiInteractionState;

class PaneId {
public:
    constexpr explicit PaneId(std::uint32_t value = 0) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    constexpr auto operator<=>(const PaneId&) const = default;

private:
    std::uint32_t value_;
};

enum class SplitAxis : std::uint8_t { Horizontal, Vertical };
enum class PaneDirection : std::uint8_t { Left, Right, Up, Down };

enum class ShellNodeKind : std::uint8_t {
    Header,
    HeaderField,
    Footer,
    FooterField,
    FooterAction,
    TabBar,
    Tab,
    Panel,
    PanelProvider,
    Pane,
    Scrollbar,
    PromptReservation,
    EmptyState,
    // A one-row draft-conflict notice reserved above the document (M15). The bar
    // spans the row (painted yellow); the actions are its clickable sub-regions.
    NoticeBar,
    NoticeAction,
    // A persistent, right-aligned footer hint (e.g. "Alt+KeyH  Help"). Distinct
    // from FooterAction so its click dispatches a plain command id directly,
    // without touching the status-queue action invocation path.
    FooterHint,
    // A non-interactive glyph painted between adjacent tabs. Carries no hit and
    // no command; it exists so a configured tab separator glyph is drawn.
    TabSeparator,
    // The external-modification bar reserved above the document (7A-5b): a
    // multi-row, bounded, windowed surface. The header row spans the width
    // (StatusWarning); each file row is a windowed entry with the selected one
    // highlighted; an action is a clickable bracketed sub-region on its row.
    ExternalModificationBar,
    ExternalModificationRow,
    ExternalModificationAction,
};

struct AccessibilityNode {
    ShellNodeKind kind = ShellNodeKind::Pane;
    std::string id;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::Canvas;
    std::string content;  // Display text for leaves; empty for containers/panes.
    std::optional<std::string> commandId;
    std::optional<StatusActionInvocation> statusInvocation;

    friend bool operator==(const AccessibilityNode&, const AccessibilityNode&) = default;
};

struct StatusField {
    std::string id;
    std::string accessibleLabel;
    std::string value;
    std::uint8_t collapseRank = 0;
    std::optional<std::string> commandId;
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

struct PaneFrame {
    PaneId id;
    Rect rect;
    friend bool operator==(const PaneFrame&, const PaneFrame&) = default;
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

class ShellState {
public:
    explicit ShellState();
    ~ShellState();
    ShellState(ShellState&&) noexcept;
    ShellState& operator=(ShellState&&) noexcept;
    ShellState(const ShellState&) = delete;
    ShellState& operator=(const ShellState&) = delete;

    [[nodiscard]] PaneId activePane() const noexcept;
    [[nodiscard]] std::size_t paneCount() const noexcept;
    [[nodiscard]] std::vector<PaneFrame> paneFrames(Rect rect) const;
    PaneId splitActive(SplitAxis axis);
    [[nodiscard]] bool closeActivePane();
    void nextPane() noexcept;
    void previousPane() noexcept;
    [[nodiscard]] bool focusPane(PaneDirection direction,
                                  const std::vector<PaneFrame>& panes) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
