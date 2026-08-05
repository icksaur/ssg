#include "ssg/ShellState.h"

#include "ssg/GraphemeLayout.h"
#include "ssg/Layout.h"
#include "ssg/Widget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace ssg {
namespace {

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

// The shell's region geometry as a box tree (doc/spec-layout-engine.md). The
// builder is where sizing POLICY lives: the caller passes the already-decided
// panel width (0 when the panel is absent), and distraction-free collapses the
// tree to just the document. The solver then computes every region rect. Chrome
// leaves carry their kind; structural containers (root/body/content) carry none
// and are not projected. The document node is never emitted as an a11y node --
// panes are -- so it is structural here too.
LayoutNode buildShellTree(bool distractionFree, int headerHeight,
                          int footerHeight, int tabBarHeight, int panelWidth,
                          bool showTabBar) {
    const auto exact = [](int cells) { return Size::exact(cells); };
    LayoutNode document{"document", std::nullopt, Size::flex(), Axis::Column,
                        {}, {}};
    if (distractionFree) return document;

    LayoutNode content{"content", std::nullopt, Size::flex(), Axis::Column,
                       {}, {}};
    // A picker (palette / file find) covers the document, which is not a
    // document view -- so the tab bar is suppressed and the picker's content
    // fills its row.  This also removes the one-row gap the tab bar left between
    // the input line and the results.
    if (showTabBar) {
        content.children.push_back({"tabbar", ShellNodeKind::TabBar,
                                    exact(tabBarHeight), Axis::Row, {}, {}});
    }
    content.children.push_back(std::move(document));

    LayoutNode body{"body", std::nullopt, Size::flex(), Axis::Row, {}, {}};
    if (panelWidth > 0) {
        body.children.push_back({"panel", ShellNodeKind::Panel,
                                 exact(panelWidth), Axis::Column, {}, {}});
    }
    body.children.push_back(std::move(content));

    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Column, {}, {}};
    root.children.push_back({"header", ShellNodeKind::Header, exact(headerHeight),
                             Axis::Row, {}, {}});
    root.children.push_back(std::move(body));
    root.children.push_back({"footer", ShellNodeKind::Footer, exact(footerHeight),
                             Axis::Row, {}, {}});
    return root;
}

} // namespace

struct ShellState::Impl {
    std::unique_ptr<PaneNode> root =
        std::make_unique<PaneNode>(PaneNode{PaneId{1}});
    PaneId active{1};
    std::uint32_t nextId = 2;
    std::vector<std::string> providers;
    std::size_t providerIndex = 0;
    bool panelRequested = false;
    FocusTarget focus = FocusTarget::Editor;
    std::vector<FocusTarget> focusStack;
    // The focus present when the panel was last shown, so hiding a focused panel
    // restores it (a dedicated slot rather than the prompt focus_stack, so it
    // never orphans an entry when the panel is hidden while a prompt holds focus).
    std::optional<FocusTarget> focusBeforePanel;
    bool distractionFree = false;
};

ShellState::ShellState(std::vector<std::string> panelProviders)
    : impl_(std::make_unique<Impl>()) {
    impl_->providers = std::move(panelProviders);
}

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
    impl_->focus = FocusTarget::Editor;
    return true;
}

void ShellState::togglePanel() noexcept {
    const bool showing = !impl_->panelRequested;
    impl_->panelRequested = showing;
    if (showing) {
        // Showing the panel moves focus to it (remembering the prior focus so
        // hiding can restore it), when the panel can actually take focus.
        if (!impl_->providers.empty()) {
            impl_->focusBeforePanel = impl_->focus;
            impl_->focus = FocusTarget::Panel;
        }
    } else if (impl_->focus == FocusTarget::Panel) {
        // Hiding the focused panel restores the focus that was present when it
        // was shown; never restore to the panel itself or a transient prompt.
        FocusTarget restored =
            impl_->focusBeforePanel.value_or(FocusTarget::Editor);
        if (restored == FocusTarget::Panel || restored == FocusTarget::Prompt) {
            restored = FocusTarget::Editor;
        }
        impl_->focus = restored;
        impl_->focusBeforePanel.reset();
    }
}

