#include "runtime/editor_runtime_internal.h"

#include <ssg/layout.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace ssg {
namespace {

ThemeSnapshot default_theme() {
    // Readable dark theme derived from the VSCode-style palette in
    // caco/public/themes/dark.css.  Low indices are dark fills, high indices
    // are light text, hues sit in the middle.  Role assignments keep every
    // co_visible_role_pairs member on a distinct palette index.
    ThemeSnapshot snapshot{};
    constexpr std::array<std::array<std::uint8_t, 3>, theme_palette_size>
        palette{{
            {30, 30, 30},     // 0  background
            {212, 212, 212},  // 1  foreground
            {62, 62, 66},     // 2  chrome fill / scrollbar track
            {133, 133, 133},  // 3  muted: line numbers, comments, inactive
            {77, 170, 252},   // 4  blue: functions, focus, active
            {229, 192, 123},  // 5  yellow: operators, warnings
            {239, 74, 74},    // 6  red: errors, deletions
            {76, 175, 80},    // 7  green: strings, additions
            {171, 71, 188},   // 8  purple: keywords, prompt, hints
            {38, 192, 192},   // 9  cyan: types, info
            {212, 149, 106},  // 10 orange: numbers, modifications, search
            {209, 109, 158},  // 11 pink: conflicts
            {187, 187, 187},  // 12 chrome text: header, footer
            {106, 106, 106},  // 13 dim: inactive tab, scrollbar thumb
            {232, 232, 232},  // 14 bright: active line number, punctuation
            {255, 255, 255},  // 15 caret
        }};
    for (std::size_t index = 0; index < snapshot.palette.size(); ++index) {
        snapshot.palette[index] = SrgbColor::from_serialized_channels(
            palette[index][0], palette[index][1], palette[index][2]);
    }

    auto role = [&](SemanticRole which, std::uint8_t index) {
        snapshot.semantic_indices[static_cast<std::size_t>(which)] = index;
    };
    role(SemanticRole::foreground, 1);
    role(SemanticRole::background, 0);
    role(SemanticRole::caret, 15);
    role(SemanticRole::selection, 4);
    role(SemanticRole::diagnostic_error, 6);
    role(SemanticRole::diagnostic_warning, 5);
    role(SemanticRole::diagnostic_info, 9);
    role(SemanticRole::diagnostic_hint, 8);
    role(SemanticRole::git_added, 7);
    role(SemanticRole::git_modified, 10);
    role(SemanticRole::git_deleted, 6);
    role(SemanticRole::git_conflict, 11);
    role(SemanticRole::tree_background, 2);
    role(SemanticRole::tree_focus, 4);
    role(SemanticRole::tab_active, 4);
    role(SemanticRole::tab_inactive, 13);
    role(SemanticRole::panel_active, 9);
    role(SemanticRole::panel_inactive, 3);
    role(SemanticRole::header, 12);
    role(SemanticRole::footer, 12);
    role(SemanticRole::status_info, 9);
    role(SemanticRole::status_warning, 5);
    role(SemanticRole::status_error, 6);
    role(SemanticRole::line_number, 3);
    role(SemanticRole::active_line_number, 14);
    role(SemanticRole::search_match, 10);
    role(SemanticRole::prompt, 8);
    role(SemanticRole::scrollbar_track, 2);
    role(SemanticRole::scrollbar_thumb, 13);
    role(SemanticRole::diff_added, 7);
    role(SemanticRole::diff_removed, 6);
    role(SemanticRole::diff_modified, 10);

    auto syntax = [&](SyntaxScope scope, std::uint8_t index) {
        snapshot.syntax_indices[static_cast<std::size_t>(scope)] = index;
    };
    syntax(SyntaxScope::plain_text, 1);
    syntax(SyntaxScope::comment, 3);
    syntax(SyntaxScope::keyword, 8);
    syntax(SyntaxScope::string, 7);
    syntax(SyntaxScope::number, 10);
    syntax(SyntaxScope::type, 9);
    syntax(SyntaxScope::function, 4);
    syntax(SyntaxScope::variable, 1);
    syntax(SyntaxScope::operator_token, 5);
    syntax(SyntaxScope::punctuation, 14);
    syntax(SyntaxScope::invalid, 6);
    return snapshot;
}

// The curated terminal runtime keymap (doc/spec-keymap.md K2): a small set of
// argument-free bindings the TUI drives, plus the context-divergent navigation
// keys.  Only argument-free-usable commands are bound (a bare chord dispatches
// with no payload); exhaustive reachability is the palette's job.  Global (*)
// chords are Escape-led and prefix-free; single strokes differ per focus.
KeymapViewState default_terminal_keymap() {
    auto seq = [](std::initializer_list<std::string_view> strokes) {
        auto parsed = parse_key_sequence(strokes);
        if (!parsed) throw std::logic_error{"curated keymap has an invalid stroke"};
        return *parsed;
    };
    KeymapViewState keymap{"default", {}};
    auto bind = [&](KeySequence sequence, std::string command,
                    std::string context) {
        keymap.bindings.push_back(
            {std::move(sequence), std::move(command), std::move(context)});
    };

    bind(seq({"Escape", "KeyS"}), "file.save", "*");
    bind(seq({"Escape", "KeyZ"}), "edit.undo", "*");
    bind(seq({"Escape", "Shift+KeyZ"}), "edit.redo", "*");
    bind(seq({"Escape", "KeyP"}), "palette.open", "*");
    bind(seq({"Escape", "KeyB"}), "panel.toggle", "*");
    bind(seq({"Escape", "KeyO"}), "panel.focus", "*");
    bind(seq({"Escape", "BracketRight"}), "tab.next", "*");
    bind(seq({"Escape", "BracketLeft"}), "tab.previous", "*");
    bind(seq({"Escape", "KeyW"}), "tab.close", "*");
    bind(seq({"Escape", "KeyF", "KeyT"}), "settings.open", "*");
    bind(seq({"Escape", "KeyA"}), "select.all", "*");
    bind(seq({"Escape", "KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Escape", "KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Escape", "KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Escape", "KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Escape", "Slash"}), "find.open", "*");
    bind(seq({"Escape", "KeyR"}), "replace.open", "*");

    bind(seq({"ArrowDown"}), "cursor.line_down", "editor");
    bind(seq({"ArrowUp"}), "cursor.line_up", "editor");
    bind(seq({"ArrowLeft"}), "cursor.left", "editor");
    bind(seq({"ArrowRight"}), "cursor.right", "editor");
    bind(seq({"Shift+ArrowLeft"}), "select.left", "editor");
    bind(seq({"Shift+ArrowRight"}), "select.right", "editor");
    bind(seq({"Shift+ArrowUp"}), "select.line_up", "editor");
    bind(seq({"Shift+ArrowDown"}), "select.line_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    bind(seq({"Escape", "Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "palette.next", "prompt");
    bind(seq({"ArrowUp"}), "palette.previous", "prompt");

    return keymap;
}

DocumentPosition zero_position() {
    return {ByteOffset{0}, LineIndex{0}, CellIndex{0}};
}

SelectionViewState initial_selection() {
    auto zero = zero_position();
    return {SelectionSet{std::vector<Selection>{Selection{zero, zero}}}, 0, std::nullopt};
}

std::filesystem::path canonical_directory(std::filesystem::path const& path) {
    std::error_code code;
    auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        throw std::invalid_argument{"workspace root must be an existing directory"};
    }
    return canonical;
}

