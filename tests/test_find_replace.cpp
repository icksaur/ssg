#include "test_helpers.h"

#include <ssg/FindReplace.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::vector<FindMatch> referenceLiteral(std::string_view text,
                                         std::string_view query,
                                         FindOptions options,
                                         std::optional<ByteRange> selection) {
    auto fold = [&](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return options.caseSensitive
                   ? value
                   : static_cast<char>(std::tolower(byte));
    };
    auto word = [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) != 0 || value == '_' || byte >= 0x80U;
    };
    std::vector<FindMatch> result;
    if (query.empty()) {
        return result;
    }
    const auto lower = selection ? selection->begin.value() : 0;
    const auto upper = selection ? selection->end.value() : text.size();
    for (std::uint64_t at = lower; at + query.size() <= upper; ++at) {
        bool equal = true;
        for (std::size_t i = 0; i < query.size(); ++i) {
            equal = equal && fold(text[at + i]) == fold(query[i]);
        }
        if (!equal) {
            continue;
        }
        if (options.wholeWord &&
            ((at > lower && word(text[at - 1])) ||
             (at + query.size() < upper && word(text[at + query.size()])))) {
            continue;
        }
        result.push_back(
            FindMatch{ByteOffset{at}, ByteOffset{at + query.size()}});
        at += query.size() - 1;
    }
    return result;
}

SelectionSet caret(std::uint64_t offset) {
    const DocumentPosition p{ByteOffset{offset}, LineIndex{0}, CellIndex{0}};
    return SelectionSet{{Selection{p, p}}};
}

class RecordingSink final : public WorkspaceRecoverySink {
public:
    bool accept = true;
    std::optional<WorkspaceRecoveryRecord> record;

    bool store(const WorkspaceRecoveryRecord& value) override {
        if (!accept) {
            return false;
        }
        record = value;
        return true;
    }
};

class MemoryWorkspace final : public FindReplaceWorkspace {
public:
    explicit MemoryWorkspace(std::vector<WorkspaceFile> files)
        : files_(std::move(files)) {}

    WorkspaceSnapshot snapshot(Revision requested) const override {
        if (requested != revision_) {
            return WorkspaceSnapshot{revision_, {}, {}};
        }
        return WorkspaceSnapshot{revision_, files_, {}};
    }

    WorkspaceApplyResult apply(const WorkspaceReplacePreview& preview,
                               WorkspaceRecoverySink& sink) override {
        if (preview.sourceRevision != revision_) {
            return {FindReplaceError::StaleRevision, revision_,
                    "stale workspace preview"};
        }
        auto candidate = files_;
        for (const auto& change : preview.changes) {
            const auto it = std::find_if(
                candidate.begin(), candidate.end(), [&](const WorkspaceFile& f) {
                    return f.path == change.path;
                });
            if (it == candidate.end() || it->text != change.before) {
                return {FindReplaceError::WorkspaceRejected, revision_,
                        "workspace changed"};
            }
            it->text = change.after;
        }
        WorkspaceRecoveryRecord record{revision_, Revision{revision_.value() + 1},
                                       preview.changes};
        if (!sink.store(record)) {
            return {FindReplaceError::RecoveryRejected, revision_,
                    "recovery sink rejected record"};
        }
        files_ = std::move(candidate);
        revision_ = record.appliedRevision;
        return {FindReplaceError::None, revision_, {}};
    }

    WorkspaceApplyResult recover(
        const WorkspaceRecoveryRecord& record) override {
        if (record.appliedRevision != revision_) {
            return {FindReplaceError::StaleRevision, revision_,
                    "stale recovery record"};
        }
        auto candidate = files_;
        for (const auto& change : record.changes) {
            const auto it = std::find_if(
                candidate.begin(), candidate.end(), [&](const WorkspaceFile& f) {
                    return f.path == change.path;
                });
            if (it == candidate.end() || it->text != change.after) {
                return {FindReplaceError::WorkspaceRejected, revision_,
                        "workspace changed"};
            }
            it->text = change.before;
        }
        files_ = std::move(candidate);
        revision_ = Revision{revision_.value() + 1};
        return {FindReplaceError::None, revision_, {}};
    }

    const std::vector<WorkspaceFile>& files() const { return files_; }
    Revision revision() const { return revision_; }

private:
    Revision revision_{1};
    std::vector<WorkspaceFile> files_;
};