bool ShellState::focusPanel() noexcept {
    if (!impl_->panelRequested || impl_->providers.empty()) return false;
    impl_->focus = FocusTarget::Panel;
    return true;
}

bool ShellState::showPanelProvider(std::string_view provider) noexcept {
    auto found = std::find(impl_->providers.begin(), impl_->providers.end(),
                           provider);
    if (found == impl_->providers.end()) return false;
    auto index = static_cast<std::size_t>(
        std::distance(impl_->providers.begin(), found));
    if (impl_->panelRequested && impl_->providerIndex == index) {
        togglePanel();
        return true;
    }
    impl_->providerIndex = index;
    if (!impl_->panelRequested) {
        togglePanel();
    }
    return true;
}

void ShellState::focusEditor() noexcept {
    impl_->focus = FocusTarget::Editor;
    impl_->focusStack.clear();
}

void ShellState::enterPromptFocus() noexcept {
    impl_->focusStack.push_back(impl_->focus);
    impl_->focus = FocusTarget::Prompt;
}

void ShellState::exitPromptFocus() noexcept {
    FocusTarget restored = FocusTarget::Editor;
    if (!impl_->focusStack.empty()) {
        restored = impl_->focusStack.back();
        impl_->focusStack.pop_back();
    }
    if (restored == FocusTarget::Panel && !impl_->panelRequested) {
        restored = FocusTarget::Editor;
    }
    impl_->focus = restored;
}

FocusTarget ShellState::focus() const noexcept { return impl_->focus; }

void ShellState::nextPanelProvider() noexcept {
    if (!impl_->providers.empty()) {
        impl_->providerIndex = (impl_->providerIndex + 1) % impl_->providers.size();
    }
}

void ShellState::previousPanelProvider() noexcept {
    if (!impl_->providers.empty()) {
        impl_->providerIndex =
            (impl_->providerIndex + impl_->providers.size() - 1) %
            impl_->providers.size();
    }
}

bool ShellState::panelRequested() const noexcept {
    return impl_->panelRequested;
}

bool ShellState::panelFocused() const noexcept {
    return impl_->focus == FocusTarget::Panel;
}

std::string_view ShellState::activePanelProvider() const noexcept {
    return impl_->providers.empty() ? std::string_view{} :
                                     impl_->providers[impl_->providerIndex];
}

void ShellState::toggleDistractionFree() noexcept {
    impl_->distractionFree = !impl_->distractionFree;
}

bool ShellState::distractionFree() const noexcept {
    return impl_->distractionFree;
}

