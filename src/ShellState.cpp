#include "ssg/ShellState.h"

#include "ssg/ChromeLowering.h"
#include "ssg/ChromeRegionShape.h"
#include "ssg/GraphemeLayout.h"
#include "ssg/InteractionState.h"
#include "ssg/Layout.h"
#include "ssg/Widget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

const UiNode* schemaArea(const UiSchema& schema, std::string_view areaId) {
    const auto* rootContainer =
        std::get_if<UiContainer>(&schema.root.content);
    if (!rootContainer) return nullptr;
    for (const auto& child : rootContainer->children)
        if (child.id.value() == areaId) return &child;
    return nullptr;
}

// Find a node anywhere in the tree by id (the scroll viewports -- panel and
// content -- are nested under the body, not direct children of the root).
const UiNode* findNodeById(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children)
            if (const auto* found = findNodeById(child, id)) return found;
    }
    return nullptr;
}

// Whether the node with `id` is declared a vertical scroll viewport. The TUI
// reserves a scrollbar gutter for a region ONLY when the tree says it scrolls, so
// the tree -- not hard-coded pane knowledge -- is the single authority for which
// regions scroll (a node the builder does not mark Vertical reserves no gutter).
bool nodeScrollsVertically(const UiSchema& schema, std::string_view id) {
    const UiNode* node = findNodeById(schema.root, id);
    if (!node) return false;
    const auto* container = std::get_if<UiContainer>(&node->content);
    return container && container->scroll == ScrollAxis::Vertical;
}

// Width of a label in terminal CELLS.  Layout budgets are cell counts, so
// measuring bytes would mis-size any label -- or any configured style glyph --
// outside ASCII.
int displayCells(std::string_view text) {
    return static_cast<int>(GraphemeLayout{}.computeRun(text).totalCells);
}

struct PaneNode {
    PaneId id;
    SplitAxis axis = SplitAxis::Vertical;
    std::unique_ptr<PaneNode> first;
    std::unique_ptr<PaneNode> second;

    [[nodiscard]] bool leaf() const noexcept { return !first; }
};

void collectIds(const PaneNode& node, std::vector<PaneId>& ids) {
    if (node.leaf()) {
        ids.push_back(node.id);
        return;
    }
    collectIds(*node.first, ids);
    collectIds(*node.second, ids);
}

PaneNode* findLeaf(PaneNode& node, PaneId id) {
    if (node.leaf()) return node.id == id ? &node : nullptr;
    if (auto* found = findLeaf(*node.first, id)) return found;
    return findLeaf(*node.second, id);
}

bool removeLeaf(std::unique_ptr<PaneNode>& node, PaneId id) {
    if (!node || node->leaf()) return false;
    if (node->first->leaf() && node->first->id == id) {
        node = std::move(node->second);
        return true;
    }
    if (node->second->leaf() && node->second->id == id) {
        node = std::move(node->first);
        return true;
    }
    return removeLeaf(node->first, id) || removeLeaf(node->second, id);
}

bool canLayout(const PaneNode& node, Rect rect) {
    if (node.leaf()) return rect.width >= 2 && rect.height >= 1;
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        return canLayout(*node.first, {rect.x, rect.y, firstWidth, rect.height}) &&
               canLayout(*node.second,
                          {rect.x + firstWidth, rect.y,
                           rect.width - firstWidth, rect.height});
    }
    const int firstHeight = rect.height / 2;
    return canLayout(*node.first, {rect.x, rect.y, rect.width, firstHeight}) &&
           canLayout(*node.second,
                      {rect.x, rect.y + firstHeight, rect.width,
                       rect.height - firstHeight});
}

