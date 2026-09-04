#pragma once

#include <ssg/Document.h>
#include <ssg/DocumentHistory.h>
#include <ssg/Search.h>
#include <ssg/Selection.h>

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
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
    bool selectionOnly = false;
    bool operator==(const FindOptions&) const noexcept = default;
};

struct FindMatch {
    ByteOffset begin;
    ByteOffset end;
    bool operator==(const FindMatch&) const noexcept = default;
};

enum class FindReplaceError : std::uint8_t {
    None = 0,
    InvalidPattern = 1,
    InvalidUtf8 = 2,
    InvalidSelection = 3,
    BudgetExhausted = 4,
    Cancelled = 5,
    NoMatch = 6,
    StaleRevision = 7,
    DocumentRejected = 8,
    WorkspaceRejected = 9,
    RecoveryRejected = 10,
};

struct FindRequest {
    std::string query;
    FindOptions options;
    std::optional<ByteRange> selection;
    std::uint64_t workBudget = 1'000'000;
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

class FindMatcher {
public:
    [[nodiscard]] FindResult find(std::string_view text,
                                  const FindRequest& request) const;
};

enum class FindReplaceCommand : std::uint8_t {
    FindOpen,
    FindWordUnderCursor,
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
// the next full string.
struct FindQueryArguments {
    std::string query;
    friend bool operator==(const FindQueryArguments&, const FindQueryArguments&) = default;
};

struct FindReplaceViewState {
    std::uint64_t generation = 0;
    bool open = false;
    bool replaceMode = false;
    std::uint64_t sourceRevision{0};
    std::string query;
    std::string replacement;
    FindOptions options;
    std::vector<FindMatch> matches;
    std::optional<std::size_t> activeMatch;
    FindReplaceError error = FindReplaceError::None;
    std::string message;
    bool operator==(const FindReplaceViewState&) const = default;
};

struct FindReplaceOperationResult {
    FindReplaceError error = FindReplaceError::None;
    std::uint64_t revision{0};
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
    std::uint64_t sourceRevision{0};
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
    std::uint64_t sourceRevision{0};
    std::uint64_t appliedRevision{0};
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
    std::uint64_t revision{0};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == FindReplaceError::None;
    }
};

class FindReplaceWorkspace {
public:
    virtual ~FindReplaceWorkspace() = default;
    [[nodiscard]] virtual WorkspaceSnapshot snapshot(
        std::uint64_t revision) const = 0;
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

class WorkspaceReplacer {
public:
    [[nodiscard]] WorkspacePreviewResult preview(
        const FindReplaceWorkspace& workspace, std::uint64_t sourceRevision,
        const FindRequest& request, std::string replacement) const;
    [[nodiscard]] WorkspaceApplyResult apply(
        FindReplaceWorkspace& workspace,
        const WorkspaceReplacePreview& preview,
        WorkspaceRecoverySink& recoverySink) const;
    [[nodiscard]] WorkspaceApplyResult recover(
        FindReplaceWorkspace& workspace,
        const WorkspaceRecoveryRecord& record) const;
};

}  // namespace ssg
