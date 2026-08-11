#pragma once

#include <ssg/EditorSessionBuilder.h>
#include <ssg/FileCommands.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/Selection.h>
#include <ssg/session_snapshot.h>

#include <algorithm>
#include <any>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace e2e {

struct CanonicalState {
    ssg::Revision revision;
    std::string text;
    std::string clipboard;
    std::string label;
    std::size_t selection_count;
    std::uint32_t first_row;
    ssg::DocumentMode mode;
    ssg::TabKind tab_kind;
    ssg::FollowMode follow_mode;
    bool workspace_open;
    bool dirty;
    bool tab_open;
    bool word_wrap;
    bool operator==(CanonicalState const&) const = default;
};

inline CanonicalState canonical(ssg::SessionSnapshot const& snapshot) {
    auto const& sections = snapshot.sections();
    auto const* tab =
        sections.tabs.tabs.empty() ? nullptr : &sections.tabs.tabs.front();
    bool workspace_open = false;
    bool word_wrap = false;
    for (auto const& node : snapshot.presentation()->shell.accessibilityNodes) {
        workspace_open =
            workspace_open || node.label.starts_with("Workspace ");
        word_wrap = word_wrap || node.label == "Word wrap on";
    }
    return {
        snapshot.revision(),
        sections.document.text,
        sections.clipboard.plainText,
        tab ? tab->label : std::string{},
        sections.selection.items().size(),
        snapshot.presentation()->selectionNav.firstVisualRow,
        tab ? tab->mode : ssg::DocumentMode::Edit,
        tab ? tab->kind : ssg::TabKind::Document,
        sections.followEdits.mode,
        workspace_open,
        tab ? tab->dirty : false,
        tab != nullptr,
        word_wrap,
    };
}

inline void apply(CanonicalState& state, ssg::SessionDelta const& delta) {
    if (delta.baseRevision() != state.revision)
        throw std::runtime_error{"canonical delta base revision mismatch"};
    if (delta.document()) {
        auto const& document = *delta.document();
        auto const start = static_cast<std::size_t>(document.start.value());
        state.text.replace(start,
                           static_cast<std::size_t>(document.erasedBytes),
                           document.insertedText);
    }
    if (delta.selection().replacement) {
        state.selection_count = delta.selection().replacement->items().size();
    }
    if (delta.selectionNav().replacement) {
        state.first_row = delta.selectionNav().replacement->firstVisualRow;
    }
    if (delta.clipboard().replacement)
        state.clipboard = delta.clipboard().replacement->plainText;
    if (delta.tabs().state) {
        auto const& tabs = *delta.tabs().state;
        auto const* tab = tabs.tabs.empty() ? nullptr : &tabs.tabs.front();
        state.label = tab ? tab->label : std::string{};
        state.mode = tab ? tab->mode : ssg::DocumentMode::Edit;
        state.tab_kind = tab ? tab->kind : ssg::TabKind::Document;
        state.dirty = tab ? tab->dirty : false;
        state.tab_open = tab != nullptr;
    }
    if (delta.followEdits().replacement)
        state.follow_mode = delta.followEdits().replacement->mode;
    if (delta.shell().replacement) {
        state.workspace_open = false;
        state.word_wrap = false;
        for (auto const& node :
             delta.shell().replacement->accessibilityNodes) {
            state.workspace_open =
                state.workspace_open || node.label.starts_with("Workspace ");
            state.word_wrap = state.word_wrap || node.label == "Word wrap on";
        }
    }
    state.revision = delta.revision();
}

// Shared mutable state consumed by all four execution paths (direct API,
// loopback WebSocket, TUI, browser). Each fixture wraps this in a
// locally-threaded model that adds sections(), viewport(), and any
// path-specific side effects (e.g. clipboard callbacks).
struct FixtureState {
    std::string text{"alpha"};
    std::string undo_text;
    std::string redo_text;
    std::string clipboard;
    std::string label{"fixture.txt"};
    std::size_t selection_count{1};
    std::uint32_t first_row{0};
    std::uint64_t follow_generation{0};
    ssg::FollowMode follow_mode{ssg::FollowMode::Following};
    ssg::DocumentMode mode{ssg::DocumentMode::Edit};
    ssg::TabKind tab_kind{ssg::TabKind::Document};
    ssg::TabRecoveryBadge recovery{ssg::TabRecoveryBadge::None};
    bool workspace_open{false};
    bool dirty{false};
    bool tab_open{true};
    bool word_wrap{false};
    bool prompt_open{true};

