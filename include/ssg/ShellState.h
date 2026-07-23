#pragma once

#include "ssg/focus.h"
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

struct GridSize {
    int columns = 0;
    int rows = 0;
    friend bool operator==(const GridSize&, const GridSize&) = default;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr int right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }
    friend bool operator==(const Rect&, const Rect&) = default;
};

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
};

struct AccessibilityNode {
    ShellNodeKind kind = ShellNodeKind::Pane;
    std::string id;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::Background;
    std::string content;  // Display text for leaves; empty for containers/panes.
    std::optional<std::string> commandId;

    friend bool operator==(const AccessibilityNode&, const AccessibilityNode&) = default;
};

struct StatusField {
    std::string id;
    std::string accessibleLabel;
    std::string value;
    std::uint8_t collapseRank = 0;
    std::optional<std::string> commandId;
};

struct ShellLabel {
    std::string id;
    std::string accessibleLabel;
};

struct TabLabel {
    std::string title;
    std::string accessibleLabel;
    bool active = false;
    bool dirty = false;
};

struct ShellLayoutRequest {
    GridSize viewport;
    std::uint8_t reservedPromptRows = 0;
    bool emptyState = false;
    std::string panelProviderLabel = "Panel";
    std::vector<StatusField> headerFields;
    std::vector<StatusField> footerFields;
    std::vector<ShellLabel> footerActions;
    std::vector<TabLabel> tabs;
    std::string leaderHint;  // Non-empty when a client is mid-chord.
    bool paletteActive = false;  // The palette prompt is open on this client.
    std::string paletteQuery;    // The client's current palette query text.
    std::string paletteGhost;    // Fish-style completion of the top candidate.
};

struct PaneGeometry {
    PaneId id;
    Rect frame;
    Rect content;
    Rect scrollbar;

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
    FocusTarget focus = FocusTarget::Editor;
    std::optional<PaletteProjection> palette;

    [[nodiscard]] std::size_t scrollbarCount() const noexcept {
        return panes.size();
    }
};

enum class ShellLayoutErrorCode : std::uint8_t {
    ViewportTooSmall,
    InvalidPromptRows,
};

struct ShellLayoutError {
    ShellLayoutErrorCode code = ShellLayoutErrorCode::ViewportTooSmall;
    std::string message;
};

struct ShellLayoutResult {
    std::optional<ShellLayoutError> error;
    std::optional<ShellViewState> view;

    [[nodiscard]] bool accepted() const noexcept {
        return view.has_value() && !error.has_value();
    }
};

struct ShellCommandDescriptor {
    std::string_view id;
};

struct ShellCommandSet {
    std::array<ShellCommandDescriptor, 16> descriptors{{
        {"pane.split_horizontal"},
        {"pane.split_vertical"},
        {"pane.close"},
        {"pane.next"},
        {"pane.previous"},
        {"pane.focus_left"},
        {"pane.focus_right"},
        {"pane.focus_up"},
        {"pane.focus_down"},
        {"panel.toggle"},
        {"panel.focus"},
        {"panel.show_files"},
        {"panel.show_git_status"},
        {"panel.next_provider"},
        {"panel.previous_provider"},
        {"view.toggle_distraction_free"},
    }};
};

class ShellState {
public:
    explicit ShellState(std::vector<std::string> panelProviders = {});
    ~ShellState();
    ShellState(ShellState&&) noexcept;
    ShellState& operator=(ShellState&&) noexcept;
    ShellState(const ShellState&) = delete;
    ShellState& operator=(const ShellState&) = delete;

    [[nodiscard]] PaneId activePane() const noexcept;
    [[nodiscard]] std::size_t paneCount() const noexcept;
    PaneId splitActive(SplitAxis axis);
    [[nodiscard]] bool closeActivePane();
    void nextPane() noexcept;
    void previousPane() noexcept;
    [[nodiscard]] bool focusPane(PaneDirection direction,
                                  const ShellViewState& view) noexcept;

    void togglePanel() noexcept;
    [[nodiscard]] bool focusPanel() noexcept;
    [[nodiscard]] bool showPanelProvider(std::string_view provider) noexcept;
    void focusEditor() noexcept;
    void enterPromptFocus() noexcept;
    void exitPromptFocus() noexcept;
    [[nodiscard]] FocusTarget focus() const noexcept;
    void nextPanelProvider() noexcept;
    void previousPanelProvider() noexcept;
    [[nodiscard]] bool panelRequested() const noexcept;
    [[nodiscard]] bool panelFocused() const noexcept;
    [[nodiscard]] std::string_view activePanelProvider() const noexcept;

    void toggleDistractionFree() noexcept;
    [[nodiscard]] bool distractionFree() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend ShellLayoutResult computeShellLayout(const ShellLayoutRequest&,
                                                  const ShellState&);
};

[[nodiscard]] ShellLayoutResult computeShellLayout(
    const ShellLayoutRequest& request, const ShellState& state);

} // namespace ssg
