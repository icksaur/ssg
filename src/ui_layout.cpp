#include "ssg/ui_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ssg {
namespace {

constexpr int minimum_columns = 20;
constexpr int minimum_rows = 4;
constexpr int panel_target_width = 24;
constexpr int panel_minimum_width = 12;
constexpr int editor_minimum_width = 20;

struct PaneNode {
    PaneId id;
    SplitAxis axis = SplitAxis::vertical;
    std::unique_ptr<PaneNode> first;
    std::unique_ptr<PaneNode> second;

    [[nodiscard]] bool leaf() const noexcept { return !first; }
};

void collect_ids(const PaneNode& node, std::vector<PaneId>& ids) {
    if (node.leaf()) {
        ids.push_back(node.id);
        return;
    }
    collect_ids(*node.first, ids);
    collect_ids(*node.second, ids);
}

PaneNode* find_leaf(PaneNode& node, PaneId id) {
    if (node.leaf()) return node.id == id ? &node : nullptr;
    if (auto* found = find_leaf(*node.first, id)) return found;
    return find_leaf(*node.second, id);
}

bool remove_leaf(std::unique_ptr<PaneNode>& node, PaneId id) {
    if (!node || node->leaf()) return false;
    if (node->first->leaf() && node->first->id == id) {
        node = std::move(node->second);
        return true;
    }
    if (node->second->leaf() && node->second->id == id) {
        node = std::move(node->first);
        return true;
    }
    return remove_leaf(node->first, id) || remove_leaf(node->second, id);
}

bool can_layout(const PaneNode& node, Rect rect) {
    if (node.leaf()) return rect.width >= 2 && rect.height >= 1;
    if (node.axis == SplitAxis::vertical) {
        const int first_width = rect.width / 2;
        return can_layout(*node.first, {rect.x, rect.y, first_width, rect.height}) &&
               can_layout(*node.second,
                          {rect.x + first_width, rect.y,
                           rect.width - first_width, rect.height});
    }
    const int first_height = rect.height / 2;
    return can_layout(*node.first, {rect.x, rect.y, rect.width, first_height}) &&
           can_layout(*node.second,
                      {rect.x, rect.y + first_height, rect.width,
                       rect.height - first_height});
}

void layout_panes(const PaneNode& node, Rect rect,
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
    if (node.axis == SplitAxis::vertical) {
        const int first_width = rect.width / 2;
        layout_panes(*node.first, {rect.x, rect.y, first_width, rect.height},
                     output);
        layout_panes(*node.second,
                     {rect.x + first_width, rect.y,
                      rect.width - first_width, rect.height},
                     output);
        return;
    }
    const int first_height = rect.height / 2;
    layout_panes(*node.first, {rect.x, rect.y, rect.width, first_height},
                 output);
    layout_panes(*node.second,
                 {rect.x, rect.y + first_height, rect.width,
                  rect.height - first_height},
                 output);
}

void add_node(ShellViewState& view, ShellNodeKind kind, std::string id,
              std::string label, Rect rect, SemanticRole role,
              std::string content = {}) {
    view.accessibility_nodes.push_back(
        {kind, std::move(id), std::move(label), rect, role, std::move(content)});
}

void add_fields(ShellViewState& view, const std::vector<StatusField>& fields,
                Rect row, ShellNodeKind kind, SemanticRole role) {
    std::vector<const StatusField*> prioritized;
    prioritized.reserve(fields.size());
    for (const auto& field : fields) prioritized.push_back(&field);
    std::ranges::stable_sort(prioritized, {}, &StatusField::collapse_rank);

    std::vector<const StatusField*> retained;
    int used = 0;
    for (const auto* field : prioritized) {
        if (field->accessible_label.empty() || field->value.empty()) continue;
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
        add_node(view, kind, field->id, field->accessible_label,
                 {x, row.y, width, 1}, role, field->value);
        x += width;
    }
}

const PaneGeometry* pane_geometry(const ShellViewState& view, PaneId id) {
    const auto found = std::ranges::find(view.panes, id, &PaneGeometry::id);
    return found == view.panes.end() ? nullptr : &*found;
}

double center_x(const Rect& rect) { return rect.x + rect.width / 2.0; }
double center_y(const Rect& rect) { return rect.y + rect.height / 2.0; }

} // namespace

