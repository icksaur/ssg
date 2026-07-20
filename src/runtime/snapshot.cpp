#include "editor_runtime_internal.h"

#include <ssg/command_metadata.h>

#include <algorithm>
#include <variant>

namespace ssg {

DocumentViewState EditorRuntime::Impl::documentView() const {
    auto const* document = activeDocument();
    if (document == nullptr) return {Revision{0}, {}, ByteOffset{0}};
    auto snapshot = document->snapshot();
    auto caret = selection.selections.primary().active.byte_offset;
    if (caret.value() > snapshot.text.size()) caret = ByteOffset{snapshot.text.size()};
    return {snapshot.revision, std::move(snapshot.text), caret};
}

TextEncodingViewState EditorRuntime::Impl::textEncodingView() const {
    auto state = activeWorkspaceState();
    return {state ? state->encoding : TextEncodingStatus{}};
}

std::string EditorRuntime::Impl::currentPathLabel() const {
    auto state = activeWorkspaceState();
    return state ? state->display_label : root.filename().string();
}

PromptStatusViewState EditorRuntime::Impl::promptStatusView(ViewportDimensions dimensions) const {
    PromptStatusViewState view;
    auto rows = promptRowCount(prompt.request() ? prompt.request()->kind : PromptKind::CommandArgument);
    Rect reservation{0, static_cast<int>(dimensions.rows > rows ? dimensions.rows - rows : 0),
                     static_cast<int>(dimensions.columns), static_cast<int>(rows)};
    auto prompt_layout = computePromptLayout(prompt, reservation);
    if (prompt_layout.accepted()) {
        view.prompt = prompt_layout.view;
        projectFindReplacePrompt(*view.prompt);
    }
    view.status = status.viewState();
    return view;
}

void EditorRuntime::Impl::projectFindReplacePrompt(PromptViewState& prompt_view) const {
    if (prompt_view.kind != PromptKind::Find && prompt_view.kind != PromptKind::Replace) {
        return;
    }
    auto const& find_state = find_replace.viewState();
    for (auto& control : prompt_view.controls) {
        switch (control.kind) {
            case PromptControlKind::Input:
                if (control.id == "find.query") control.value = find_state.query;
                else if (control.id == "replace.replacement")
                    control.value = find_state.replacement;
                break;
            case PromptControlKind::Count: {
                auto position = find_state.active_match ? *find_state.active_match + 1 : 0;
                control.value = std::to_string(position) + "/" +
                                std::to_string(find_state.matches.size());
                break;
            }
            case PromptControlKind::Toggle:
                if (control.id == "find.toggle_case")
                    control.checked = find_state.options.case_sensitive;
                else if (control.id == "find.toggle_whole_word")
                    control.checked = find_state.options.whole_word;
                else if (control.id == "find.toggle_regex")
                    control.checked = find_state.options.regex;
                break;
        }
    }
}

ShellViewState EditorRuntime::Impl::shellView(ViewportDimensions dimensions,
                                               KeySequence const& leader_pending,
                                               PaletteReport const& palette_report) const {
    std::vector<TabLabel> labels;
    for (auto const& tab : tabs.viewState().tabs) {
        labels.push_back({tab.label, tab.label,
                          tabs.viewState().active == tab.id, tab.dirty});
    }
    auto status_projection = status.footerProjection();
    auto follow_projection = follow.footerProjection();
    ShellLayoutRequest request;
    request.viewport = {static_cast<int>(dimensions.columns), static_cast<int>(dimensions.rows)};
    request.reserved_prompt_rows = prompt.active() ? promptRowCount(prompt.request()->kind) : 0;
    request.empty_state = activeDocument() == nullptr;
    request.panel_provider_label = std::string{shell.activePanelProvider()};
    request.header_fields = {{"cwd", "Workspace", root.string(), 0},
                             {"file", "File", currentPathLabel(), 1}};
    request.footer_fields = {{"status", "Status", status_projection.value, 0},
                             {"follow", "Follow edits", follow_projection.mode, 1}};
    request.footer_actions = status_projection.actions;
    request.tabs = std::move(labels);
    if (!leader_pending.empty()) {
        std::string hint = "leader:";
        for (auto const& stroke : leader_pending) {
            hint += ' ';
            hint += formatKeyStroke(stroke);
        }
        request.leader_hint = std::move(hint);
    }
    bool const palette_open = prompt.active() && prompt.request() &&
                              prompt.request()->kind == PromptKind::Palette;
    if (palette_open) {
        request.palette_active = true;
        request.palette_query = palette_report.query;
        request.palette_ghost = palette_report.ghost;
    }
    auto result = computeShellLayout(request, shell);
    if (!result.accepted()) return {};
    auto view = *result.view;

    if (!view.panes.empty()) {
        auto const& content = view.panes.front().content;
        last_pane_content_rows = static_cast<std::uint32_t>(std::max(content.height, 1));
        last_pane_content_columns = static_cast<std::uint32_t>(std::max(content.width, 1));
        last_reserved_prompt_rows = static_cast<std::uint32_t>(request.reserved_prompt_rows);
    }
    // Cache the tree content height (panel height minus the provider-label row)
    // so tree_view can resolve the scroll offset against the real panel size.
    last_panel_content_rows =
        view.panel ? static_cast<std::uint32_t>(std::max(view.panel->height - 1, 0))
                   : 0;

    if (palette_open && !view.panes.empty()) {
        PaletteProjection projection;
        projection.rect = view.panes.front().content;
        projection.scrollbar_rect = view.panes.front().scrollbar;
        projection.selected = palette_report.selected;
        projection.first_visible = palette_report.first_visible;
        projection.scrollbar = palette_report.scrollbar;
        for (auto const& candidate : palette_report.rows) {
            projection.rows.push_back({candidate.label, candidate.detail});
        }
        view.palette = std::move(projection);
    }
    return view;
}

SessionSnapshotSections EditorRuntime::Impl::sections(ViewportDimensions dimensions,
                                                     KeySequence const& leader_pending,
                                                     PaletteReport const& palette_report) const {
    auto current_history = HistoryViewState{false, false, 0};
    if (auto id = activeDocumentId()) {
        auto found = histories.find(id->value());
        if (found != histories.end()) current_history = found->second.viewState();
    }
    // Compute the shell layout first: it caches the panel height that tree_view
    // resolves the tree scroll offset against (the aggregate below does not
    // guarantee evaluation order).
    auto shell = shellView(dimensions, leader_pending, palette_report);
    auto tree_section = treeView();
    return {documentView(),
            selection,
            current_history,
            clipboard.viewState(),
            promptStatusView(dimensions),
            search.viewState(),
            find_replace.viewState(),
            settings.viewState(),
            keymap,
            textEncodingView(),
            tabs.viewState(),
            diff.viewState(),
            external.viewState(),
            follow.viewState(),
            std::move(tree_section),
            syntax.viewState(),
            lsp_sync,
            lsp_features,
            theme,
            std::move(shell),
            paletteView()};
}

TreeViewState EditorRuntime::Impl::treeView() const {
    auto view = tree.viewState();
    if (view.providers.empty()) return view;
    // Only the active (front) provider is rendered. Resolve a display window from
    // the command-set offset and the current client's panel height WITHOUT
    // persisting anything: keep-visible ran on the command path, so here we only
    // clamp the offset to this height and window the nodes. This keeps snapshot
    // generation a pure read (no cross-client scroll interference).
    auto& provider = view.providers.front();
    std::optional<std::uint32_t> selected_index;
    if (provider.selected) {
        for (std::size_t i = 0; i < provider.nodes.size(); ++i) {
            if (provider.nodes[i].node.id == *provider.selected) {
                selected_index = static_cast<std::uint32_t>(i);
                break;
            }
        }
    }
    auto scroll = computeListScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        last_panel_content_rows, tree_first_visible, selected_index,
        /*keep_selection_visible=*/false);
    provider.first_visible = scroll.first_visible;
    provider.scrollbar = scroll.scrollbar;
    provider.visible_node_ids.clear();
    provider.visible_node_ids.reserve(scroll.visible_count);
    for (std::uint32_t row = 0; row < scroll.visible_count; ++row) {
        provider.visible_node_ids.push_back(
            provider.nodes[scroll.first_visible + row].node.id);
    }
    return view;
}