std::string read_file_text(std::filesystem::path const& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<std::filesystem::path> path_from_uri(std::string_view uri) {
    constexpr std::string_view prefix{"file://"};
    if (uri.rfind(prefix, 0) != 0) return std::nullopt;
    return std::filesystem::path{std::string{uri.substr(prefix.size())}};
}

std::string uri_from_path(std::filesystem::path const& path) {
    return "file://" + path.generic_string();
}

std::optional<std::string> relative_to_root(std::filesystem::path const& root,
                                            std::filesystem::path const& path) {
    auto relative = path.lexically_relative(root);
    if (relative.empty()) return std::nullopt;
    for (auto const& part : relative) {
        if (part == "..") return std::nullopt;
    }
    return relative.generic_string();
}

bool path_contains(std::filesystem::path const& root,
                   std::filesystem::path const& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> workspace_change_path(
    std::filesystem::path const& root, std::string_view raw_path,
    std::string& message) {
    auto supplied = std::filesystem::path{raw_path};
    if (raw_path.empty() || supplied.is_absolute()) {
        message = "workspace replacement path must be relative";
        return std::nullopt;
    }
    for (auto const& part : supplied) {
        if (part == "..") {
            message = "workspace replacement path must not traverse";
            return std::nullopt;
        }
    }
    std::error_code code;
    auto candidate = std::filesystem::weakly_canonical(root / supplied, code);
    if (code) {
        message = code.message();
        return std::nullopt;
    }
    if (!path_contains(root, candidate)) {
        message = "workspace replacement path resolves outside the workspace";
        return std::nullopt;
    }
    return candidate;
}

} // namespace

CommandHandlerResult success() { return CommandHandlerResult::success(); }
CommandHandlerResult failure(std::string message) {
    return CommandHandlerResult::failure(std::move(message));
}

std::string wrong_payload(std::string_view command_id) {
    return std::string{command_id} + " payload has the wrong type";
}

std::string workspace_message(WorkspaceResult const& result) {
    return result.message.empty() ? "workspace operation failed" : result.message;
}

std::string tab_message(TabResult const& result) {
    return result.message.empty() ? "tab operation failed" : result.message;
}

EditorRuntime::Impl::Impl(std::filesystem::path canonical_cwd,
                          std::filesystem::path scratch_root,
                          std::filesystem::path recovery_root)
    : root{std::move(canonical_cwd)},
      scratch_root{std::filesystem::weakly_canonical(scratch_root)},
      recovery_root{std::filesystem::weakly_canonical(recovery_root)},
      recovery{RecoveryActions::create(recovery_root)},
      scratch{ScratchStore::create(scratch_root, root)},
      workspace{Workspace::create(root, recovery)},
      selection{initial_selection()},
      clipboard{4},
      shell{{"Files", "Git", "Symbols"}},
      tabs{*this},
      external{recovery, diff},
      syntax{},
      search{*this, *this},
      theme{default_theme()} {
    refresh_tree();
    refresh_syntax();
}

CommandHandlerResult EditorRuntime::Impl::run_transaction(
    std::function<CommandHandlerResult()> operation) {
    return operation();
}

std::any& EditorRuntime::Impl::feature_state_value(std::type_index) {
    throw std::logic_error{"EditorRuntime exposes feature state through snapshots"};
}

void EditorRuntime::Impl::publish_status_value(std::type_index, std::any status_value) {
    if (auto const* item = std::any_cast<StatusItem>(&status_value)) {
        (void)status.enqueue(*item);
    }
}

void EditorRuntime::Impl::publish_delta_value(std::type_index, std::any) {}

TabLifecycleResult EditorRuntime::Impl::close(
    const TabState& tab, std::chrono::milliseconds durability_timeout) {
    if (!tab.document) return {};
    auto state = workspace.state(*tab.document);
    if (!state) return {TabError::not_found, "tab document does not exist", std::nullopt, false};
    std::optional<JournalDocument> document;
    if (auto const* current = active_document(); current != nullptr) {
        document = JournalDocument{state->key, current->mode(), state->dirty,
                                   current->snapshot().text};
    }
    auto closed = recovery.close_document(document, scratch, durability_timeout);
    if (!closed.accepted()) {
        return {TabError::lifecycle_failed, closed.error->message, std::nullopt, false};
    }
    if (document) scratch.remove_document(state->key);
    return {TabError::none, {}, closed.compensation, scratch.wait_until_durable(durability_timeout)};
}

TabLifecycleResult EditorRuntime::Impl::reopen(
    const TabState&, const RecoveryRecordId& compensation) {
    auto restored = workspace.restore(compensation);
    if (!restored.accepted()) {
        return {TabError::lifecycle_failed, workspace_message(restored), std::nullopt, false};
    }
    return {};
}

WorkspaceSnapshot EditorRuntime::Impl::snapshot(Revision revision) const {
    WorkspaceSnapshot result;
    result.revision = revision;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::saved) continue;
        result.files.push_back({state->key.saved_path(), workspace.document(id).snapshot().text});
    }
    std::filesystem::recursive_directory_iterator it{root};
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
        auto const& entry = *it;
        if (entry.is_directory() &&
            (path_contains(scratch_root, entry.path()) ||
             path_contains(recovery_root, entry.path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file()) continue;
        auto relative = relative_to_root(root, entry.path());
        if (!relative) continue;
        if (std::find_if(result.files.begin(), result.files.end(), [&](WorkspaceFile const& file) {
                return file.path == *relative;
            }) != result.files.end()) {
            continue;
        }
        result.files.push_back({*relative, read_file_text(entry.path())});
    }
    return result;
}