struct ShellState::Impl {
    std::unique_ptr<PaneNode> root =
        std::make_unique<PaneNode>(PaneNode{PaneId{1}});
    PaneId active{1};
    std::uint32_t next_id = 2;
    std::vector<std::string> providers;
    std::size_t provider_index = 0;
    bool panel_requested = false;
    FocusTarget focus = FocusTarget::editor;
    std::vector<FocusTarget> focus_stack;
    bool distraction_free = false;
};

ShellState::ShellState(std::vector<std::string> panel_providers)
    : impl_(std::make_unique<Impl>()) {
    impl_->providers = std::move(panel_providers);
}

ShellState::~ShellState() = default;
ShellState::ShellState(ShellState&&) noexcept = default;
ShellState& ShellState::operator=(ShellState&&) noexcept = default;

PaneId ShellState::active_pane() const noexcept { return impl_->active; }

std::size_t ShellState::pane_count() const noexcept {
    std::vector<PaneId> ids;
    collect_ids(*impl_->root, ids);
    return ids.size();
}

PaneId ShellState::split_active(SplitAxis axis) {
    auto* leaf = find_leaf(*impl_->root, impl_->active);
    const PaneId original = leaf->id;
    const PaneId created{impl_->next_id++};
    leaf->axis = axis;
    leaf->first = std::make_unique<PaneNode>(PaneNode{original});
    leaf->second = std::make_unique<PaneNode>(PaneNode{created});
    impl_->active = created;
    return created;
}