    CanonicalState canonical() const {
        return {ssg::Revision{0},
                text,       clipboard,      label,        selection_count,
                first_row,  mode,           tab_kind,     follow_mode,
                workspace_open, dirty,      tab_open,     word_wrap};
    }

    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload) {
        if (id == "workspace.open_directory") {
            workspace_open = true;
        } else if (id == "file.open") {
            tab_open = true;
            mode = ssg::DocumentMode::Edit;
        } else if (id == "text.insert") {
            if (mode != ssg::DocumentMode::Edit)
                return ssg::CommandHandlerResult::failure(
                    "document is not editable");
            undo_text = text;
            text += std::any_cast<ssg::TextInputArguments const&>(payload).text;
            dirty = true;
        } else if (id == "select.add_cursor_down") {
            selection_count = 2;
        } else if (id == "clipboard.copy") {
            clipboard = text;
        } else if (id == "clipboard.cut") {
            undo_text = text;
            clipboard = text;
            text.clear();
            dirty = true;
        } else if (id == "clipboard.paste") {
            undo_text = text;
            text += clipboard;
            dirty = true;
        } else if (id == "edit.undo") {
            redo_text = text;
            text = undo_text;
            dirty = true;
        } else if (id == "edit.redo") {
            undo_text = text;
            text = redo_text;
            dirty = true;
        } else if (id == "view.toggle_word_wrap") {
            word_wrap = !word_wrap;
        } else if (id == "view.scroll_lines") {
            auto rows =
                std::any_cast<ssg::ScrollLinesArguments const&>(payload).rows;
            first_row = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(first_row) + rows, 0, 80));
        } else if (id == "view.scroll_to_fraction") {
            auto const& f =
                std::any_cast<ssg::ScrollFractionArguments const&>(payload);
            first_row = static_cast<std::uint32_t>(
                80ULL * f.numerator / f.denominator);
        } else if (id == "file.save") {
            dirty = false;
        } else if (id == "tab.close") {
            tab_open = false;
            recovery = ssg::TabRecoveryBadge::Durable;
        } else if (id == "tab.reopen_closed") {
            tab_open = true;
        } else if (id == "file.reload") {
            mode = ssg::DocumentMode::ReadOnly;
        } else if (id == "external.open_diff") {
            mode = ssg::DocumentMode::Diff;
            tab_kind = ssg::TabKind::LiveDiff;
        } else if (id == "follow_edits.pause") {
            follow_mode = ssg::FollowMode::Paused;
            ++follow_generation;
        } else if (id == "follow_edits.resume") {
            follow_mode = ssg::FollowMode::Following;
            ++follow_generation;
        } else if (id == "follow_edits.toggle") {
            follow_mode = follow_mode == ssg::FollowMode::Following
                              ? ssg::FollowMode::Paused
                              : ssg::FollowMode::Following;
            ++follow_generation;
        } else if (id == "prompt.submit") {
            prompt_open = false;
        } else if (id == "file.open_dropped_content") {
            auto const& dropped =
                std::any_cast<ssg::DroppedContentArguments const&>(payload);
            text.assign(dropped.bytes.begin(), dropped.bytes.end());
            label = dropped.suggestedLabel;
            mode = ssg::DocumentMode::Edit;
            tab_kind = ssg::TabKind::Document;
            tab_open = true;
            dirty = true;
        }
        return ssg::CommandHandlerResult::success();
    }
};

struct WorkflowStep {
    std::string command_id;
    std::any payload;
    bool expected_accepted;
};

inline std::vector<WorkflowStep> load_workflow(char const* path) {
    std::ifstream f{path};
    if (!f)
        throw std::runtime_error{"cannot open workflow: " + std::string{path}};
    std::vector<WorkflowStep> steps;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> cols;
        std::size_t begin = 0;
        while (begin <= line.size()) {
            auto end = line.find('\t', begin);
            cols.push_back(line.substr(begin, end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (cols.size() != 4)
            throw std::runtime_error{"invalid workflow row: " + line};
        std::any args;
        if (cols[1] == "text") {
            args = ssg::TextInputArguments{cols[2]};
        } else if (cols[1] == "select_default") {
            args = ssg::SelectionCommandArguments{};
        } else if (cols[1].starts_with("lines:")) {
            args = ssg::ScrollLinesArguments{std::stoll(cols[1].substr(6))};
        } else if (cols[1].starts_with("fraction:")) {
            auto v = cols[1].substr(9);
            auto slash = v.find('/');
            if (slash == std::string::npos || slash == 0 ||
                slash + 1 == v.size())
                throw std::runtime_error{"invalid fraction argument: " + v};
            args = ssg::ScrollFractionArguments{
                static_cast<std::uint32_t>(std::stoul(v.substr(0, slash))),
                static_cast<std::uint32_t>(std::stoul(v.substr(slash + 1)))};
        } else if (cols[1].starts_with("dropped:")) {
            auto v = cols[1].substr(8);
            auto slash = v.find('/');
            if (slash == std::string::npos || slash + 1 == v.size())
                throw std::runtime_error{"invalid dropped argument: " + v};
            args = ssg::DroppedContentArguments{
                std::vector<std::uint8_t>{v.begin(), v.begin() + slash},
                v.substr(slash + 1)};
        } else if (cols[1] != "none") {
            throw std::runtime_error{"unknown argument kind: " + cols[1]};
        }
        steps.push_back(
            {std::move(cols[0]), std::move(args), cols[3] == "accepted"});
    }
    return steps;
}

} // namespace e2e