std::vector<SearchCommandDescriptor> EditorRuntime::Impl::descriptors() const {
    std::vector<SearchCommandDescriptor> result;
    for (auto const& descriptor : p0_command_descriptors()) {
        result.push_back({descriptor.id, descriptor.id});
    }
    return result;
}

PaletteExecutionResult EditorRuntime::Impl::execute(std::string_view command_id) {
    auto descriptors = p0_command_descriptors();
    return {std::find_if(descriptors.begin(), descriptors.end(),
                         [&](CommandDescriptor const& descriptor) { return descriptor.id == command_id; }) !=
                descriptors.end(),
            {}};
}

WorkspaceApplyResult EditorRuntime::Impl::apply(
    const WorkspaceReplacePreview& preview, WorkspaceRecoverySink& recovery_sink) {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> normalized_paths;
    paths.reserve(preview.changes.size());
    normalized_paths.reserve(preview.changes.size());
    for (auto const& change : preview.changes) {
        std::string message;
        auto path = workspace_change_path(root, change.path, message);
        if (!path) {
            return {FindReplaceError::workspace_rejected,
                    preview.source_revision, std::move(message)};
        }
        if (path_contains(scratch_root, *path) ||
            path_contains(recovery_root, *path)) {
            return {FindReplaceError::workspace_rejected,
                    preview.source_revision,
                    "workspace replacement path targets runtime state"};
        }
        auto normalized =
            std::filesystem::path{change.path}.lexically_normal().generic_string();
        std::string current;
        bool found_open_document = false;
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::saved ||
                state->key.saved_path() != normalized) {
                continue;
            }
            current = workspace.document(id).snapshot().text;
            found_open_document = true;
            break;
        }
        if (!found_open_document) {
            current = read_file_text(*path);
        }
        if (current != change.before) {
            return {FindReplaceError::stale_revision, preview.source_revision,
                    "workspace replacement preview is stale"};
        }
        paths.push_back(std::move(*path));
        normalized_paths.push_back(std::move(normalized));
    }
    WorkspaceRecoveryRecord record{preview.source_revision, Revision{preview.source_revision.value() + 1}, preview.changes};
    if (!recovery_sink.store(record)) {
        return {FindReplaceError::recovery_rejected, preview.source_revision, "workspace replacement recovery rejected"};
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        std::ofstream output{paths[index], std::ios::binary | std::ios::trunc};
        if (!output) return {FindReplaceError::workspace_rejected, preview.source_revision, "failed to write workspace file"};
        output << change.after;
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::saved ||
                state->key.saved_path() != normalized_paths[index]) {
                continue;
            }
            auto reloaded = workspace.reload(id);
            if (!reloaded.accepted()) {
                return {FindReplaceError::workspace_rejected,
                        preview.source_revision, workspace_message(reloaded)};
            }
            (void)update_tabs_for(id);
        }
    }
    return {FindReplaceError::none, record.applied_revision, {}};
}