ShellLayoutResult computeShellLayout(const ShellLayoutRequest& request,
                                       const ShellState& state) {
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
    view.focus = state.focus();
    const bool distractionFree = state.impl_->distractionFree;
    const int gutterWidth = request.style.dimensions.scrollbarGutterWidth;
    const int headerHeight = request.style.dimensions.headerHeight;
    const int footerHeight = request.style.dimensions.footerHeight;
    const int tabBarHeight = request.style.dimensions.tabBarHeight;
    // A picker (palette / file find) is not a document view, so it covers the
    // tab bar rather than sitting below it (removing the confusing visible tabs
    // and the one-row gap between the input line and the results).
    const bool showTabBar = !request.inputLineActive;

    // Region geometry comes from the box-tree solver (doc/spec-layout-engine.md).
    // Sizing POLICY stays here: the panel width is decided with the same rule as
    // before (yield to keep the editor's minimum; absent below the threshold) and
    // handed to the builder, which turns it and the chrome heights into a tree the
    // solver places. The content packers and the accessibility-node emission below
    // are unchanged -- they receive the solved rects instead of hand-built ones.
    int panelWidth = 0;
    if (!distractionFree && state.impl_->panelRequested &&
        request.viewport.columns >=
            request.style.dimensions.editorMinimumWidth +
                request.style.dimensions.panelMinimumWidth) {
        panelWidth = std::min(request.style.dimensions.panelTargetWidth,
                              request.viewport.columns -
                                  request.style.dimensions.editorMinimumWidth);
    }
    auto solved = solveLayout(
        buildShellTree(distractionFree, headerHeight, footerHeight, tabBarHeight,
                       panelWidth, showTabBar),
        {0, 0, request.viewport.columns, request.viewport.rows});
    if (!solved) {
        return {ShellLayoutError{ShellLayoutErrorCode::ViewportTooSmall,
                                 "viewport too small for the shell layout"},
                std::nullopt};
    }
    Rect editor = solved->find("document")->rect;

    if (!distractionFree) {
        view.header = solved->find("header")->rect;
        view.footer = solved->find("footer")->rect;
        addNode(view, ShellNodeKind::Header, "header", "Status header",
                 *view.header, SemanticRole::Header);
        addNode(view, ShellNodeKind::Footer, "footer", "Status footer",
                 *view.footer, SemanticRole::Footer);
        // Status fields FIRST, anchored at the header's left edge, so their
        // position does not depend on the input line's contents: typing into a
        // picker must not slide the working directory and branch rightward or
        // collapse them out (doc/spec-input-line.md).
        //
        // The input line's needs are subtracted from the fields' width BEFORE
        // they are laid out, never after. Emitting fields first and then pulling
        // the input line back over them would overlap two nodes on the same
        // cells, and hit-testing takes the FIRST node containing a cell, so a
        // click on visible query cells would dispatch the field's command.
        // Reserving up front keeps the fields' own collapse-rank logic the one
        // mechanism that decides what fits.
        const int headerRight = view.header->right();
        int fieldWidth = view.header->width;
        if (request.inputLineActive) {
            const int reserved = std::min(
                request.style.inputLineReservation(), view.header->width);
            fieldWidth = std::max(0, view.header->width - reserved);
        }

        // Header status fields are a WidgetStack LEFT collapse group over the
        // field width (doc/spec-chrome-stacks.md). The input line's fixed
        // reservation is already subtracted from `fieldWidth`, so the fields
        // collapse independently of the query -- the reservation is the floor
        // that protects the input's room, and the fields never reflow as the
        // user types (doc/spec-input-line.md). The input line + ghost keep the
        // `layoutTextInput` seam below (they become a prompt-mode TextInput
        // widget in the focus phase, where reserve/grow/ghost geometry is
        // designed rather than shoehorned into a stack item).
        WidgetStack headerStack{1};
        std::vector<const StatusField*> headerFieldSource;
        for (const auto& field : request.headerFields) {
            if (field.accessibleLabel.empty() || field.value.empty()) continue;
            StackItem item;
            item.id = "F" + std::to_string(headerFieldSource.size());
            item.content = field.value;
            item.desired = measureFieldCells(field.value);
            item.rank = field.collapseRank;
            headerStack.packLeft(std::move(item));
            headerFieldSource.push_back(&field);
        }
        int headerX = view.header->x;
        if (const auto resolved = headerStack.resolve(fieldWidth)) {
            const auto find = [&](std::string_view id) -> const StackPlacement* {
                for (const auto& p : resolved->placed)
                    if (p.id == id) return &p;
                return nullptr;
            };
            for (std::size_t i = 0; i < headerFieldSource.size(); ++i) {
                const auto* p = find("F" + std::to_string(i));
                if (!p) continue;
                const StatusField& field = *headerFieldSource[i];
                addNode(view, ShellNodeKind::HeaderField, field.id,
                         field.accessibleLabel,
                         {view.header->x + p->offset, view.header->y, p->size, 1},
                         SemanticRole::Header, field.value, field.commandId);
                headerX = std::max(headerX,
                                   view.header->x + p->offset + p->size);
            }
        }
        // A space between the fields and whatever follows them.
        if (headerX > view.header->x) ++headerX;

        // The input line occupies this slot when a picker is open.
        if (request.inputLineActive) {
            const int available = std::max(0, headerRight - headerX);
            // Scroll the query rather than reclaim field width when it outgrows
            // the space: the fields' positions are the thing being protected,
            // and the end of the query is what the user is looking at. The "> "
            // sigil stays put as the surface's identity while the text slides
            // under it. The TextInput seam owns the caret reservation and tail
            // scroll (doc/spec-widget-chrome.md §TextInput seam).
            const auto input = layoutTextInput(request.style.inputLineSigil,
                                               request.inputLineQuery, available);
            addNode(view, ShellNodeKind::HeaderField, "input_line.query",
                     "Input line", {headerX, view.header->y, input.width, 1},
                     SemanticRole::Prompt, input.text);
            headerX += input.width;
            if (!request.inputLineGhost.empty() && headerX < headerRight) {
                const int ghostWidth =
                    std::min(headerRight - headerX,
                             displayCells(request.inputLineGhost));
                addNode(view, ShellNodeKind::HeaderField, "input_line.ghost",
                         "Input line completion",
                         {headerX, view.header->y, ghostWidth, 1},
                         SemanticRole::LineNumber, request.inputLineGhost);
                headerX += ghostWidth;
            }
        }
        // The whole footer row is ONE WidgetStack (doc/spec-chrome-stacks.md):
        // status fields are the LEFT collapse group, the help hint + status
        // actions are the RIGHT group (hint leftmost, actions after it, the last
        // action flush right -- the packEnd rule). The left fields resolve over
        // the space to the LEFT of the right group automatically (the stack's
        // rightStart), so no explicit fieldsWidth arithmetic. Synthetic per-item
        // ids ("F"/"H"/"A") key the placement lookup, so no field/action id
        // uniqueness is assumed. Nodes are emitted in the SAME order as before
        // (actions reverse, hint, then fields) so the golden node sequence and
        // hit-test order are unchanged.
        const int labelPadding = request.style.dimensions.labelPadding;
        const bool hasHint =
            request.footerHint && !request.footerHint->label.empty();

        WidgetStack footer{1};
        std::vector<const StatusField*> fieldSource;
        for (const auto& field : request.footerFields) {
            if (field.accessibleLabel.empty() || field.value.empty()) continue;
            StackItem item;
            item.id = "F" + std::to_string(fieldSource.size());
            item.content = field.value;
            item.desired = measureFieldCells(field.value);
            item.rank = field.collapseRank;
            footer.packLeft(std::move(item));
            fieldSource.push_back(&field);
        }
        if (hasHint) {
            StackItem item;
            item.id = "H";
            item.content = request.footerHint->label;
            item.desired = displayCells(request.footerHint->label) + labelPadding;
            item.overflow = Overflow::Truncate;
            footer.packRight(std::move(item));
        }
        std::vector<const ShellLabel*> actionSource;
        for (const auto& action : request.footerActions) {
            if (action.accessibleLabel.empty()) continue;
            StackItem item;
            item.id = "A" + std::to_string(actionSource.size());
            item.content = action.accessibleLabel;
            item.desired = displayCells(action.accessibleLabel) + labelPadding;
            item.overflow = Overflow::Truncate;
            footer.packRight(std::move(item));
            actionSource.push_back(&action);
        }

        const auto resolved = footer.resolve(view.footer->width);
        if (resolved) {
            const auto find = [&](std::string_view id) -> const StackPlacement* {
                for (const auto& p : resolved->placed)
                    if (p.id == id) return &p;
                return nullptr;
            };
            const auto rectOf = [&](const StackPlacement& p) {
                return Rect{view.footer->x + p.offset, view.footer->y, p.size, 1};
            };

            // Actions in reverse original order, then the hint (unchanged node
            // order).
            for (std::size_t i = actionSource.size(); i-- > 0;) {
                const auto* p = find("A" + std::to_string(i));
                if (!p) continue;
                addNode(view, ShellNodeKind::FooterAction, actionSource[i]->id,
                         actionSource[i]->accessibleLabel, rectOf(*p),
                         SemanticRole::StatusInfo, actionSource[i]->accessibleLabel);
            }
            if (hasHint) {
                if (const auto* p = find("H")) {
                    addNode(view, ShellNodeKind::FooterHint, "footer.hint",
                             request.footerHint->label, rectOf(*p),
                             SemanticRole::Footer, request.footerHint->label,
                             request.footerHint->commandId);
                }
            }
            // Status fields left-to-right.
            for (std::size_t i = 0; i < fieldSource.size(); ++i) {
                const auto* p = find("F" + std::to_string(i));
                if (!p) continue;
                const StatusField& field = *fieldSource[i];
                addNode(view, ShellNodeKind::FooterField, field.id,
                         field.accessibleLabel, rectOf(*p), SemanticRole::Footer,
                         field.value, field.commandId);
            }
        }

        if (panelWidth > 0) {
            view.panel = solved->find("panel")->rect;
            addNode(view, ShellNodeKind::Panel, "panel", "Side panel",
                     *view.panel, SemanticRole::PanelInactive);
            addNode(view, ShellNodeKind::PanelProvider, "panel.provider",
                     request.panelProviderLabel, *view.panel,
                     state.impl_->focus == FocusTarget::Panel ? SemanticRole::PanelActive :
                                                  SemanticRole::PanelInactive,
                     request.panelProviderLabel);
            // Reserve the tree's scrollbar gutter: the right column over the tree
            // content rows (below the provider-label row). Content is the panel
            // minus this column, so tree text width never changes with the thumb.
            if (panelWidth > gutterWidth && view.panel->height > 1) {
                view.panelScrollbar =
                    Rect{panelWidth - gutterWidth, view.panel->y + 1,
                         gutterWidth, view.panel->height - 1};
                addNode(view, ShellNodeKind::Scrollbar, "panel.scrollbar",
                         "Panel scrollbar", *view.panelScrollbar,
                         SemanticRole::ScrollbarTrack);
            }
        }

        editor = solved->find("document")->rect;
        if (showTabBar) {
            view.tabBar = solved->find("tabbar")->rect;
            addNode(view, ShellNodeKind::TabBar, "tabs", "Open tabs",
                     *view.tabBar, SemanticRole::TabInactive);
            // A tab draws as leftEdge + title[+dirtySuffix] + rightEdge; its width
            // is measured from that, with no fixed padding.  Consecutive tabs are
            // parted by the separator glyph, emitted as its own non-interactive
            // node.
            auto const chipDisplay = [&](TabLabel const& tab) {
                std::string display = request.style.tab.leftEdge + tab.title;
                if (tab.dirty) display += request.style.tab.dirtySuffix;
                display += request.style.tab.rightEdge;
                return display;
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
            // The prompt occupies the bottom rows of the screen (over the
            // footer), which is where promptStatusView renders it.  Shrink the
            // editor from the BOTTOM -- never move its top -- so opening a prompt
            // reduces document height without pushing content down.
            const int promptTop =
                request.viewport.rows - request.reservedPromptRows;
            view.prompt = Rect{editor.x, promptTop, editor.width,
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

    const int lineNumberWidth = std::max(0, request.lineNumberGutterWidth);
    const int editorMinimumWidth = request.style.dimensions.editorMinimumWidth;
    if (canLayout(*state.impl_->root, editor)) {
        layoutPanes(*state.impl_->root, editor, view.panes, gutterWidth,
                    lineNumberWidth, editorMinimumWidth);
    } else {
        int panelessNumbers = lineNumberWidth;
        if (panelessNumbers > 0 &&
            editor.width - gutterWidth - panelessNumbers < editorMinimumWidth) {
            panelessNumbers = 0;
        }
        view.panes.push_back({
            state.impl_->active,
            editor,
            {editor.x + panelessNumbers, editor.y,
             editor.width - gutterWidth - panelessNumbers, editor.height},
            {editor.right() - gutterWidth, editor.y, gutterWidth, editor.height},
            panelessNumbers > 0
                ? Rect{editor.x, editor.y, panelessNumbers, editor.height}
                : Rect{0, 0, 0, 0},
        });
    }

    for (const auto& pane : view.panes) {
        const auto suffix = std::to_string(pane.id.value());
        addNode(view, ShellNodeKind::Pane, "pane." + suffix,
                 "Editor pane " + suffix, pane.frame, SemanticRole::Canvas);
        addNode(view, ShellNodeKind::Scrollbar, "pane." + suffix + ".scrollbar",
                 "Scrollbar for editor pane " + suffix, pane.scrollbar,
                 SemanticRole::ScrollbarTrack);
        if (request.emptyState) {
            addNode(view, ShellNodeKind::EmptyState,
                     "pane." + suffix + ".empty", "empty editor",
                     pane.content, SemanticRole::Canvas, "empty editor");
        }
    }

    return {std::nullopt, std::move(view)};
}

} // namespace ssg
