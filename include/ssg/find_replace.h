#pragma once

#include <ssg/document.h>
#include <ssg/history.h>
#include <ssg/search.h>
#include <ssg/selection.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct ByteRange {
    ByteOffset begin;
    ByteOffset end;
    bool operator==(const ByteRange&) const noexcept = default;
};

struct FindOptions {
    bool case_sensitive = false;
    bool whole_word = false;
    bool regex = false;
    bool selection_only = false;
    bool operator==(const FindOptions&) const noexcept = default;
};

struct FindMatch {
    ByteOffset begin;
    ByteOffset end;
    bool operator==(const FindMatch&) const noexcept = default;
};

enum class FindReplaceError : std::uint8_t {
    none,
    invalid_pattern,
    invalid_utf8,
    invalid_selection,
    budget_exhausted,
    cancelled,
    no_match,
    stale_revision,
    document_rejected,
    workspace_rejected,
    recovery_rejected,
};

struct FindRequest {
    std::string query;
    FindOptions options;
    std::optional<ByteRange> selection;
    std::uint64_t work_budget = 1'000'000;
    const std::atomic_bool* cancelled = nullptr;
};

struct FindResult {
    FindReplaceError error = FindReplaceError::none;
    std::vector<FindMatch> matches;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::none;
    }
};

[[nodiscard]] FindResult find_matches(std::string_view text,
                                      const FindRequest& request);

enum class FindReplaceCommand : std::uint8_t {
    find_open,
    find_close,
    find_next,
    find_previous,
    find_toggle_case,
    find_toggle_whole_word,
    find_toggle_regex,
    find_toggle_selection,
    replace_open,
    replace_current,
    replace_all,
    replace_workspace_preview,
    replace_workspace_apply,
};

struct FindReplaceCommandDescriptor {
    std::string_view id;
    FindReplaceCommand command;
    bool operator==(const FindReplaceCommandDescriptor&) const noexcept = default;
};

class FindReplaceCommandSet {
public:
    [[nodiscard]] const std::array<FindReplaceCommandDescriptor, 13>&
    descriptors() const noexcept;

private:
    friend FindReplaceCommandSet find_replace_command_set();
    FindReplaceCommandSet();
    const std::array<FindReplaceCommandDescriptor, 13> descriptors_;
};

[[nodiscard]] FindReplaceCommandSet find_replace_command_set();

struct FindReplaceViewState {
    std::uint64_t generation = 0;
    bool open = false;
    bool replace_mode = false;
    Revision source_revision{0};
    std::string query;
    FindOptions options;
    std::vector<FindMatch> matches;
    std::optional<std::size_t> active_match;
    FindReplaceError error = FindReplaceError::none;
    std::string message;
    bool operator==(const FindReplaceViewState&) const = default;
};

struct FindReplaceDelta {
    bool changed = false;
    std::uint64_t base_generation = 0;
    std::optional<FindReplaceViewState> replacement;
    bool operator==(const FindReplaceDelta&) const = default;
};

enum class FindReplaceReplayError : std::uint8_t {
    none,
    base_mismatch,
    malformed_delta,
};

struct FindReplaceReplayResult {
    FindReplaceReplayError error = FindReplaceReplayError::none;
    FindReplaceViewState state;
};

[[nodiscard]] FindReplaceDelta derive_find_replace_delta(
    const FindReplaceViewState& before, const FindReplaceViewState& after);
[[nodiscard]] FindReplaceReplayResult replay_find_replace_delta(
    const FindReplaceViewState& base, const FindReplaceDelta& delta);

struct FindReplaceOperationResult {
    FindReplaceError error = FindReplaceError::none;
    Revision revision{0};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::none;
    }
};

class FindReplaceController {
public:
    void open(const DocumentSnapshot& document, FindRequest request);
    void open_replace(const DocumentSnapshot& document, FindRequest request);
    void close();
    void update_query(const DocumentSnapshot& document, std::string query,
                      std::optional<ByteRange> selection);
    void toggle_case(const DocumentSnapshot& document);
    void toggle_whole_word(const DocumentSnapshot& document);
    void toggle_regex(const DocumentSnapshot& document);
    void toggle_selection(const DocumentSnapshot& document,
                          std::optional<ByteRange> selection);
    void refresh(const DocumentSnapshot& document,
                 std::optional<ByteRange> selection);
    void next();
    void previous();

    [[nodiscard]] FindReplaceOperationResult replace_current(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections_before,
        const SelectionSet& selections_after, std::string replacement,
        std::uint64_t timestamp_ms);
    [[nodiscard]] FindReplaceOperationResult replace_all(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections_before,
        const SelectionSet& selections_after, std::string replacement,
        std::uint64_t timestamp_ms);

    [[nodiscard]] const FindReplaceViewState& view_state() const noexcept;

private:
    void evaluate(const DocumentSnapshot& document);
    FindRequest request_;
    FindReplaceViewState state_;
};

struct WorkspaceFileReplacement {
    std::string path;
    std::string before;
    std::string after;
    std::vector<FindMatch> matches;
    bool operator==(const WorkspaceFileReplacement&) const = default;
};

struct WorkspaceReplacePreview {
    Revision source_revision{0};
    std::string query;
    std::string replacement;
    FindOptions options;
    std::vector<WorkspaceFileReplacement> changes;
    bool operator==(const WorkspaceReplacePreview&) const = default;
};

struct WorkspaceRecoveryRecord {
    Revision source_revision{0};
    Revision applied_revision{0};
    std::vector<WorkspaceFileReplacement> changes;
    bool operator==(const WorkspaceRecoveryRecord&) const = default;
};

class WorkspaceRecoverySink {
public:
    virtual ~WorkspaceRecoverySink() = default;
    virtual bool store(const WorkspaceRecoveryRecord& record) = 0;
};

struct WorkspaceApplyResult {
    FindReplaceError error = FindReplaceError::none;
    Revision revision{0};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::none;
    }
};

class FindReplaceWorkspace {
public:
    virtual ~FindReplaceWorkspace() = default;
    [[nodiscard]] virtual WorkspaceSnapshot snapshot(
        Revision revision) const = 0;
    [[nodiscard]] virtual WorkspaceApplyResult apply(
        const WorkspaceReplacePreview& preview,
        WorkspaceRecoverySink& recovery_sink) = 0;
    [[nodiscard]] virtual WorkspaceApplyResult recover(
        const WorkspaceRecoveryRecord& record) = 0;
};

struct WorkspacePreviewResult {
    FindReplaceError error = FindReplaceError::none;
    std::optional<WorkspaceReplacePreview> preview;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::none;
    }
};

[[nodiscard]] WorkspacePreviewResult preview_workspace_replace(
    const FindReplaceWorkspace& workspace, Revision source_revision,
    const FindRequest& request, std::string replacement);
[[nodiscard]] WorkspaceApplyResult apply_workspace_replace(
    FindReplaceWorkspace& workspace, const WorkspaceReplacePreview& preview,
    WorkspaceRecoverySink& recovery_sink);
[[nodiscard]] WorkspaceApplyResult recover_workspace_replace(
    FindReplaceWorkspace& workspace, const WorkspaceRecoveryRecord& record);

}  // namespace ssg