WorkspaceApplyResult EditorRuntime::Impl::recover(const WorkspaceRecoveryRecord& record) {
    for (auto const& change : record.changes) {
        std::ofstream output{root / change.path, std::ios::binary | std::ios::trunc};
        if (!output) return {FindReplaceError::workspace_rejected, record.applied_revision, "failed to recover workspace file"};
        output << change.before;
    }
    return {FindReplaceError::none, record.applied_revision, {}};
}

bool EditorRuntime::Impl::store(const WorkspaceRecoveryRecord&) { return true; }

std::optional<LspDocumentSnapshot> EditorRuntime::Impl::snapshot(std::string_view uri) const {
    auto path = path_from_uri(uri);
    if (!path) return std::nullopt;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::saved) continue;
        if (uri_from_path(root / state->key.saved_path()) == uri) {
            return LspDocumentSnapshot{std::string{uri}, workspace.document(id).revision(), 1,
                                       workspace.document(id).snapshot().text};
        }
    }
    return std::nullopt;
}

LspWorkspaceDocumentWriteResult EditorRuntime::Impl::apply(
    std::string uri, Revision expected_revision, std::string text) {
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::saved) continue;
        if (uri_from_path(root / state->key.saved_path()) != uri) continue;
        auto& document = const_cast<Document&>(workspace.document(id));
        if (document.revision() != expected_revision) {
            return {document.revision(), LspWorkspaceDocumentError::stale_revision, "document revision is stale"};
        }
        auto snapshot = document.snapshot();
        auto result = document.apply({snapshot.revision, {{ByteOffset{0}, snapshot.text.size(), std::move(text)}}});
        if (!result.accepted()) return {document.revision(), LspWorkspaceDocumentError::write_failed, result.message};
        return {result.revision, LspWorkspaceDocumentError::none, {}};
    }
    return {Revision{0}, LspWorkspaceDocumentError::unknown_document, "document URI is not open"};
}

