#include "ssg/ui_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace ssg {
namespace {

constexpr int kMinimumColumns = 20;
constexpr int kMinimumRows = 4;
constexpr int kPanelTargetWidth = 24;
constexpr int kPanelMinimumWidth = 12;
constexpr int kEditorMinimumWidth = 20;

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
                  std::vector<PaneGeometry>& output) {
    if (node.leaf()) {
        output.push_back({
            node.id,
            rect,
            {rect.x, rect.y, rect.width - 1, rect.height},
            {rect.right() - 1, rect.y, 1, rect.height},
        });
        return;
    }
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        layoutPanes(*node.first, {rect.x, rect.y, firstWidth, rect.height},
                     output);
        layoutPanes(*node.second,
                     {rect.x + firstWidth, rect.y,
                      rect.width - firstWidth, rect.height},
                     output);
        return;
    }
    const int firstHeight = rect.height / 2;
    layoutPanes(*node.first, {rect.x, rect.y, rect.width, firstHeight},
                 output);
    layoutPanes(*node.second,
                 {rect.x, rect.y + firstHeight, rect.width,
                  rect.height - firstHeight},
                 output);
}

void addNode(ShellViewState& view, ShellNodeKind kind, std::string id,
              std::string label, Rect rect, SemanticRole role,
              std::string content = {}) {
    view.accessibilityNodes.push_back(
        {kind, std::move(id), std::move(label), rect, role, std::move(content)});
}

void addFields(ShellViewState& view, const std::vector<StatusField>& fields,
                Rect row, ShellNodeKind kind, SemanticRole role) {
    std::vector<const StatusField*> prioritized;
    prioritized.reserve(fields.size());
    for (const auto& field : fields) prioritized.push_back(&field);
    std::ranges::stable_sort(prioritized, {}, &StatusField::collapseRank);

    std::vector<const StatusField*> retained;
    int used = 0;
    for (const auto* field : prioritized) {
        if (field->accessibleLabel.empty() || field->value.empty()) continue;
        const int desired = std::max(1, static_cast<int>(field->value.size()) + 2);
        const int separator = retained.empty() ? 0 : 1;
        if (used + separator + desired > row.width) break;
        used += separator + desired;
        retained.push_back(field);
    }
    std::ranges::sort(retained, [](const auto* a, const auto* b) {
        return a < b;
    });

    int x = row.x;
    for (const auto* field : retained) {
        if (x != row.x) ++x;
        const int width = static_cast<int>(field->value.size()) + 2;
        addNode(view, kind, field->id, field->accessibleLabel,
                 {x, row.y, width, 1}, role, field->value);
        x += width;
    }
}

const PaneGeometry* paneGeometry(const ShellViewState& view, PaneId id) {
    const auto found = std::ranges::find(view.panes, id, &PaneGeometry::id);
    return found == view.panes.end() ? nullptr : &*found;
}