TEST(literalCaseWordAndSelectionMatchIndependentOracle) {
    const std::vector<std::string> texts = {
        "", "a", "Aa aA", "cat scatter cat_cat cat", "na\xC3\xAFve na"};
    const std::vector<std::string> queries = {"a", "A", "cat", "na"};
    for (const auto& text : texts) {
        for (const auto& query : queries) {
            for (const bool caseSensitive : {false, true}) {
                for (const bool wholeWord : {false, true}) {
                    FindOptions options{caseSensitive, wholeWord, false, false};
                    FindRequest request{query, options, std::nullopt, 100000,
                                        nullptr};
                    ASSERT_EQ(FindMatcher{}.find(text, request).matches,
                              referenceLiteral(text, query, options,
                                                std::nullopt));
                    if (text.size() >= 2) {
                        request.options.selectionOnly = true;
                        request.selection =
                            ByteRange{ByteOffset{1},
                                      ByteOffset{text.size() - 1}};
                        ASSERT_EQ(FindMatcher{}.find(text, request).matches,
                                  referenceLiteral(text, query, options,
                                                    request.selection));
                    }
                }
            }
        }
    }
}

TEST(regexOracleCoversGrammarCaseWordAndInvalidPattern) {
    FindRequest request{"(ab|cd)+", FindOptions{true, false, true, false},
                        std::nullopt, 100000, nullptr};
    ASSERT_EQ(FindMatcher{}.find("xxabcdcd yy ab", request).matches,
              (std::vector<FindMatch>{{ByteOffset{2}, ByteOffset{8}},
                                      {ByteOffset{12}, ByteOffset{14}}}));

    request.query = "h[ae]llo";
    request.options.caseSensitive = false;
    ASSERT_EQ(FindMatcher{}.find("HELLO hallo hxllo", request).matches,
              (std::vector<FindMatch>{{ByteOffset{0}, ByteOffset{5}},
                                      {ByteOffset{6}, ByteOffset{11}}}));

    request.query = "cat";
    request.options.wholeWord = true;
    ASSERT_EQ(FindMatcher{}.find("cat scatter cat", request).matches,
              (std::vector<FindMatch>{{ByteOffset{0}, ByteOffset{3}},
                                      {ByteOffset{12}, ByteOffset{15}}}));

    request.query = "a-?";
    ASSERT_EQ(FindMatcher{}.find("a-b", request).matches,
              (std::vector<FindMatch>{{ByteOffset{0}, ByteOffset{1}}}));

    request.query.clear();
    ASSERT_TRUE(FindMatcher{}.find("abc", request).matches.empty());

    request.query = "(unterminated";
    ASSERT_EQ(FindMatcher{}.find("text", request).error,
              FindReplaceError::InvalidPattern);
}

TEST(zeroWidthAdvancesOneUnicodeScalarAndBudgetCancels) {
    FindRequest request{"a*", FindOptions{true, false, true, false},
                        std::nullopt, 100000, nullptr};
    ASSERT_EQ(FindMatcher{}.find("a\xC3\xA9", request).matches,
              (std::vector<FindMatch>{{ByteOffset{0}, ByteOffset{1}},
                                      {ByteOffset{1}, ByteOffset{1}},
                                      {ByteOffset{3}, ByteOffset{3}}}));

    request.query = "(a|aa)*b";
    request.workBudget = 1;
    ASSERT_EQ(FindMatcher{}.find(std::string(200, 'a'), request).error,
              FindReplaceError::BudgetExhausted);

    request.query = "z";
    request.options.regex = false;
    request.workBudget = 3;
    ASSERT_EQ(FindMatcher{}.find("aaaaaaaa", request).error,
              FindReplaceError::BudgetExhausted);

    std::atomic_bool cancelled{true};
    request.workBudget = 100000;
    request.cancelled = &cancelled;
    ASSERT_EQ(FindMatcher{}.find("ab", request).error,
              FindReplaceError::Cancelled);
}

TEST(currentReplaceIsAtomicOneUndoUnitAndStaleSafe) {
    Document document{"one two one"};
    DocumentHistory history;
    FindReplaceController controller;
    controller.open(document.snapshot(),
                    FindRequest{"one", {}, std::nullopt, 100000, nullptr});
    ASSERT_EQ(controller.viewState().matches.size(), std::size_t{2});
    controller.next();
    auto replaced =
        controller.replaceCurrent(document, history, caret(0), caret(9), "1",
                                   10);
    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one two 1"});
    auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one two one"});
    auto redone = history.redo(document);
    ASSERT_TRUE(redone.accepted());
    ASSERT_EQ(redone.selections, std::optional<SelectionSet>{caret(9)});
    ASSERT_TRUE(history.undo(document).accepted());

    controller.refresh(document.snapshot(), std::nullopt);
    auto all =
        controller.replaceAll(document, history, caret(0), caret(7), "1", 20);
    ASSERT_TRUE(all.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"1 two 1"});
    ASSERT_TRUE(history.undo(document).accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one two one"});

    controller.refresh(document.snapshot(), std::nullopt);
    const auto external = document.apply(
        EditTransaction{document.revision(),
                        {{ByteOffset{0}, 0, "x"}}});
    ASSERT_TRUE(external.accepted());
    const auto before = document.snapshot();
    auto stale =
        controller.replaceAll(document, history, caret(0), caret(12), "z", 30);
    ASSERT_EQ(stale.error, FindReplaceError::StaleRevision);
    ASSERT_EQ(document.snapshot(), before);
}