LspWorkspaceFileResult EditorRuntime::Impl::snapshot(std::string_view uri, LspWorkspaceFileNode& node) const {
    auto path = path_from_uri(uri);
    if (!path) return {LspWorkspaceFileError::not_found, "URI is not a file URI"};
    if (!std::filesystem::exists(*path)) {
        node.kind = LspWorkspaceFileNodeKind::missing;
    } else if (std::filesystem::is_directory(*path)) {
        node.kind = LspWorkspaceFileNodeKind::directory;
    } else {
        node.kind = LspWorkspaceFileNodeKind::file;
        node.content = read_file_text(*path);
    }
    return {};
}

LspWorkspaceFileResult EditorRuntime::Impl::create_file(std::string uri, bool overwrite) {
    auto path = path_from_uri(uri);
    if (!path) return {LspWorkspaceFileError::io_error, "URI is not a file URI"};
    if (std::filesystem::exists(*path) && !overwrite) return {LspWorkspaceFileError::already_exists, "file already exists"};
    std::ofstream output{*path, std::ios::binary | std::ios::trunc};
    return output ? LspWorkspaceFileResult{} : LspWorkspaceFileResult{LspWorkspaceFileError::io_error, "failed to create file"};
}

LspWorkspaceFileResult EditorRuntime::Impl::write_file(std::string uri, std::string content) {
    auto path = path_from_uri(uri);
    if (!path) return {LspWorkspaceFileError::io_error, "URI is not a file URI"};
    std::ofstream output{*path, std::ios::binary | std::ios::trunc};
    if (!output) return {LspWorkspaceFileError::io_error, "failed to write file"};
    output << content;
    return {};
}

