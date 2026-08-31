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

// The header prompt input's grid-only text sidecar: the query and ghost that
// `computeShellLayout` lowers into the input_line nodes when the header prompt is
// present. Threaded alongside the interaction state rather than carried on the
// schema, because the query is a client-local prediction, not semantic state.
struct PromptInputReport {
    std::string query;
    std::string ghost;
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


// One clickable action in the draft-conflict notice (M15): a bracketed label
// hit-tested to dispatch `commandId`.
struct ShellNoticeAction {
    std::string id;
    std::string label;
    std::string commandId;
};

// The draft-conflict notice reserved above the document (M15): a message plus
// clickable actions. Absent (nullopt on the request) when there is no conflict.
struct ShellNotice {
    std::string text;
    std::vector<ShellNoticeAction> actions;
};

// One offered action on an external-modification row (7A-5b): a bracketed label
// and the payload-less action command it dispatches after the row is selected.
struct ShellExternalActionEntry {
    std::string label;
    std::string commandId;

    friend bool operator==(const ShellExternalActionEntry&,
                           const ShellExternalActionEntry&) = default;
};

// One externally-changed file the bar can show (7A-5b): its runtime-minted id,
// the display text (status glyph + path), and its offered actions.
struct ShellExternalRow {
    std::string fileId;
    std::string text;
    std::vector<ShellExternalActionEntry> actions;

    friend bool operator==(const ShellExternalRow&,
                           const ShellExternalRow&) = default;
};

// The external-modification bar to reserve above the document (7A-5b). The
// request carries ALL rows plus the ABSOLUTE selected index; computeShellLayout
// windows them to a bounded height that never consumes the document/footer, so
// the tiny-viewport degrade is layout's decision, not the caller's. Absent
// (nullopt) or with no rows reserves ZERO rows, keeping an empty-section golden
// byte-identical.
struct ShellExternalBar {
    std::string message;
    std::vector<ShellExternalRow> rows;
    std::uint32_t selected = 0;

    friend bool operator==(const ShellExternalBar&,
                           const ShellExternalBar&) = default;
};

struct TabLabel {
    std::string title;
    std::string accessibleLabel;
    bool active = false;
    bool dirty = false;
};


struct ShellLayoutRequest {
    GridSize viewport;
    bool distractionFree = false;
    std::uint8_t reservedPromptRows = 0;
    // Width in columns of the editor's left line-number gutter, or 0 when line
    // numbers are off. Carved from the LEFT of each
    // editor pane's content; 0 reproduces today's layout exactly.
    int lineNumberGutterWidth = 0;
    // A draft-conflict notice to reserve one chrome row for, above the document
    // (M15). Reserving a chrome row (rather than stealing document row 0) keeps
    // the document's own coordinate space -- line numbers, caret, scroll -- intact.
    std::optional<ShellNotice> notice;
    // The external-modification bar to reserve above the document (7A-5b, below
    // the notice in a fixed order). Absent or empty reserves ZERO rows.
    std::optional<ShellExternalBar> externalBar;
    bool emptyState = false;
    std::string panelProviderLabel = "Panel";
    // The panel presence and focus this layout is computed against. Stage-(i) of the
    // interaction-authority cutover threads these through the request so stage (ii) can
    // source them from the authority projection instead of ShellState without touching
    // computeShellLayout. Sourced from ShellState today.
    bool panelPresent = false;
    FocusTarget focus = FocusTarget::Editor;
    std::vector<TabLabel> tabs;
    // Dimensions and chrome glyphs this layout is computed against.  Defaults
    // reproduce the shipped appearance.
    Style style;
    ChromeProviderResolver chromeProviderResolver;
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
                                  const ShellViewState& view) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend ShellLayoutResult computeShellLayout(const ShellLayoutRequest&,
                                                const ShellState&,
                                                const UiInteractionState&,
                                                const StatusViewState&,
                                                const PromptInputReport&);
};

[[nodiscard]] ShellLayoutResult computeShellLayout(
    const ShellLayoutRequest& request, const ShellState& state,
    const UiInteractionState& interaction, const StatusViewState& statusView,
    const PromptInputReport& promptInput);

} // namespace ssg