TEST(workspacePreviewApplyRecoverAndFailuresRoundTrip) {
    MemoryWorkspace workspace{{{"a.txt", "cat cat"},
                               {"b.txt", "dog cat"},
                               {"c.txt", "none"}}};
    FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    auto preview =
        WorkspaceReplacer{}.preview(workspace, workspace.revision(), request, "x");
    ASSERT_TRUE(preview.accepted());
    ASSERT_EQ(preview.preview->changes.size(), std::size_t{2});
    ASSERT_EQ(preview.preview->changes[0].after, std::string{"x x"});

    RecordingSink rejecting;
    rejecting.accept = false;
    const auto original = workspace.files();
    auto rejected = WorkspaceReplacer{}.apply(workspace, *preview.preview, rejecting);
    ASSERT_EQ(rejected.error, FindReplaceError::RecoveryRejected);
    ASSERT_EQ(workspace.files(), original);

    RecordingSink sink;
    auto applied = WorkspaceReplacer{}.apply(workspace, *preview.preview, sink);
    ASSERT_TRUE(applied.accepted());
    ASSERT_TRUE(sink.record.has_value());
    ASSERT_EQ(workspace.files()[0].text, std::string{"x x"});
    ASSERT_TRUE(WorkspaceReplacer{}.recover(workspace, *sink.record).accepted());
    ASSERT_EQ(workspace.files(), original);

    auto stale = WorkspaceReplacer{}.apply(workspace, *preview.preview, sink);
    ASSERT_EQ(stale.error, FindReplaceError::StaleRevision);
    ASSERT_EQ(workspace.files(), original);
}

TEST(viewDeltaReplayAndCommandExportsAreExact) {
    const auto commands = FindReplaceCommandSet{};
    ASSERT_EQ(commands.descriptors().size(), std::size_t{16});
    ASSERT_EQ(commands.descriptors().front().id, std::string_view{"find.open"});
    ASSERT_EQ(commands.descriptors()[1].id,
              std::string_view{"find.word_under_cursor"});
    ASSERT_EQ(commands.descriptors().back().id,
              std::string_view{"replace.workspace_apply"});

    Document document{"alpha alpha"};
    FindReplaceController controller;
    const auto closed = controller.viewState();
    controller.open(document.snapshot(),
                    FindRequest{"alpha", {}, std::nullopt, 100000, nullptr});
    const auto open = controller.viewState();
    ASSERT_FALSE(open.replaceMode);
    controller.toggleCase(document.snapshot());
    ASSERT_TRUE(controller.viewState().options.caseSensitive);
    controller.updateQuery(document.snapshot(), "ALPHA", std::nullopt);
    ASSERT_EQ(controller.viewState().matches.size(), std::size_t{0});
    controller.openReplace(
        document.snapshot(),
        FindRequest{"alpha", {}, std::nullopt, 100000, nullptr});
    ASSERT_TRUE(controller.viewState().replaceMode);
    const auto delta = FindReplaceDeltaCodec{}.derive(closed, open);
    ASSERT_TRUE(delta.changed);
    ASSERT_EQ(FindReplaceDeltaCodec{}.replay(closed, delta).state, open);
    ASSERT_EQ(FindReplaceDeltaCodec{}.replay(open, delta).error,
              FindReplaceReplayError::BaseMismatch);
}

}  // namespace
}  // namespace ssg

int main() {
    RUN(ssg::literalCaseWordAndSelectionMatchIndependentOracle);
    RUN(ssg::regexOracleCoversGrammarCaseWordAndInvalidPattern);
    RUN(ssg::zeroWidthAdvancesOneUnicodeScalarAndBudgetCancels);
    RUN(ssg::currentReplaceIsAtomicOneUndoUnitAndStaleSafe);
    RUN(ssg::workspacePreviewApplyRecoverAndFailuresRoundTrip);
    RUN(ssg::viewDeltaReplayAndCommandExportsAreExact);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