LspWorkspaceFileResult EditorRuntime::Impl::rename_path(std::string old_uri, std::string new_uri, bool overwrite) {
    auto old_path = path_from_uri(old_uri);
    auto new_path = path_from_uri(new_uri);
    if (!old_path || !new_path) return {LspWorkspaceFileError::io_error, "URI is not a file URI"};
    if (std::filesystem::exists(*new_path) && !overwrite) return {LspWorkspaceFileError::already_exists, "destination exists"};
    std::error_code code;
    std::filesystem::rename(*old_path, *new_path, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::io_error, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorRuntime::Impl::delete_path(std::string uri, bool recursive) {
    auto path = path_from_uri(uri);
    if (!path) return {LspWorkspaceFileError::io_error, "URI is not a file URI"};
    std::error_code code;
    if (recursive) std::filesystem::remove_all(*path, code);
    else std::filesystem::remove(*path, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::io_error, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorRuntime::Impl::restore_path(std::string uri, const LspWorkspaceFileNode& node) {
    auto path = path_from_uri(uri);
    if (!path) return {LspWorkspaceFileError::io_error, "URI is not a file URI"};
    if (node.kind == LspWorkspaceFileNodeKind::missing) {
        std::error_code code;
        std::filesystem::remove_all(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::io_error, code.message()} : LspWorkspaceFileResult{};
    }
    if (node.kind == LspWorkspaceFileNodeKind::directory) {
        std::error_code code;
        std::filesystem::create_directories(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::io_error, code.message()} : LspWorkspaceFileResult{};
    }
    return write_file(std::move(uri), node.content);
}

std::optional<FileDocumentId> EditorRuntime::Impl::active_document_id() const {
    // The active tab is the single source of truth for the active editor
    // document.  With no active tab (e.g. the last tab was closed) there is no
    // active document and the shell renders its empty state; the editor view
    // never shows a document that has no tab.
    auto const& view = tabs.view_state();
    if (!view.active) return std::nullopt;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(), [&](TabState const& tab) {
        return tab.id == *view.active;
    });
    if (found != view.tabs.end()) return found->document;
    return std::nullopt;
}

Document const* EditorRuntime::Impl::active_document() const {
    auto id = active_document_id();
    return id ? &workspace.document(*id) : nullptr;
}

Document* EditorRuntime::Impl::active_document() {
    auto id = active_document_id();
    return id ? const_cast<Document*>(&workspace.document(*id)) : nullptr;
}

DocumentHistory& EditorRuntime::Impl::history_for(FileDocumentId document) {
    auto [it, inserted] = histories.try_emplace(document.value(), HistoryConfig::defaults());
    return it->second;
}

std::optional<WorkspaceDocumentState> EditorRuntime::Impl::active_workspace_state() const {
    auto id = active_document_id();
    return id ? workspace.state(*id) : std::nullopt;
}

std::string EditorRuntime::Impl::active_text() const {
    auto const* document = active_document();
    return document ? document->snapshot().text : std::string{};
}

void EditorRuntime::Impl::reset_selection_for_active_document() {
    selection = initial_selection();
    requested_first_visual_row = 0;
}

void EditorRuntime::Impl::clamp_selection_to_active_document() {
    auto text = active_text();
    auto offset = selection.selections.primary().active.byte_offset.value();
    if (offset > text.size()) offset = text.size();
    auto position = resolve_document_position(text, ByteOffset{offset}).value_or(zero_position());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

std::vector<CellRun> EditorRuntime::Impl::active_cell_runs() const {
    std::vector<CellRun> runs;
    std::string const text = active_text();
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? end : end - start);
        runs.push_back(compute_cell_run(line, 4));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (runs.empty()) runs.push_back(compute_cell_run("", 4));
    return runs;
}

ViewportViewState EditorRuntime::Impl::viewport(ViewportDimensions dimensions) const {
    auto runs = active_cell_runs();
    return compute_viewport(runs, dimensions, requested_first_visual_row);
}

void EditorRuntime::Impl::refresh_tree() {
    tree.replace_provider(filesystem_tree_snapshot(
        TreeProviderId{"filesystem"}, root, TreeRevision{next_tree_revision++}));
}

void EditorRuntime::Impl::reconcile_prompt_focus() {
    if (prompt.active() && shell.focus() != FocusTarget::prompt) {
        shell.enter_prompt_focus();
    } else if (!prompt.active() && shell.focus() == FocusTarget::prompt) {
        shell.exit_prompt_focus();
    }
}

void EditorRuntime::Impl::refresh_syntax() {
    auto const* document = active_document();
    auto text = document ? document->snapshot().text : std::string{};
    auto revision = document ? document->revision() : Revision{0};
    auto request = syntax.request(revision, LanguageId::plain_text(), std::move(text));
    if (request.accepted()) {
        auto output = syntax.run(*request.request);
        (void)syntax.accept(request.request, output);
    }
}

void EditorRuntime::Impl::enqueue_status(StatusPriority priority, std::string text) {
    auto value = next_status_id++;
    (void)status.enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}});
}

