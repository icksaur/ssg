#pragma once

#include "ssg/focus.h"
#include "ssg/theme.h"
#include "ssg/viewport.h"

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

enum class SplitAxis : std::uint8_t { horizontal, vertical };
enum class PaneDirection : std::uint8_t { left, right, up, down };

enum class ShellNodeKind : std::uint8_t {
    header,
    header_field,
    footer,
    footer_field,
    footer_action,
    tab_bar,
    tab,
    panel,
    panel_provider,
    pane,
    scrollbar,
    prompt_reservation,
    empty_state,
};

struct AccessibilityNode {
    ShellNodeKind kind = ShellNodeKind::pane;
    std::string id;
    std::string label;
    Rect rect;
    SemanticRole role = SemanticRole::background;
    std::string content;  // Display text for leaves; empty for containers/panes.

    friend bool operator==(const AccessibilityNode&, const AccessibilityNode&) = default;
};

struct StatusField {
    std::string id;
    std::string accessible_label;
    std::string value;
    std::uint8_t collapse_rank = 0;
};

struct ShellLabel {
    std::string id;
    std::string accessible_label;
};

struct TabLabel {
    std::string title;
    std::string accessible_label;
    bool active = false;
    bool dirty = false;
};

struct ShellLayoutRequest {
    GridSize viewport;
    std::uint8_t reserved_prompt_rows = 0;
    bool empty_state = false;
    std::string panel_provider_label = "Panel";
    std::vector<StatusField> header_fields;
    std::vector<StatusField> footer_fields;
    std::vector<ShellLabel> footer_actions;
    std::vector<TabLabel> tabs;
    std::string leader_hint;  // Non-empty when a client is mid-chord.
    bool palette_active = false;  // The palette prompt is open on this client.
    std::string palette_query;    // The client's current palette query text.
    std::string palette_ghost;    // Fish-style completion of the top candidate.
};

struct PaneGeometry {
    PaneId id;
    Rect frame;
    Rect content;
    Rect scrollbar;

    friend bool operator==(const PaneGeometry&, const PaneGeometry&) = default;
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
    Rect scrollbar_rect;
    std::vector<PaletteRow> rows;
    // `selected` and `first_visible` are ABSOLUTE indices into the full ranked
    // order; `rows` is the windowed subset, so the on-screen row for the
    // selection is `selected - first_visible`. `scrollbar` drives the gutter
    // thumb (hidden when the list fits).
    std::optional<std::uint32_t> selected;
    std::uint32_t first_visible = 0;
    ScrollbarMetrics scrollbar{};

    friend bool operator==(const PaletteProjection&, const PaletteProjection&) = default;
};

struct ShellViewState {
    GridSize viewport;
    std::optional<Rect> header;
    std::optional<Rect> footer;
    std::optional<Rect> tab_bar;
    std::optional<Rect> panel;
    // The reserved 1-column scrollbar gutter for the side panel (the tree). Set
    // whenever `panel` is set; the panel content is the panel minus this column,
    // so the content width is stable whether or not a thumb is shown.
    std::optional<Rect> panel_scrollbar;
    std::optional<Rect> prompt;
    std::vector<PaneGeometry> panes;
    std::vector<AccessibilityNode> accessibility_nodes;
    FocusTarget focus = FocusTarget::editor;
    std::optional<PaletteProjection> palette;

    [[nodiscard]] std::size_t scrollbar_count() const noexcept {
        return panes.size();
    }
};

enum class ShellLayoutErrorCode : std::uint8_t {
    viewport_too_small,
    invalid_prompt_rows,
};

struct ShellLayoutError {
    ShellLayoutErrorCode code = ShellLayoutErrorCode::viewport_too_small;
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
    std::array<ShellCommandDescriptor, 14> descriptors{{
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
        {"panel.next_provider"},
        {"panel.previous_provider"},
        {"view.toggle_distraction_free"},
    }};
};

class ShellState {
public:
    explicit ShellState(std::vector<std::string> panel_providers = {});
    ~ShellState();
    ShellState(ShellState&&) noexcept;
    ShellState& operator=(ShellState&&) noexcept;
    ShellState(const ShellState&) = delete;
    ShellState& operator=(const ShellState&) = delete;

    [[nodiscard]] PaneId active_pane() const noexcept;
    [[nodiscard]] std::size_t pane_count() const noexcept;
    PaneId split_active(SplitAxis axis);
    [[nodiscard]] bool close_active_pane();
    void next_pane() noexcept;
    void previous_pane() noexcept;
    [[nodiscard]] bool focus_pane(PaneDirection direction,
                                  const ShellViewState& view) noexcept;

    void toggle_panel() noexcept;
    [[nodiscard]] bool focus_panel() noexcept;
    void focus_editor() noexcept;
    void enter_prompt_focus() noexcept;
    void exit_prompt_focus() noexcept;
    [[nodiscard]] FocusTarget focus() const noexcept;
    void next_panel_provider() noexcept;
    void previous_panel_provider() noexcept;
    [[nodiscard]] bool panel_requested() const noexcept;
    [[nodiscard]] bool panel_focused() const noexcept;
    [[nodiscard]] std::string_view active_panel_provider() const noexcept;

    void toggle_distraction_free() noexcept;
    [[nodiscard]] bool distraction_free() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend ShellLayoutResult compute_shell_layout(const ShellLayoutRequest&,
                                                  const ShellState&);
};

[[nodiscard]] ShellLayoutResult compute_shell_layout(
    const ShellLayoutRequest& request, const ShellState& state);

} // namespace ssg
