#include "editor_runtime_internal.h"

#include <ssg/command_metadata.h>

#include <algorithm>
#include <variant>

namespace ssg {

DocumentViewState EditorRuntime::Impl::document_view() const {
    auto const* document = active_document();
    if (document == nullptr) return {Revision{0}, {}, ByteOffset{0}};
    auto snapshot = document->snapshot();
    auto caret = selection.selections.primary().active.byte_offset;
    if (caret.value() > snapshot.text.size()) caret = ByteOffset{snapshot.text.size()};
    return {snapshot.revision, std::move(snapshot.text), caret};
}

TextEncodingViewState EditorRuntime::Impl::text_encoding_view() const {
    auto state = active_workspace_state();
    return {state ? state->encoding : TextEncodingStatus{}};
}

std::string EditorRuntime::Impl::current_path_label() const {
    auto state = active_workspace_state();
    return state ? state->display_label : root.filename().string();
}

PromptStatusViewState EditorRuntime::Impl::prompt_status_view(ViewportDimensions dimensions) const {
    PromptStatusViewState view;
    auto rows = prompt_row_count(prompt.request() ? prompt.request()->kind : PromptKind::command_argument);
    Rect reservation{0, static_cast<int>(dimensions.rows > rows ? dimensions.rows - rows : 0),
                     static_cast<int>(dimensions.columns), static_cast<int>(rows)};
    auto prompt_layout = compute_prompt_layout(prompt, reservation);
    if (prompt_layout.accepted()) {
        view.prompt = prompt_layout.view;
        project_find_replace_prompt(*view.prompt);
    }
    view.status = status.view_state();
    return view;
}

void EditorRuntime::Impl::project_find_replace_prompt(PromptViewState& prompt_view) const {
    if (prompt_view.kind != PromptKind::find && prompt_view.kind != PromptKind::replace) {
        return;
    }
    auto const& find_state = find_replace.view_state();
    for (auto& control : prompt_view.controls) {
        switch (control.kind) {
            case PromptControlKind::input:
                if (control.id == "find.query") control.value = find_state.query;
                break;
            case PromptControlKind::count: {
                auto position = find_state.active_match ? *find_state.active_match + 1 : 0;
                control.value = std::to_string(position) + "/" +
                                std::to_string(find_state.matches.size());
                break;
            }
            case PromptControlKind::toggle:
                break;
        }
    }
}

ShellViewState EditorRuntime::Impl::shell_view(ViewportDimensions dimensions,
                                               KeySequence const& leader_pending,
                                               PaletteReport const& palette_report) const {
    std::vector<TabLabel> labels;
    for (auto const& tab : tabs.view_state().tabs) {
        labels.push_back({tab.label, tab.label,
                          tabs.view_state().active == tab.id, tab.dirty});
    }
    auto status_projection = status.footer_projection();
    auto follow_projection = follow.footer_projection();
    ShellLayoutRequest request;
    request.viewport = {static_cast<int>(dimensions.columns), static_cast<int>(dimensions.rows)};
    request.reserved_prompt_rows = prompt.active() ? prompt_row_count(prompt.request()->kind) : 0;
    request.empty_state = active_document() == nullptr;
    request.panel_provider_label = std::string{shell.active_panel_provider()};
    request.header_fields = {{"cwd", "Workspace", root.string(), 0},
                             {"file", "File", current_path_label(), 1}};
    request.footer_fields = {{"status", "Status", status_projection.value, 0},
                             {"follow", "Follow edits", follow_projection.mode, 1}};
    request.footer_actions = status_projection.actions;
    request.tabs = std::move(labels);
    if (!leader_pending.empty()) {
        std::string hint = "leader:";
        for (auto const& stroke : leader_pending) {
            hint += ' ';
            hint += format_key_stroke(stroke);
        }
        request.leader_hint = std::move(hint);
    }
    bool const palette_open = prompt.active() && prompt.request() &&
                              prompt.request()->kind == PromptKind::palette;
    if (palette_open) {
        request.palette_active = true;
        request.palette_query = palette_report.query;
        request.palette_ghost = palette_report.ghost;
    }
    auto result = compute_shell_layout(request, shell);
    if (!result.accepted()) return {};
    auto view = *result.view;

    if (!view.panes.empty()) {
        auto const& content = view.panes.front().content;
        last_pane_content_rows = static_cast<std::uint32_t>(std::max(content.height, 1));
        last_pane_content_columns = static_cast<std::uint32_t>(std::max(content.width, 1));
        last_reserved_prompt_rows = static_cast<std::uint32_t>(request.reserved_prompt_rows);
    }

    if (palette_open && !view.panes.empty()) {
        PaletteProjection projection;
        projection.rect = view.panes.front().content;
        projection.selected = palette_report.selected;
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
    if (auto id = active_document_id()) {
        auto found = histories.find(id->value());
        if (found != histories.end()) current_history = found->second.view_state();
    }
    return {document_view(),
            selection,
            current_history,
            clipboard.view_state(),
            prompt_status_view(dimensions),
            search.view_state(),
            find_replace.view_state(),
            settings.view_state(),
            keymap,
            text_encoding_view(),
            tabs.view_state(),
            diff.view_state(),
            external.view_state(),
            follow.view_state(),
            tree.view_state(),
            syntax.view_state(),
            lsp_sync,
            lsp_features,
            theme,
            shell_view(dimensions, leader_pending, palette_report),
            palette_view()};
}

PaletteViewState EditorRuntime::Impl::palette_view() const {
    PaletteViewState view;
    view.mode = SearchMode::command;
    for (auto const& descriptor : descriptors()) {
        std::string detail;
        if (auto sequence = preferred_binding(keymap, descriptor.id)) {
            detail = format_key_sequence(*sequence);
        }
        view.candidates.push_back(
            {descriptor.id, command_label(descriptor.id), std::move(detail)});
    }
    return view;
}

} // namespace ssg