void EditorRuntime::Impl::revealTreeSelection() {
    auto view = tree.viewState();
    if (view.providers.empty()) return;
    auto const& provider = view.providers.front();
    if (!provider.selected) return;
    std::optional<std::uint32_t> selected_index;
    for (std::size_t i = 0; i < provider.nodes.size(); ++i) {
        if (provider.nodes[i].node.id == *provider.selected) {
            selected_index = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (!selected_index) return;
    auto scroll = computeListScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        last_panel_content_rows, tree_first_visible, selected_index,
        /*keep_selection_visible=*/true);
    tree_first_visible = scroll.first_visible;
}

void EditorRuntime::Impl::scrollTree(std::int64_t rows) {
    auto view = tree.viewState();
    if (view.providers.empty()) return;
    auto const& provider = view.providers.front();
    // Resolve the current scroll geometry (read-only) to bound the offset, then
    // shift it by `rows`. keep_selection_visible is false: a wheel scroll moves
    // the viewport, not the selection (a later reveal_tree_selection re-snaps).
    auto scroll = computeListScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        last_panel_content_rows, tree_first_visible, std::nullopt,
        /*keep_selection_visible=*/false);
    // Saturating add-then-clamp: `rows` is a wire-decoded int64 and may be huge,
    // so guard the extremes before the add to avoid signed overflow. Within the
    // guarded range |rows| < maximum <= UINT32_MAX, so cur + rows cannot overflow.
    auto const maximum = static_cast<std::int64_t>(scroll.scrollbar.maximum_first_row);
    auto const current = static_cast<std::int64_t>(tree_first_visible);
    std::int64_t next;
    if (rows >= maximum) {
        next = maximum;
    } else if (rows <= -maximum) {
        next = 0;
    } else {
        next = std::clamp<std::int64_t>(current + rows, 0, maximum);
    }
    tree_first_visible = static_cast<std::uint32_t>(next);
}

PaletteViewState EditorRuntime::Impl::paletteView() const {
    PaletteViewState view;
    view.mode = SearchMode::Command;
    for (auto const& descriptor : descriptors()) {
        std::string detail;
        if (auto sequence = preferredBinding(keymap, descriptor.id)) {
            detail = formatKeySequence(*sequence);
        }
        view.candidates.push_back(
            {descriptor.id, commandLabel(descriptor.id), std::move(detail)});
    }
    return view;
}

} // namespace ssg