void layoutPanes(const PaneNode& node, Rect rect,
                  std::vector<PaneGeometry>& output, int gutterWidth,
                  int lineNumberWidth, int editorMinimumWidth) {
    if (node.leaf()) {
        // Reserve the left line-number gutter, but never at the cost of a usable
        // editor: if the content left after both gutters would fall below the
        // editor minimum, drop the number gutter for this pane (it reappears when
        // the pane grows).  The renderer keys off the published rect, so the two
        // cannot disagree.
        int lineNumbers = lineNumberWidth;
        if (lineNumbers > 0 &&
            rect.width - gutterWidth - lineNumbers < editorMinimumWidth) {
            lineNumbers = 0;
        }
        output.push_back({
            node.id,
            rect,
            {rect.x + lineNumbers, rect.y,
             rect.width - gutterWidth - lineNumbers, rect.height},
            {rect.right() - gutterWidth, rect.y, gutterWidth, rect.height},
            lineNumbers > 0 ? Rect{rect.x, rect.y, lineNumbers, rect.height}
                            : Rect{0, 0, 0, 0},
        });
        return;
    }
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        layoutPanes(*node.first, {rect.x, rect.y, firstWidth, rect.height},
                     output, gutterWidth, lineNumberWidth, editorMinimumWidth);
        layoutPanes(*node.second,
                     {rect.x + firstWidth, rect.y,
                      rect.width - firstWidth, rect.height},
                     output, gutterWidth, lineNumberWidth, editorMinimumWidth);
        return;
    }
    const int firstHeight = rect.height / 2;
    layoutPanes(*node.first, {rect.x, rect.y, rect.width, firstHeight},
                 output, gutterWidth, lineNumberWidth, editorMinimumWidth);
    layoutPanes(*node.second,
                 {rect.x, rect.y + firstHeight, rect.width,
                  rect.height - firstHeight},
                 output, gutterWidth, lineNumberWidth, editorMinimumWidth);
}

void addNode(ShellViewState& view, ShellNodeKind kind, std::string id,
              std::string label, Rect rect, SemanticRole role,
              std::string content = {},
              std::optional<std::string> commandId = std::nullopt) {
    view.accessibilityNodes.push_back(
        {kind, std::move(id), std::move(label), rect, role, std::move(content),
         std::move(commandId)});
}

const PaneGeometry* paneGeometry(const ShellViewState& view, PaneId id) {
    const auto found = std::ranges::find(view.panes, id, &PaneGeometry::id);
    return found == view.panes.end() ? nullptr : &*found;
}

double centerX(const Rect& rect) { return rect.x + rect.width / 2.0; }
double centerY(const Rect& rect) { return rect.y + rect.height / 2.0; }

// The shell's region geometry as a box tree. The
// builder is where sizing POLICY lives: the caller passes the already-decided
// panel width (0 when the panel is absent), and distraction-free collapses the
// tree to just the document. The solver then computes every region rect.
// Accessibility projection remains separate during this compatibility step.
LayoutNode buildShellTree(bool distractionFree, int headerHeight,
                          int footerHeight, int tabBarHeight, int panelWidth,
                          bool showTabBar) {
    const auto exact = [](int cells) { return Size::exact(cells); };
    LayoutNode document{UiNodeId{"document"}, Size::flex(), Axis::Column,
                        {}, {}};
    if (distractionFree) return document;

    LayoutNode content{UiNodeId{"content"}, Size::flex(), Axis::Column,
                       {}, {}};
    // A picker (palette / file find) covers the document, which is not a
    // document view -- so the tab bar is suppressed and the picker's content
    // fills its row.  This also removes the one-row gap the tab bar left between
    // the input line and the results.
    if (showTabBar) {
        content.children.push_back(
            {UiNodeId{"tabbar"}, exact(tabBarHeight), Axis::Row, {}, {}});
    }
    content.children.push_back(std::move(document));

    LayoutNode body{UiNodeId{"body"}, Size::flex(), Axis::Row, {}, {}};
    if (panelWidth > 0) {
        body.children.push_back(
            {UiNodeId{"panel"}, exact(panelWidth), Axis::Column, {}, {}});
    }
    body.children.push_back(std::move(content));

    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Column, {}, {}};
    root.children.push_back(
        {UiNodeId{"header"}, exact(headerHeight), Axis::Row, {}, {}});
    root.children.push_back(std::move(body));
    root.children.push_back(
        {UiNodeId{"footer"}, exact(footerHeight), Axis::Row, {}, {}});
    return root;
}

} // namespace

struct ShellState::Impl {
    std::unique_ptr<PaneNode> root =
        std::make_unique<PaneNode>(PaneNode{PaneId{1}});
    PaneId active{1};
    std::uint32_t nextId = 2;
};

ShellState::ShellState() : impl_(std::make_unique<Impl>()) {}

ShellState::~ShellState() = default;
ShellState::ShellState(ShellState&&) noexcept = default;
ShellState& ShellState::operator=(ShellState&&) noexcept = default;

