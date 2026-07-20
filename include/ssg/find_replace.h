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
    None,
    InvalidPattern,
    InvalidUtf8,
    InvalidSelection,
    BudgetExhausted,
    Cancelled,
    NoMatch,
    StaleRevision,
    DocumentRejected,
    WorkspaceRejected,
    RecoveryRejected,
};

struct FindRequest {
    std::string query;
    FindOptions options;
    std::optional<ByteRange> selection;
    std::uint64_t work_budget = 1'000'000;
    const std::atomic_bool* cancelled = nullptr;
};

struct FindResult {
    FindReplaceError error = FindReplaceError::None;
    std::vector<FindMatch> matches;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::None;
    }
};

[[nodiscard]] FindResult findMatches(std::string_view text,
                                      const FindRequest& request);

enum class FindReplaceCommand : std::uint8_t {
    FindOpen,
    FindClose,
    FindNext,
    FindPrevious,
    FindUpdateQuery,
    FindToggleCase,
    FindToggleWholeWord,
    FindToggleRegex,
    FindToggleSelection,
    ReplaceOpen,
    ReplaceUpdateReplacement,
    ReplaceCurrent,
    ReplaceAll,
    ReplaceWorkspacePreview,
    ReplaceWorkspaceApply,
};

// The typed argument for find.update_query / replace.update_replacement: the full
// query (or replacement) text.  Carried as a command argument so the query lives
// in the controller (server-authoritative), edited by the client which reports
// the next full string (see doc/spec-m7.md F1).
struct FindQueryArguments {
    std::string query;
    friend bool operator==(const FindQueryArguments&, const FindQueryArguments&) = default;
};

struct FindReplaceCommandDescriptor {
    std::string_view id;
    FindReplaceCommand command;
    bool operator==(const FindReplaceCommandDescriptor&) const noexcept = default;
};

class FindReplaceCommandSet {
public:
    [[nodiscard]] const std::array<FindReplaceCommandDescriptor, 15>&
    descriptors() const noexcept;

private:
    friend FindReplaceCommandSet findReplaceCommandSet();
    FindReplaceCommandSet();
    const std::array<FindReplaceCommandDescriptor, 15> descriptors_;
};

[[nodiscard]] FindReplaceCommandSet findReplaceCommandSet();

struct FindReplaceViewState {
    std::uint64_t generation = 0;
    bool open = false;
    bool replace_mode = false;
    Revision source_revision{0};
    std::string query;
    std::string replacement;
    FindOptions options;
    std::vector<FindMatch> matches;
    std::optional<std::size_t> active_match;
    FindReplaceError error = FindReplaceError::None;
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
    None,
    BaseMismatch,
    MalformedDelta,
};

struct FindReplaceReplayResult {
    FindReplaceReplayError error = FindReplaceReplayError::None;
    FindReplaceViewState state;
};

[[nodiscard]] FindReplaceDelta deriveFindReplaceDelta(
    const FindReplaceViewState& before, const FindReplaceViewState& after);
[[nodiscard]] FindReplaceReplayResult replayFindReplaceDelta(
    const FindReplaceViewState& base, const FindReplaceDelta& delta);

struct FindReplaceOperationResult {
    FindReplaceError error = FindReplaceError::None;
    Revision revision{0};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::None;
    }
};

class FindReplaceController {
public:
    void open(const DocumentSnapshot& document, FindRequest request);
    void openReplace(const DocumentSnapshot& document, FindRequest request);
    void close();
    void updateQuery(const DocumentSnapshot& document, std::string query,
                      std::optional<ByteRange> selection);
    void updateReplacement(std::string replacement);
    void toggleCase(const DocumentSnapshot& document);
    void toggleWholeWord(const DocumentSnapshot& document);
    void toggleRegex(const DocumentSnapshot& document);
    void toggleSelection(const DocumentSnapshot& document,
                          std::optional<ByteRange> selection);
    void refresh(const DocumentSnapshot& document,
                 std::optional<ByteRange> selection);
    void next();
    void previous();

    [[nodiscard]] FindReplaceOperationResult replaceCurrent(
        Document& document, DocumentHistory& history,
        const SelectionSet& selectionsBefore,
        const SelectionSet& selectionsAfter, std::string replacement,
        std::uint64_t timestampMs);
    [[nodiscard]] FindReplaceOperationResult replaceAll(
        Document& document, DocumentHistory& history,
        const SelectionSet& selectionsBefore,
        const SelectionSet& selectionsAfter, std::string replacement,
        std::uint64_t timestampMs);

    [[nodiscard]] const FindReplaceViewState& viewState() const noexcept;

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

struct WorkspaceReplaceArguments {
    FindRequest request;
    std::string replacement;

    bool operator==(const WorkspaceReplaceArguments&) const = default;
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
    FindReplaceError error = FindReplaceError::None;
    Revision revision{0};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::None;
    }
};

class FindReplaceWorkspace {
public:
    virtual ~FindReplaceWorkspace() = default;
    [[nodiscard]] virtual WorkspaceSnapshot snapshot(
        Revision revision) const = 0;
    [[nodiscard]] virtual WorkspaceApplyResult apply(
        const WorkspaceReplacePreview& preview,
        WorkspaceRecoverySink& recoverySink) = 0;
    [[nodiscard]] virtual WorkspaceApplyResult recover(
        const WorkspaceRecoveryRecord& record) = 0;
};

struct WorkspacePreviewResult {
    FindReplaceError error = FindReplaceError::None;
    std::optional<WorkspaceReplacePreview> preview;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::None;
    }
};

[[nodiscard]] WorkspacePreviewResult previewWorkspaceReplace(
    const FindReplaceWorkspace& workspace, Revision sourceRevision,
    const FindRequest& request, std::string replacement);
[[nodiscard]] WorkspaceApplyResult applyWorkspaceReplace(
    FindReplaceWorkspace& workspace, const WorkspaceReplacePreview& preview,
    WorkspaceRecoverySink& recoverySink);
[[nodiscard]] WorkspaceApplyResult recoverWorkspaceReplace(
    FindReplaceWorkspace& workspace, const WorkspaceRecoveryRecord& record);

}  // namespace ssg
