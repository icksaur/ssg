#pragma once

#include <ssg/clipboard.h>
#include <ssg/diff.h>
#include <ssg/edit_commands.h>
#include <ssg/editor_runtime.h>
#include <ssg/editor_session_assembly.h>
#include <ssg/external_modification.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/follow_edits.h>
#include <ssg/history.h>
#include <ssg/input.h>
#include <ssg/lsp_features.h>
#include <ssg/lsp_workspace_edit.h>
#include <ssg/lua.h>
#include <ssg/prompt.h>
#include <ssg/search.h>
#include <ssg/settings.h>
#include <ssg/status.h>
#include <ssg/syntax.h>
#include <ssg/tabs.h>
#include <ssg/tree.h>
#include <ssg/ui_layout.h>
#include <ssg/workspace.h>

#include <any>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace ssg {

void bind_runtime_editing(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bind_runtime_files(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bind_runtime_presentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bind_runtime_navigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bind_runtime_language_services(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);

struct EditorRuntime::Impl final : CommandServices,
                                   TabLifecycle,
                                   SearchWorkspaceSource,
                                   SearchCommandSource,
                                   FindReplaceWorkspace,
                                   WorkspaceRecoverySink,
                                   LspWorkspaceEditDocuments,
                                   LspWorkspaceFileOperations {
    Impl(std::filesystem::path canonical_cwd,
         std::filesystem::path scratch_root,
         std::filesystem::path recovery_root);

    std::filesystem::path root;
    std::filesystem::path scratch_root;
    std::filesystem::path recovery_root;
    RecoveryActions recovery;
    ScratchStore scratch;
    Workspace workspace;
    SelectionViewState selection;
    std::map<std::uint64_t, DocumentHistory> histories;
    ClipboardRegister clipboard;
    SettingsModel settings;
    FindReplaceController find_replace;
    // The document the find/replace controller last evaluated against.  Find
    // matches are byte offsets into one specific document; when the active
    // document identity or revision drifts from this, the controller is stale and
    // must be dismissed (see reconcile_find_document).
    std::optional<FileDocumentId> find_document_id;
    PromptSurface prompt;
    StatusQueue status;
    ShellState shell;
    TabManager tabs;
    DiffModel diff;
    ExternalModificationFlow external;
    FollowEditsModel follow;
    TreeModel tree;
    SyntaxModel syntax;
    SearchController search;
    NavigationHistory navigation{64};
    LspSyncViewState lsp_sync;
    LspFeatureViewState lsp_features;
    KeymapViewState keymap{"default", {}};
    ThemeSnapshot theme{};
    std::optional<WorkspaceReplacePreview> workspace_replace_preview;
    std::unique_ptr<EditorSession> session;
    // Set by palette.execute after validating the selected candidate; the
    // EditorRuntime dispatch wrapper runs it through the registry once the
    // palette.execute transaction's session lock releases (the session mutex is
    // non-reentrant, so a handler cannot re-enter dispatch).
    std::optional<std::string> pending_palette_target;
    std::uint32_t requested_first_visual_row = 0;
    bool word_wrap = false;
    std::uint64_t next_status_id = 1;
    std::uint64_t next_tree_revision = 1;

    [[nodiscard]] CommandHandlerResult run_transaction(
        std::function<CommandHandlerResult()> operation) override;

    [[nodiscard]] std::any& feature_state_value(std::type_index type) override;
    void publish_status_value(std::type_index type, std::any status) override;
    void publish_delta_value(std::type_index type, std::any delta) override;

    [[nodiscard]] TabLifecycleResult close(
        const TabState& tab, std::chrono::milliseconds durability_timeout) override;
    [[nodiscard]] TabLifecycleResult reopen(
        const TabState& tab, const RecoveryRecordId& compensation) override;

    [[nodiscard]] WorkspaceSnapshot snapshot(Revision revision) const override;
    [[nodiscard]] std::vector<SearchCommandDescriptor> descriptors() const override;
    PaletteExecutionResult execute(std::string_view command_id) override;

    [[nodiscard]] WorkspaceApplyResult apply(
        const WorkspaceReplacePreview& preview,
        WorkspaceRecoverySink& recovery_sink) override;
    [[nodiscard]] WorkspaceApplyResult recover(
        const WorkspaceRecoveryRecord& record) override;
    bool store(const WorkspaceRecoveryRecord& record) override;

    [[nodiscard]] std::optional<LspDocumentSnapshot> snapshot(
        std::string_view uri) const override;
    [[nodiscard]] LspWorkspaceDocumentWriteResult apply(
        std::string uri, Revision expected_revision, std::string text) override;
    [[nodiscard]] LspWorkspaceFileResult snapshot(
        std::string_view uri, LspWorkspaceFileNode& node) const override;
    [[nodiscard]] LspWorkspaceFileResult create_file(
        std::string uri, bool overwrite) override;
    [[nodiscard]] LspWorkspaceFileResult write_file(
        std::string uri, std::string content) override;
    [[nodiscard]] LspWorkspaceFileResult rename_path(
        std::string old_uri, std::string new_uri, bool overwrite) override;
    [[nodiscard]] LspWorkspaceFileResult delete_path(
        std::string uri, bool recursive) override;
    [[nodiscard]] LspWorkspaceFileResult restore_path(
        std::string uri, const LspWorkspaceFileNode& node) override;

    [[nodiscard]] std::optional<FileDocumentId> active_document_id() const;
    [[nodiscard]] Document const* active_document() const;
    [[nodiscard]] Document* active_document();
    [[nodiscard]] DocumentHistory& history_for(FileDocumentId document);
    [[nodiscard]] std::optional<WorkspaceDocumentState> active_workspace_state() const;
    [[nodiscard]] std::string active_text() const;
    void reset_selection_for_active_document();
    void clamp_selection_to_active_document();
    [[nodiscard]] std::vector<CellRun> active_cell_runs() const;
    [[nodiscard]] ViewportViewState viewport(ViewportDimensions dimensions) const;
    [[nodiscard]] SessionSnapshotSections sections(ViewportDimensions dimensions,
                                                   KeySequence const& leader_pending = {},
                                                   PaletteReport const& palette_report = {}) const;
    [[nodiscard]] PromptStatusViewState prompt_status_view(ViewportDimensions dimensions) const;
    void project_find_replace_prompt(PromptViewState& prompt_view) const;
    // Dismiss the find/replace controller (and its prompt) when the active
    // document identity or revision no longer matches what it evaluated against,
    // so stale matches are never navigable or projected.
    void reconcile_find_document();
    [[nodiscard]] ShellViewState shell_view(ViewportDimensions dimensions,
                                            KeySequence const& leader_pending = {},
                                            PaletteReport const& palette_report = {}) const;
    [[nodiscard]] PaletteViewState palette_view() const;
    [[nodiscard]] TextEncodingViewState text_encoding_view() const;
    [[nodiscard]] DocumentViewState document_view() const;
    [[nodiscard]] std::string current_path_label() const;
    [[nodiscard]] CommandHandlerResult update_tabs_for(FileDocumentId document);
    [[nodiscard]] CommandHandlerResult activate_document(FileDocumentId document);
    void refresh_tree();
    void reconcile_prompt_focus();
    void refresh_syntax();
    void enqueue_status(StatusPriority priority, std::string text);
};

[[nodiscard]] CommandHandlerResult success();
[[nodiscard]] CommandHandlerResult failure(std::string message);
[[nodiscard]] std::string workspace_message(WorkspaceResult const& result);
[[nodiscard]] std::string tab_message(TabResult const& result);
[[nodiscard]] std::string wrong_payload(std::string_view command_id);

} // namespace ssg