PaneId ShellState::activePane() const noexcept { return impl_->active; }

std::size_t ShellState::paneCount() const noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    return ids.size();
}

PaneId ShellState::splitActive(SplitAxis axis) {
    auto* leaf = findLeaf(*impl_->root, impl_->active);
    const PaneId original = leaf->id;
    const PaneId created{impl_->nextId++};
    leaf->axis = axis;
    leaf->first = std::make_unique<PaneNode>(PaneNode{original});
    leaf->second = std::make_unique<PaneNode>(PaneNode{created});
    impl_->active = created;
    return created;
}

bool ShellState::closeActivePane() {
    if (paneCount() == 1) return false;
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    const PaneId replacement =
        active + 1 != ids.end() ? *(active + 1) : *(active - 1);
    const bool removed = removeLeaf(impl_->root, impl_->active);
    if (removed) impl_->active = replacement;
    return removed;
}

void ShellState::nextPane() noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = *(active + 1 == ids.end() ? ids.begin() : active + 1);
}

void ShellState::previousPane() noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = active == ids.begin() ? ids.back() : *(active - 1);
}

bool ShellState::focusPane(PaneDirection direction,
                            const ShellViewState& view) noexcept {
    const auto* current = paneGeometry(view, impl_->active);
    if (!current) return false;
    const double currentX = centerX(current->frame);
    const double currentY = centerY(current->frame);
    const PaneGeometry* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& candidate : view.panes) {
        if (candidate.id == impl_->active) continue;
        const double dx = centerX(candidate.frame) - currentX;
        const double dy = centerY(candidate.frame) - currentY;
        const bool eligible =
            (direction == PaneDirection::Left && dx < 0) ||
            (direction == PaneDirection::Right && dx > 0) ||
            (direction == PaneDirection::Up && dy < 0) ||
            (direction == PaneDirection::Down && dy > 0);
        if (!eligible) continue;
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            best = &candidate;
            bestDistance = distance;
        }
    }
    if (!best) return false;
    impl_->active = best->id;
    return true;
}