bool ShellState::close_active_pane() {
    if (pane_count() == 1) return false;
    std::vector<PaneId> ids;
    collect_ids(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    const PaneId replacement =
        active + 1 != ids.end() ? *(active + 1) : *(active - 1);
    const bool removed = remove_leaf(impl_->root, impl_->active);
    if (removed) impl_->active = replacement;
    return removed;
}

void ShellState::next_pane() noexcept {
    std::vector<PaneId> ids;
    collect_ids(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = *(active + 1 == ids.end() ? ids.begin() : active + 1);
}

void ShellState::previous_pane() noexcept {
    std::vector<PaneId> ids;
    collect_ids(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = active == ids.begin() ? ids.back() : *(active - 1);
}

bool ShellState::focus_pane(PaneDirection direction,
                            const ShellViewState& view) noexcept {
    const auto* current = pane_geometry(view, impl_->active);
    if (!current) return false;
    const double current_x = center_x(current->frame);
    const double current_y = center_y(current->frame);
    const PaneGeometry* best = nullptr;
    double best_distance = std::numeric_limits<double>::max();
    for (const auto& candidate : view.panes) {
        if (candidate.id == impl_->active) continue;
        const double dx = center_x(candidate.frame) - current_x;
        const double dy = center_y(candidate.frame) - current_y;
        const bool eligible =
            (direction == PaneDirection::left && dx < 0) ||
            (direction == PaneDirection::right && dx > 0) ||
            (direction == PaneDirection::up && dy < 0) ||
            (direction == PaneDirection::down && dy > 0);
        if (!eligible) continue;
        const double distance = dx * dx + dy * dy;
        if (distance < best_distance) {
            best = &candidate;
            best_distance = distance;
        }
    }
    if (!best) return false;
    impl_->active = best->id;
    impl_->focus = FocusTarget::editor;
    return true;
}

void ShellState::toggle_panel() noexcept {
    impl_->panel_requested = !impl_->panel_requested;
    if (!impl_->panel_requested && impl_->focus == FocusTarget::panel) {
        impl_->focus = FocusTarget::editor;
    }
}

bool ShellState::focus_panel() noexcept {
    if (!impl_->panel_requested || impl_->providers.empty()) return false;
    impl_->focus = FocusTarget::panel;
    return true;
}

void ShellState::focus_editor() noexcept {
    impl_->focus = FocusTarget::editor;
    impl_->focus_stack.clear();
}

void ShellState::enter_prompt_focus() noexcept {
    impl_->focus_stack.push_back(impl_->focus);
    impl_->focus = FocusTarget::prompt;
}

void ShellState::exit_prompt_focus() noexcept {
    FocusTarget restored = FocusTarget::editor;
    if (!impl_->focus_stack.empty()) {
        restored = impl_->focus_stack.back();
        impl_->focus_stack.pop_back();
    }
    if (restored == FocusTarget::panel && !impl_->panel_requested) {
        restored = FocusTarget::editor;
    }
    impl_->focus = restored;
}

FocusTarget ShellState::focus() const noexcept { return impl_->focus; }

void ShellState::next_panel_provider() noexcept {
    if (!impl_->providers.empty()) {
        impl_->provider_index = (impl_->provider_index + 1) % impl_->providers.size();
    }
}

void ShellState::previous_panel_provider() noexcept {
    if (!impl_->providers.empty()) {
        impl_->provider_index =
            (impl_->provider_index + impl_->providers.size() - 1) %
            impl_->providers.size();
    }
}

bool ShellState::panel_requested() const noexcept {
    return impl_->panel_requested;
}

bool ShellState::panel_focused() const noexcept {
    return impl_->focus == FocusTarget::panel;
}

std::string_view ShellState::active_panel_provider() const noexcept {
    return impl_->providers.empty() ? std::string_view{} :
                                     impl_->providers[impl_->provider_index];
}

void ShellState::toggle_distraction_free() noexcept {
    impl_->distraction_free = !impl_->distraction_free;
}

bool ShellState::distraction_free() const noexcept {
    return impl_->distraction_free;
}

ShellLayoutResult compute_shell_layout(const ShellLayoutRequest& request,
                                       const ShellState& state) {
    if (request.viewport.columns < minimum_columns ||
        request.viewport.rows < minimum_rows) {
        return {ShellLayoutError{ShellLayoutErrorCode::viewport_too_small,
                                 "viewport must be at least 20 columns by 4 rows"},
                std::nullopt};
    }
    if (request.reserved_prompt_rows > 3) {
        return {ShellLayoutError{ShellLayoutErrorCode::invalid_prompt_rows,
                                 "reserved prompt rows must be in [0, 3]"},
                std::nullopt};
    }

    ShellViewState view;
    view.viewport = request.viewport;
    view.focus = state.focus();
    const bool distraction_free = state.impl_->distraction_free;
    Rect editor{0, 0, request.viewport.columns, request.viewport.rows};

    if (!distraction_free) {
        view.header = Rect{0, 0, request.viewport.columns, 1};
        view.footer =
            Rect{0, request.viewport.rows - 1, request.viewport.columns, 1};
        add_node(view, ShellNodeKind::header, "header", "Status header",
                 *view.header, SemanticRole::header);
        add_node(view, ShellNodeKind::footer, "footer", "Status footer",
                 *view.footer, SemanticRole::footer);
        int header_x = view.header->x;
        if (request.palette_active) {
            // The palette owns the header while open: its query renders in the
            // prompt role, the ghost completion trails it dim.  Palette focus
            // and leader-chord entry are mutually exclusive, so they never
            // compete for the header start.
            std::string query = "> " + request.palette_query;
            const int query_width =
                std::min(view.header->width, static_cast<int>(query.size()));
            add_node(view, ShellNodeKind::header_field, "palette_query",
                     "Palette query", {header_x, view.header->y, query_width, 1},
                     SemanticRole::prompt, std::move(query));
            header_x += query_width;
            if (!request.palette_ghost.empty() &&
                header_x < view.header->right()) {
                const int ghost_width =
                    std::min(view.header->right() - header_x,
                             static_cast<int>(request.palette_ghost.size()));
                add_node(view, ShellNodeKind::header_field, "palette_ghost",
                         "Palette completion",
                         {header_x, view.header->y, ghost_width, 1},
                         SemanticRole::line_number, request.palette_ghost);
                header_x += ghost_width;
            }
        } else if (!request.leader_hint.empty()) {
            const int width = std::min(
                view.header->width,
                static_cast<int>(request.leader_hint.size()) + 1);
            add_node(view, ShellNodeKind::header_field, "leader", "Leader hint",
                     {header_x, view.header->y, width, 1}, SemanticRole::prompt,
                     request.leader_hint);
            header_x += width;
        }
        add_fields(view, request.header_fields,
                   {header_x, view.header->y,
                    view.header->right() - header_x, view.header->height},
                   ShellNodeKind::header_field, SemanticRole::header);
        int action_x = view.footer->right();
        for (auto action = request.footer_actions.rbegin();
             action != request.footer_actions.rend(); ++action) {
            const int width =
                std::min(action_x, static_cast<int>(action->accessible_label.size()) + 2);
            if (width <= 0 || action->accessible_label.empty()) continue;
            action_x -= width;
            add_node(view, ShellNodeKind::footer_action, action->id,
                     action->accessible_label,
                     {action_x, view.footer->y, width, 1},
                     SemanticRole::status_info, action->accessible_label);
        }
        add_fields(view, request.footer_fields,
                   {view.footer->x, view.footer->y,
                    action_x - view.footer->x, view.footer->height},
                   ShellNodeKind::footer_field, SemanticRole::footer);

        const bool panel_requested = state.impl_->panel_requested;
        int panel_width = 0;
        if (panel_requested &&
            request.viewport.columns >=
                editor_minimum_width + panel_minimum_width) {
            panel_width = std::min(
                panel_target_width,
                request.viewport.columns - editor_minimum_width);
            view.panel = Rect{0, 1, panel_width, request.viewport.rows - 2};
            add_node(view, ShellNodeKind::panel, "panel", "Side panel",
                     *view.panel, SemanticRole::panel_inactive);
            add_node(view, ShellNodeKind::panel_provider, "panel.provider",
                     request.panel_provider_label, *view.panel,
                     state.impl_->focus == FocusTarget::panel ? SemanticRole::panel_active :
                                                  SemanticRole::panel_inactive,
                     request.panel_provider_label);
            // Reserve the tree's scrollbar gutter: the right column over the tree
            // content rows (below the provider-label row). Content is the panel
            // minus this column, so tree text width never changes with the thumb.
            if (panel_width > 1 && view.panel->height > 1) {
                view.panel_scrollbar = Rect{panel_width - 1, view.panel->y + 1, 1,
                                            view.panel->height - 1};
                add_node(view, ShellNodeKind::scrollbar, "panel.scrollbar",
                         "Panel scrollbar", *view.panel_scrollbar,
                         SemanticRole::scrollbar_track);
            }
        }

        editor = {panel_width, 1, request.viewport.columns - panel_width,
                  request.viewport.rows - 2};
        view.tab_bar = Rect{editor.x, editor.y, editor.width, 1};
        add_node(view, ShellNodeKind::tab_bar, "tabs", "Open tabs",
                 *view.tab_bar, SemanticRole::tab_inactive);
        int tab_x = view.tab_bar->x;
        for (std::size_t i = 0; i < request.tabs.size(); ++i) {
            const auto& tab = request.tabs[i];
            const std::string display =
                tab.dirty ? tab.title + " *" : tab.title;
            const int width =
                std::min(view.tab_bar->right() - tab_x,
                         std::max(1, static_cast<int>(display.size()) + 2));
            if (width <= 0 || tab.accessible_label.empty()) break;
            add_node(view, ShellNodeKind::tab, "tab." + std::to_string(i),
                     tab.accessible_label,
                     {tab_x, view.tab_bar->y, width, 1},
                     tab.active ? SemanticRole::tab_active :
                                  SemanticRole::tab_inactive,
                     display);
            view.tab_hits.push_back(
                TabHit{{tab_x, view.tab_bar->y, width, 1},
                       static_cast<std::uint32_t>(i)});
            tab_x += width;
        }

        editor.y += 1;
        editor.height -= 1;
        if (request.reserved_prompt_rows >= editor.height) {
            return {ShellLayoutError{
                        ShellLayoutErrorCode::viewport_too_small,
                        "prompt reservation leaves no editor content row"},
                    std::nullopt};
        }
        if (request.reserved_prompt_rows > 0) {
            view.prompt = Rect{editor.x, editor.y, editor.width,
                               request.reserved_prompt_rows};
            add_node(view, ShellNodeKind::prompt_reservation, "prompt",
                     "Prompt surface", *view.prompt, SemanticRole::prompt);
            editor.y += request.reserved_prompt_rows;
            editor.height -= request.reserved_prompt_rows;
        }
    }

    if (can_layout(*state.impl_->root, editor)) {
        layout_panes(*state.impl_->root, editor, view.panes);
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
        add_node(view, ShellNodeKind::pane, "pane." + suffix,
                 "Editor pane " + suffix, pane.frame, SemanticRole::background);
        add_node(view, ShellNodeKind::scrollbar, "pane." + suffix + ".scrollbar",
                 "Scrollbar for editor pane " + suffix, pane.scrollbar,
                 SemanticRole::scrollbar_track);
        if (request.empty_state) {
            add_node(view, ShellNodeKind::empty_state,
                     "pane." + suffix + ".empty", "Empty editor",
                     pane.content, SemanticRole::background, "Empty editor");
        }
    }

    return {std::nullopt, std::move(view)};
}

} // namespace ssg