EditorRuntime::EditorRuntime(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}
EditorRuntime::~EditorRuntime() = default;

EditorRuntimeCreateResult EditorRuntime::create(EditorRuntimeConfig config) {
    try {
        auto cwd = canonical_directory(config.cwd);
        if (config.scratch_root.empty()) config.scratch_root = cwd / ".ssg" / "scratch";
        if (config.recovery_root.empty()) config.recovery_root = cwd / ".ssg" / "recovery";
        std::filesystem::create_directories(config.scratch_root);
        std::filesystem::create_directories(config.recovery_root);
        auto impl = std::make_unique<Impl>(cwd, config.scratch_root, config.recovery_root);
        impl->keymap = default_terminal_keymap();
        if (auto errors = validate_keymap(impl->keymap, {}); !errors.empty()) {
            return {nullptr, "default keymap is invalid: " + errors.front().message};
        }
        if (!has_global_binding(impl->keymap, "settings.open", {})) {
            return {nullptr,
                    "default keymap lacks a global settings.open escape hatch"};
        }
        EditorSessionBuilder builder;
        builder.services(*impl);
        bind_runtime_editing(builder, *impl);
        bind_runtime_files(builder, *impl);
        bind_runtime_presentation(builder, *impl);
        bind_runtime_navigation(builder, *impl);
        bind_runtime_language_services(builder, *impl);
        impl->session = builder.build();
        return {std::unique_ptr<EditorRuntime>{new EditorRuntime{std::move(impl)}}, {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

AttachResult EditorRuntime::attach(InvocationPrincipal principal, ViewId view_id) {
    auto client_id = principal.client_id();
    auto result = impl_->session->attach(std::move(principal), view_id);
    if (result.accepted()) {
        (void)impl_->follow.attach_client(client_id, ViewportDimensions{80, 24});
    }
    return result;
}

bool EditorRuntime::detach(ClientId client_id) {
    (void)impl_->follow.detach_client(client_id);
    return impl_->session->detach(client_id);
}

CommandResult EditorRuntime::dispatch(ClientId client_id, ClientCommand const& command) {
    auto result = impl_->session->dispatch(client_id, command);
    impl_->reconcile_prompt_focus();
    // palette.execute validates the selected candidate then defers execution to
    // here so the target runs through the registry (with its own capability and
    // revision checks) outside the non-reentrant session lock.
    if (result.accepted() && impl_->pending_palette_target) {
        auto target = std::move(*impl_->pending_palette_target);
        impl_->pending_palette_target.reset();
        auto target_result = impl_->session->dispatch(
            client_id, {target, impl_->session->revision(), {}});
        impl_->reconcile_prompt_focus();
        return target_result;
    }
    return result;
}

Revision EditorRuntime::revision() const { return impl_->session->revision(); }
std::filesystem::path const& EditorRuntime::workspace_root() const noexcept { return impl_->root; }
std::optional<SessionSnapshot> EditorRuntime::snapshot(ClientId client_id, ViewportDimensions dimensions,
                                                       KeySequence leader_pending,
                                                       PaletteReport palette_report) const {
    auto client = impl_->session->attached_client(client_id);
    if (!client) return std::nullopt;
    return assemble_session_snapshot(impl_->session->revision(), impl_->session->topology(),
                                     client->principal, client->view_id,
                                     impl_->viewport(dimensions),
                                     impl_->sections(dimensions, leader_pending, palette_report));
}

std::string EditorRuntime::active_document_text() const { return impl_->active_text(); }

} // namespace ssg