ShellLayoutResult computeShellLayout(const ShellLayoutRequest& request,
                                       const ShellState& state,
                                       const UiInteractionState& interaction,
                                       const StatusViewState& statusView,
                                       const PromptInputReport& promptInput) {
    const ValidatedSchema& schema = interaction.schema();
    if (request.viewport.columns < request.style.dimensions.minimumColumns ||
        request.viewport.rows < request.style.dimensions.minimumRows) {
        return {ShellLayoutError{ShellLayoutErrorCode::ViewportTooSmall,
                                 "viewport must be at least 20 columns by 4 rows"},
                std::nullopt};
    }
    if (request.reservedPromptRows > 3) {
        return {ShellLayoutError{ShellLayoutErrorCode::InvalidPromptRows,
                                 "reserved prompt rows must be in [0, 3]"},
                std::nullopt};
    }

    ShellViewState view;
    view.viewport = request.viewport;
    const bool distractionFree = request.distractionFree;
    const int gutterWidth = request.style.dimensions.scrollbarGutterWidth;
    // The tree is the single authority for which regions scroll.
    const bool panelScrolls =
        nodeScrollsVertically(schema.schema(), kPanelNodeId);
    const bool documentScrolls =
        nodeScrollsVertically(schema.schema(), kDocumentViewportNodeId);
    const int contentGutter = documentScrolls ? gutterWidth : 0;
    const int headerHeight = request.style.dimensions.headerHeight;
    const int footerHeight = request.style.dimensions.footerHeight;
    const int tabBarHeight = request.style.dimensions.tabBarHeight;
    // Tab-bar presence comes from the same published tree the web client renders.
    const bool inputVisible = interaction.presence().isPresent(
        UiNodeId{std::string{kHeaderPromptInputNodeId}});
    const bool showTabBar =
        interaction.presence().isPresent(UiNodeId{std::string{kEditorNodeId}}) &&
        interaction.presence().isPresent(UiNodeId{std::string{kTabBarNodeId}});

    // Region geometry comes from the box-tree solver.
    // Sizing POLICY stays here: the panel width is decided with the same rule as
    // before (yield to keep the editor's minimum; absent below the threshold) and
    // handed to the builder, which turns it and the chrome heights into a tree the
    // solver places. The content packers and the accessibility-node emission below
    // are unchanged -- they receive the solved rects instead of hand-built ones.
    int panelWidth = 0;
    if (!distractionFree && request.panelPresent &&
        request.viewport.columns >=
            request.style.dimensions.editorMinimumWidth +
                request.style.dimensions.panelMinimumWidth) {
        panelWidth = std::min(request.style.dimensions.panelTargetWidth,
                              request.viewport.columns -
                                  request.style.dimensions.editorMinimumWidth);
    }
    auto solved = solveGridTree(
        buildShellTree(distractionFree, headerHeight, footerHeight, tabBarHeight,
                       panelWidth, showTabBar),
        {0, 0, request.viewport.columns, request.viewport.rows});
    if (!solved) {
        return {ShellLayoutError{ShellLayoutErrorCode::ViewportTooSmall,
                                 "viewport too small for the shell layout"},
                std::nullopt};
    }
    Rect editor = solved->find(UiNodeId{"document"})->rect;

    if (!distractionFree) {
        view.header = solved->find(UiNodeId{"header"})->rect;
        view.footer = solved->find(UiNodeId{"footer"})->rect;
        addNode(view, ShellNodeKind::Header, "header", "Status header",
                 *view.header, SemanticRole::Header);
        addNode(view, ShellNodeKind::Footer, "footer", "Status footer",
                 *view.footer, SemanticRole::Footer);
        // A composed region's `provider` widgets resolve through this. When the
        // request carries no resolver, provider widgets find nothing and drop
        // (as an empty-value built-in field drops); a null std::function must
        // not be invoked, so wrap it defensively.
        const ChromeProviderResolver chromeResolver =
            request.chromeProviderResolver
                ? request.chromeProviderResolver
                : [](std::string_view) { return std::optional<ResolvedProvider>{}; };
        // The header lowers its status groups AND the prompt input in one pass: the
        // input's fixed reservation is a floor subtracted from the groups' width
        // before they collapse (so typing never reflows the working directory and
        // branch), then the input grows across the header's remaining width after
        // them. A composed header REPLACES the built-in status fields but keeps this
        // same input placement, so the query line follows whichever left group is
        // present.
        const UiNode* headerRegion = schemaArea(schema.schema(), kHeaderNodeId);
        if (headerRegion) {
            const PromptInputProjection promptProjection{inputVisible,
                                                         promptInput.query,
                                                         promptInput.ghost};
            const auto lowered = lowerUiChromeRegion(
                *headerRegion,
                {view.header->x, view.header->y, view.header->width, 1},
                ShellNodeKind::HeaderField, SemanticRole::Header, request.style,
                chromeResolver, view.accessibilityNodes, nullptr, &promptProjection);
            if (!lowered.ok()) throw std::invalid_argument(*lowered.error);
        }
        const UiNode* footerRegion = schemaArea(schema.schema(), kFooterNodeId);
        if (footerRegion) {
            // A composed footer REPLACES the whole built-in footer row (fields,
            // hint, actions). Unlike the header it supports full left/right/
            // center, so it lowers over the entire footer rect; every widget
            // becomes a FooterField node.
            const auto lowered = lowerUiChromeRegion(
                *footerRegion,
                {view.footer->x, view.footer->y, view.footer->width, 1},
                ShellNodeKind::FooterField, SemanticRole::Footer, request.style,
                chromeResolver, view.accessibilityNodes, &statusView);
            if (!lowered.ok()) throw std::invalid_argument(*lowered.error);
        }

        if (panelWidth > 0) {
            view.panel = solved->find(UiNodeId{"panel"})->rect;
            addNode(view, ShellNodeKind::Panel, "panel", "Side panel",
                     *view.panel, SemanticRole::PanelInactive);
            addNode(view, ShellNodeKind::PanelProvider, "panel.provider",
                     request.panelProviderLabel, *view.panel,
                     request.focus == FocusTarget::Panel ? SemanticRole::PanelActive :
                                                  SemanticRole::PanelInactive,
                     request.panelProviderLabel);
            // Reserve the tree's scrollbar gutter: the right column over the tree
            // content rows (below the provider-label row). Content is the panel
            // minus this column, so tree text width never changes with the thumb.
            if (panelScrolls && panelWidth > gutterWidth && view.panel->height > 1) {
                view.panelScrollbar =
                    Rect{panelWidth - gutterWidth, view.panel->y + 1,
                         gutterWidth, view.panel->height - 1};
                addNode(view, ShellNodeKind::Scrollbar, "panel.scrollbar",
                         "Panel scrollbar", *view.panelScrollbar,
                         SemanticRole::ScrollbarTrack);
            }
        }

        editor = solved->find(UiNodeId{"document"})->rect;
        if (showTabBar) {
            view.tabBar = solved->find(UiNodeId{"tabbar"})->rect;
            addNode(view, ShellNodeKind::TabBar, "tabs", "Open tabs",
                     *view.tabBar, SemanticRole::TabInactive);
            // A tab draws as leftEdge + title[+dirtySuffix] + rightEdge; its width
            // is measured from that, with no fixed padding.  Consecutive tabs are
            // parted by the separator glyph, emitted as its own non-interactive
            // node.
            auto const chipDisplay = [&](TabLabel const& tab) {
                return gridTabDisplay(tab.title, tab.dirty,
                                      request.style.tab);
            };
            auto const tabWidth = [&](TabLabel const& tab) {
                return std::max(1, displayCells(chipDisplay(tab)));
            };
            int const separatorWidth = displayCells(request.style.tab.separator);
            // Which tab the bar starts at.  Tabs used to lay out from the first and
            // simply stop at the edge, so opening enough of them put the active tab
            // off-screen -- invisible AND unclickable, with no way back to it but the
            // keyboard.
            //
            // Derived rather than stored: the smallest start index that still leaves
            // room for the active tab.  That keeps the active tab visible, shows as
            // many preceding tabs as fit, and needs no scroll offset to keep in sync
            // with tabs opening, closing and being reordered.  next/previous
            // therefore auto-scroll for free -- they move the active tab, and the
            // window follows it.
            std::size_t firstTab = 0;
            if (!request.tabs.empty()) {
                std::size_t active = 0;
                for (std::size_t i = 0; i < request.tabs.size(); ++i) {
                    if (request.tabs[i].active) active = i;
                }
                int used = 0;
                for (std::size_t i = 0; i <= active; ++i) used += tabWidth(request.tabs[i]);
                used += static_cast<int>(active) * separatorWidth;
                while (firstTab < active && used > view.tabBar->width) {
                    used -= tabWidth(request.tabs[firstTab]) + separatorWidth;
                    ++firstTab;
                }
            }
            int tabX = view.tabBar->x;
            bool placedAny = false;
            for (std::size_t i = firstTab; i < request.tabs.size(); ++i) {
                const auto& tab = request.tabs[i];
                if (tab.accessibleLabel.empty()) break;
                // A separator sits BETWEEN placed tabs, so it is reserved before the
                // tab it precedes and emitted only once that tab is confirmed to
                // fit.  Keying it off the tab that actually lands -- rather than the
                // logical next index -- is what keeps a separator from dangling past
                // the last visible tab when the row runs out of room.
                const int separatorReserve =
                    (placedAny && separatorWidth > 0)
                        ? std::min(view.tabBar->right() - tabX, separatorWidth)
                        : 0;
                const int chipX = tabX + separatorReserve;
                const std::string display = chipDisplay(tab);
                const int width = std::min(view.tabBar->right() - chipX,
                                           std::max(1, displayCells(display)));
                if (width <= 0) break;
                if (separatorReserve > 0) {
                    addNode(view, ShellNodeKind::TabSeparator,
                             "tabsep." + std::to_string(i), "",
                             {tabX, view.tabBar->y, separatorReserve, tabBarHeight},
                             SemanticRole::TabInactive, request.style.tab.separator);
                }
                addNode(view, ShellNodeKind::Tab, "tab." + std::to_string(i),
                         tab.accessibleLabel,
                         {chipX, view.tabBar->y, width, tabBarHeight},
                         tab.active ? SemanticRole::TabActive :
                                      SemanticRole::TabInactive,
                         display);
                view.tabHits.push_back(
                    TabHit{{chipX, view.tabBar->y, width, tabBarHeight},
                           static_cast<std::uint32_t>(i)});
                tabX = chipX + width;
                placedAny = true;
            }
        }

        // `editor` is already the solved document rect (below the tab bar). The
        // prompt reservation shrinks it from the bottom, over the footer, without
        // moving its top -- so opening a prompt reduces document height but never
        // pushes content down.
        if (request.reservedPromptRows >= editor.height) {
            return {ShellLayoutError{
                        ShellLayoutErrorCode::ViewportTooSmall,
                        "prompt reservation leaves no editor content row"},
                    std::nullopt};
        }
        if (request.reservedPromptRows > 0) {
            // The prompt occupies the bottom rows of the screen, FULL WIDTH --
            // this rect is the single source for the footer-anchored prompt
            //: the shell
            // reservation (here) and the prompt-status reservation
            // (runtime/snapshot.cpp) are the SAME rect, so the a11y node, the
            // hit region, and the rendered controls cannot diverge. It spans the
            // whole viewport width (like the footer region it sits over), not the
            // editor width, matching where the controls actually render. Shrink
            // the editor from the BOTTOM by height only -- never move its top, and
            // never change its x/width -- so opening a prompt reduces document
            // height without pushing content down or reflowing it.
            const int promptTop =
                request.viewport.rows - request.reservedPromptRows;
            view.prompt = Rect{0, promptTop, request.viewport.columns,
                               request.reservedPromptRows};
            addNode(view, ShellNodeKind::PromptReservation, "prompt",
                     "Prompt surface", *view.prompt, SemanticRole::Prompt);
            editor.height = std::max(0, promptTop - editor.y);
        }
    }

    // The draft-conflict notice reserves ONE row at the TOP of the document
    // region (below the tab bar). Shrinking `editor` from the top -- rather than
    // stealing document row 0 -- keeps the document's own coordinate space (line
    // numbers, caret, scroll, viewport-relative hit-testing) unperturbed; the
    // document simply shows one fewer row while the notice is up, exactly as the
    // prompt reservation does from the bottom.
    if (request.notice && editor.height > 1) {
        const Rect noticeRow{editor.x, editor.y, editor.width, 1};
        addNode(view, ShellNodeKind::NoticeBar, "draft.notice",
                "Draft conflict notice", noticeRow, SemanticRole::StatusWarning,
                request.notice->text);
        // Actions are packed from the right so the message owns the left of the
        // row; each becomes a clickable sub-rect carrying its command id.
        int actionX = noticeRow.right();
        for (auto it = request.notice->actions.rbegin();
             it != request.notice->actions.rend(); ++it) {
            const std::string label = "[" + it->label + "]";
            const int width = displayCells(label);
            actionX -= width;
            if (actionX < noticeRow.x) break;
            addNode(view, ShellNodeKind::NoticeAction, it->id, it->label,
                    {actionX, noticeRow.y, width, 1}, SemanticRole::StatusWarning,
                    label, it->commandId);
            actionX -= 1;  // one-cell gap between actions
        }
        editor.y += 1;
        editor.height -= 1;
    }

    // The external-modification bar reserves a BOUNDED, WINDOWED block below the
    // notice (fixed order), above the document. A header row plus a window of file
    // rows scrolled to keep the selection visible, with a "+N more" overflow row.
    // The reserved height is capped so it never consumes the document or footer:
    // at least one document row always survives. It degrades on a tiny viewport --
    // header + the selected row, then (below a floor) the header alone with a
    // count -- and reserves ZERO rows when no file is present, so an empty-section
    // golden stays byte-identical.
    if (request.externalBar && !request.externalBar->rows.empty() &&
        editor.height > 1) {
        constexpr int kMaxVisibleFileRows = 4;
        const auto& bar = *request.externalBar;
        const int total = static_cast<int>(bar.rows.size());
        const int selected =
            std::clamp(static_cast<int>(bar.selected), 0, total - 1);
        // Rows we may take while leaving at least one document row.
        const int budget = editor.height - 1;
        const int headerRows = 1;
        const int fileBudget = std::max(0, budget - headerRows);

        int firstVisible = 0;
        int shown = 0;
        bool overflow = false;
        if (fileBudget >= 1) {
            const int listRows =
                std::min({total, kMaxVisibleFileRows, fileBudget});
            // The overflow indicator claims one of the list rows, but never the
            // last remaining row (tier-2 degrade shows just the selected file).
            shown = (total > listRows && listRows > 1) ? listRows - 1 : listRows;
            overflow = total > shown && listRows > 1;
            // Window so the selected row is visible: clamp its top so
            // [firstVisible, firstVisible+shown) contains `selected`.
            firstVisible = std::clamp(selected - shown / 2, 0,
                                      std::max(0, total - shown));
        }
        const int overflowRows = overflow ? 1 : 0;
        const int reserved = headerRows + shown + overflowRows;

        int y = editor.y;
        const Rect headerRect{editor.x, y, editor.width, 1};
        addNode(view, ShellNodeKind::ExternalModificationBar, "external.bar",
                "External modification bar", headerRect,
                SemanticRole::StatusWarning, bar.message);
        y += 1;
        for (int i = 0; i < shown; ++i) {
            const int index = firstVisible + i;
            const auto& row = bar.rows[static_cast<std::size_t>(index)];
            const bool isSelected = index == selected;
            const Rect rowRect{editor.x, y, editor.width, 1};
            const SemanticRole rowRole = isSelected ? SemanticRole::Selection
                                                    : SemanticRole::StatusWarning;
            addNode(view, ShellNodeKind::ExternalModificationRow, row.fileId,
                    "External modification file", rowRect, rowRole, row.text);
            // Actions pack from the right so the file text owns the left; each is a
            // clickable sub-rect carrying its file id and action command.
            int actionX = rowRect.right();
            for (auto it = row.actions.rbegin(); it != row.actions.rend(); ++it) {
                const std::string label = "[" + it->label + "]";
                const int width = displayCells(label);
                actionX -= width;
                if (actionX < rowRect.x) break;
                const Rect actionRect{actionX, y, width, 1};
                addNode(view, ShellNodeKind::ExternalModificationAction,
                        row.fileId + "|" + it->commandId, it->label, actionRect,
                        rowRole, label);
                view.externalActions.push_back(
                    {actionRect, row.fileId, it->commandId});
                actionX -= 1;
            }
            y += 1;
        }
        if (overflow) {
            const int more = total - shown;
            const Rect moreRect{editor.x, y, editor.width, 1};
            addNode(view, ShellNodeKind::ExternalModificationRow,
                    "external.overflow", "More external modifications", moreRect,
                    SemanticRole::StatusWarning,
                    "+" + std::to_string(more) + " more");
            y += 1;
        }
        editor.y += reserved;
        editor.height -= reserved;
    }

    const int lineNumberWidth = std::max(0, request.lineNumberGutterWidth);
    const int editorMinimumWidth = request.style.dimensions.editorMinimumWidth;
    if (canLayout(*state.impl_->root, editor)) {
        layoutPanes(*state.impl_->root, editor, view.panes, contentGutter,
                    lineNumberWidth, editorMinimumWidth);
    } else {
        int panelessNumbers = lineNumberWidth;
        if (panelessNumbers > 0 &&
            editor.width - contentGutter - panelessNumbers < editorMinimumWidth) {
            panelessNumbers = 0;
        }
        view.panes.push_back({
            state.impl_->active,
            editor,
            {editor.x + panelessNumbers, editor.y,
             editor.width - contentGutter - panelessNumbers, editor.height},
            {editor.right() - contentGutter, editor.y, contentGutter, editor.height},
            panelessNumbers > 0
                ? Rect{editor.x, editor.y, panelessNumbers, editor.height}
                : Rect{0, 0, 0, 0},
        });
    }

    for (const auto& pane : view.panes) {
        const auto suffix = std::to_string(pane.id.value());
        addNode(view, ShellNodeKind::Pane, "pane." + suffix,
                 "Editor pane " + suffix, pane.frame, SemanticRole::Canvas);
        if (documentScrolls) {
            addNode(view, ShellNodeKind::Scrollbar, "pane." + suffix + ".scrollbar",
                     "Scrollbar for editor pane " + suffix, pane.scrollbar,
                     SemanticRole::ScrollbarTrack);
        }
        if (request.emptyState) {
            addNode(view, ShellNodeKind::EmptyState,
                     "pane." + suffix + ".empty", "empty editor",
                     pane.content, SemanticRole::Canvas, "empty editor");
        }
    }

    return {std::nullopt, std::move(view)};
}

} // namespace ssg