double centerX(const Rect& rect) { return rect.x + rect.width / 2.0; }
double centerY(const Rect& rect) { return rect.y + rect.height / 2.0; }

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
    if (request.viewport.columns < kMinimumColumns ||
        request.viewport.rows < kMinimumRows) {
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
    Rect editor{0, 0, request.viewport.columns, request.viewport.rows};

    if (!distractionFree) {
        view.header = Rect{0, 0, request.viewport.columns, 1};
        view.footer =
            Rect{0, request.viewport.rows - 1, request.viewport.columns, 1};
        addNode(view, ShellNodeKind::Header, "header", "Status header",
                 *view.header, SemanticRole::Header);
        addNode(view, ShellNodeKind::Footer, "footer", "Status footer",
                 *view.footer, SemanticRole::Footer);
        int headerX = view.header->x;
        if (request.paletteActive) {
            // The palette owns the header while open: its query renders in the
            // prompt role, the ghost completion trails it dim.  Palette focus
            // and leader-chord entry are mutually exclusive, so they never
            // compete for the header start.
            std::string query = "> " + request.paletteQuery;
            const int queryWidth =
                std::min(view.header->width, static_cast<int>(query.size()));
            addNode(view, ShellNodeKind::HeaderField, "palette_query",
                     "Palette query", {headerX, view.header->y, queryWidth, 1},
                     SemanticRole::Prompt, std::move(query));
            headerX += queryWidth;
            if (!request.paletteGhost.empty() &&
                headerX < view.header->right()) {
                const int ghostWidth =
                    std::min(view.header->right() - headerX,
                             static_cast<int>(request.paletteGhost.size()));
                addNode(view, ShellNodeKind::HeaderField, "palette_ghost",
                         "Palette completion",
                         {headerX, view.header->y, ghostWidth, 1},
                         SemanticRole::LineNumber, request.paletteGhost);
                headerX += ghostWidth;
            }
        } else if (!request.leaderHint.empty()) {
            const int width = std::min(
                view.header->width,
                static_cast<int>(request.leaderHint.size()) + 1);
            addNode(view, ShellNodeKind::HeaderField, "leader", "Leader hint",
                     {headerX, view.header->y, width, 1}, SemanticRole::Prompt,
                     request.leaderHint);
            headerX += width;
        }
        addFields(view, request.headerFields,
                   {headerX, view.header->y,
                    view.header->right() - headerX, view.header->height},
                   ShellNodeKind::HeaderField, SemanticRole::Header);
        int actionX = view.footer->right();
        for (auto action = request.footerActions.rbegin();
             action != request.footerActions.rend(); ++action) {
            const int width =
                std::min(actionX, static_cast<int>(action->accessibleLabel.size()) + 2);
            if (width <= 0 || action->accessibleLabel.empty()) continue;
            actionX -= width;
            addNode(view, ShellNodeKind::FooterAction, action->id,
                     action->accessibleLabel,
                     {actionX, view.footer->y, width, 1},
                     SemanticRole::StatusInfo, action->accessibleLabel);
        }
        addFields(view, request.footerFields,
                   {view.footer->x, view.footer->y,
                    actionX - view.footer->x, view.footer->height},
                   ShellNodeKind::FooterField, SemanticRole::Footer);

        const bool panelRequested = state.impl_->panelRequested;
        int panelWidth = 0;
        if (panelRequested &&
            request.viewport.columns >=
                kEditorMinimumWidth + kPanelMinimumWidth) {
            panelWidth = std::min(
                kPanelTargetWidth,
                request.viewport.columns - kEditorMinimumWidth);
            view.panel = Rect{0, 1, panelWidth, request.viewport.rows - 2};
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
            if (panelWidth > 1 && view.panel->height > 1) {
                view.panelScrollbar = Rect{panelWidth - 1, view.panel->y + 1, 1,
                                            view.panel->height - 1};
                addNode(view, ShellNodeKind::Scrollbar, "panel.scrollbar",
                         "Panel scrollbar", *view.panelScrollbar,
                         SemanticRole::ScrollbarTrack);
            }
        }

        editor = {panelWidth, 1, request.viewport.columns - panelWidth,
                  request.viewport.rows - 2};
        view.tabBar = Rect{editor.x, editor.y, editor.width, 1};
        addNode(view, ShellNodeKind::TabBar, "tabs", "Open tabs",
                 *view.tabBar, SemanticRole::TabInactive);
        int tabX = view.tabBar->x;
        for (std::size_t i = 0; i < request.tabs.size(); ++i) {
            const auto& tab = request.tabs[i];
            const std::string display =
                tab.dirty ? tab.title + " *" : tab.title;
            const int width =
                std::min(view.tabBar->right() - tabX,
                         std::max(1, static_cast<int>(display.size()) + 2));
            if (width <= 0 || tab.accessibleLabel.empty()) break;
            addNode(view, ShellNodeKind::Tab, "tab." + std::to_string(i),
                     tab.accessibleLabel,
                     {tabX, view.tabBar->y, width, 1},
                     tab.active ? SemanticRole::TabActive :
                                  SemanticRole::TabInactive,
                     display);
            view.tabHits.push_back(
                TabHit{{tabX, view.tabBar->y, width, 1},
                       static_cast<std::uint32_t>(i)});
            tabX += width;
        }

        editor.y += 1;
        editor.height -= 1;
        if (request.reservedPromptRows >= editor.height) {
            return {ShellLayoutError{
                        ShellLayoutErrorCode::ViewportTooSmall,
                        "prompt reservation leaves no editor content row"},
                    std::nullopt};
        }
        if (request.reservedPromptRows > 0) {
            view.prompt = Rect{editor.x, editor.y, editor.width,
                               request.reservedPromptRows};
            addNode(view, ShellNodeKind::PromptReservation, "prompt",
                     "Prompt surface", *view.prompt, SemanticRole::Prompt);
            editor.y += request.reservedPromptRows;
            editor.height -= request.reservedPromptRows;
        }
    }

    if (canLayout(*state.impl_->root, editor)) {
        layoutPanes(*state.impl_->root, editor, view.panes);
    } else {
        view.panes.push_back({
            state.impl_->active,
            editor,
            {editor.x, editor.y, editor.width - 1, editor.height},
            {editor.right() - 1, editor.y, 1, editor.height},
        });
    }

    for (const auto& pane : view.panes) {
        const auto suffix = std::to_string(pane.id.value());
        addNode(view, ShellNodeKind::Pane, "pane." + suffix,
                 "Editor pane " + suffix, pane.frame, SemanticRole::Background);
        addNode(view, ShellNodeKind::Scrollbar, "pane." + suffix + ".scrollbar",
                 "Scrollbar for editor pane " + suffix, pane.scrollbar,
                 SemanticRole::ScrollbarTrack);
        if (request.emptyState) {
            addNode(view, ShellNodeKind::EmptyState,
                     "pane." + suffix + ".empty", "Empty editor",
                     pane.content, SemanticRole::Background, "Empty editor");
        }
    }

    return {std::nullopt, std::move(view)};
}

} // namespace ssg
