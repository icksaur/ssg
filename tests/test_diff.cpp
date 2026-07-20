#include "ssg/DiffModel.h"
#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ssg;

std::string fixture(std::string_view name) {
    std::ifstream input(std::filesystem::path{SSG_DIFF_FIXTURE_DIR} / name,
                        std::ios::binary);
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::vector<std::string> fixtureChanges(std::string_view name) {
    std::istringstream input{fixture(name)};
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) {
        result.push_back(std::move(line));
    }
    return result;
}

std::string reconstruct(std::string_view baseline,
                        const std::vector<DiffHunk>& hunks) {
    auto lines = splitDiffLines(baseline);
    std::ptrdiff_t offset = 0;
    for (const auto& hunk : hunks) {
        const auto start =
            static_cast<std::ptrdiff_t>(hunk.baselineStart) + offset;
        lines.erase(lines.begin() + start,
                    lines.begin() + start +
                        static_cast<std::ptrdiff_t>(hunk.baselineLines.size()));
        lines.insert(lines.begin() + start, hunk.targetLines.begin(),
                     hunk.targetLines.end());
        offset += static_cast<std::ptrdiff_t>(hunk.targetLines.size()) -
                  static_cast<std::ptrdiff_t>(hunk.baselineLines.size());
    }
    std::string result;
    for (const auto& line : lines) {
        result += line;
    }
    return result;
}

std::vector<std::string> normalized(const DiffFileView& view) {
    std::vector<std::string> result;
    for (const auto& change : view.changedLines) {
        const auto oldLine = change.baselineLine
                                  ? std::to_string(*change.baselineLine)
                                  : "-";
        const auto newLine =
            change.targetLine ? std::to_string(*change.targetLine) : "-";
        const char kind = change.kind == DiffLineKind::Modified
                              ? 'M'
                              : change.kind == DiffLineKind::Removed ? 'R' : 'A';
        result.push_back(std::string{kind} + " " + oldLine + " " + newLine);
    }
    return result;
}

const DiffFileView& onlyFile(const DiffModel& model) {
    const auto state = model.viewState();
    ASSERT_EQ(state.files.size(), std::size_t{1});
    return model.file(state.files.front().id)->get();
}

TEST(gitTrackedFixtureReconstructsAndMatchesIndependentChangedLines) {
    DiffModel model;
    const auto result = model.updateGitFile(
        {.id = DiffFileId{"tracked"},
         .path = "src/file.cpp",
         .indexContent = fixture("tracked.baseline"),
         .workingContent = fixture("tracked.target"),
         .indexIdentity = "index-a"},
        Revision{1});

    ASSERT_TRUE(result.accepted());
    const auto& view = onlyFile(model);
    ASSERT_EQ(reconstruct(fixture("tracked.baseline"), view.hunks),
              fixture("tracked.target"));
    ASSERT_EQ(normalized(view), fixtureChanges("tracked.changes"));
    ASSERT_EQ(view.baselineIdentity, std::string{"index-a"});
}

TEST(gitUntrackedRenameDeleteAndIndexChangeRetainIdentity) {
    DiffModel untracked;
    ASSERT_TRUE(untracked
                    .updateGitFile(
                        {.id = DiffFileId{"untracked"},
                         .path = "new.txt",
                         .workingContent = fixture("untracked.target"),
                         .indexIdentity = "index-a"},
                        Revision{1})
                    .accepted());
    ASSERT_EQ(normalized(onlyFile(untracked)),
              fixtureChanges("untracked.changes"));

    DiffModel renamed;
    ASSERT_TRUE(renamed
                    .updateGitFile(
                        {.id = DiffFileId{"stable"},
                         .path = "new-name.txt",
                         .previousPath = std::filesystem::path{"old-name.txt"},
                         .indexContent = fixture("tracked.baseline"),
                         .workingContent = fixture("tracked.target"),
                         .indexIdentity = "index-a"},
                        Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(renamed).id, DiffFileId{"stable"});
    ASSERT_EQ(onlyFile(renamed).previousPath,
              std::optional<std::filesystem::path>{"old-name.txt"});

    DiffModel removed;
    ASSERT_TRUE(removed
                    .updateGitFile(
                        {.id = DiffFileId{"deleted"},
                         .path = "gone.txt",
                         .indexContent = fixture("delete.baseline"),
                         .indexIdentity = "index-a"},
                        Revision{1})
                    .accepted());
    ASSERT_TRUE(onlyFile(removed).deleted);
    ASSERT_EQ(normalized(onlyFile(removed)),
              fixtureChanges("delete.changes"));
    ASSERT_EQ(diffOpenFile(onlyFile(removed)).path,
              std::filesystem::path{"gone.txt"});

    ASSERT_TRUE(renamed
                    .updateGitFile(
                        {.id = DiffFileId{"stable"},
                         .path = "new-name.txt",
                         .indexContent = fixture("tracked.target"),
                         .workingContent = fixture("tracked.target"),
                         .indexIdentity = "index-b"},
                        Revision{2})
                    .accepted());
    ASSERT_TRUE(onlyFile(renamed).hunks.empty());
    ASSERT_EQ(onlyFile(renamed).baselineIdentity, std::string{"index-b"});
}

TEST(seededNonGitEventsAdvanceBaselineAndRetainRenameDelete) {
    DiffModel model;
    ASSERT_TRUE(model.seedNonGit(
                         {{.id = DiffFileId{"seed"},
                           .path = "old.txt",
                           .content = fixture("tracked.baseline")}},
                         Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Rename,
                         .id = DiffFileId{"seed"},
                         .path = "new.txt",
                         .previousPath = std::filesystem::path{"old.txt"},
                         .content = fixture("tracked.target")},
                        Revision{2})
                    .accepted());
    ASSERT_EQ(onlyFile(model).id, DiffFileId{"seed"});
    ASSERT_EQ(onlyFile(model).previousPath,
              std::optional<std::filesystem::path>{"old.txt"});
    ASSERT_EQ(reconstruct(fixture("tracked.baseline"), onlyFile(model).hunks),
              fixture("tracked.target"));

    ASSERT_TRUE(model
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Modify,
                         .id = DiffFileId{"seed"},
                         .path = "new.txt",
                         .content = fixture("tracked.target") + "tail\n"},
                        Revision{3})
                    .accepted());
    ASSERT_EQ(onlyFile(model).hunks.front().baselineLines.size(),
              std::size_t{0});
    ASSERT_EQ(onlyFile(model).hunks.front().targetLines,
              (std::vector<std::string>{"tail\n"}));

    ASSERT_TRUE(model
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Remove,
                         .id = DiffFileId{"seed"},
                         .path = "new.txt"},
                        Revision{4})
                    .accepted());
    ASSERT_TRUE(onlyFile(model).deleted);
    ASSERT_EQ(diffOpenFile(onlyFile(model)).path,
              std::filesystem::path{"new.txt"});
}

TEST(staleInvalidAndOverBudgetWorkAreFailureAtomic) {
    DiffModel model{DiffConfig{.maximumLineCount = 4,
                               .maximumMatrixCells = 4}};
    ASSERT_TRUE(model.seedNonGit(
                         {{.id = DiffFileId{"seed"},
                           .path = "a.txt",
                           .content = "a\n"}},
                         Revision{1})
                    .accepted());
    const auto before = model.viewState();

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Modify,
                       .id = DiffFileId{"seed"},
                       .path = "a.txt",
                       .content = "b\n"},
                      Revision{1})
                  .error,
              DiffError::StaleRevision);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Remove,
                       .id = DiffFileId{"missing"},
                       .path = "missing.txt"},
                      Revision{2})
                  .error,
              DiffError::UnknownFile);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"seed"},
                       .path = "a.txt",
                       .indexContent = "a\n",
                       .workingContent = "b\n",
                       .indexIdentity = "index"},
                      Revision{2})
                  .error,
              DiffError::DuplicateFile);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"git"},
                       .path = "git.txt",
                       .indexContent = "a\n",
                       .workingContent = "b\n"},
                      Revision{2})
                  .error,
              DiffError::BaselineIdentityRequired);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Create,
                       .id = DiffFileId{"seed"},
                       .path = "a.txt"},
                      Revision{2})
                  .error,
              DiffError::ContentRequired);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Modify,
                       .id = DiffFileId{"seed"},
                       .path = "a.txt",
                       .content = "b\nc\nd\ne\nf\n"},
                      Revision{2})
                  .error,
              DiffError::WorkLimitExceeded);
    ASSERT_EQ(model.viewState(), before);
}

TEST(deltaReplayAndExactCommandNavigationContract) {
    DiffModel model;
    const auto base = model.viewState();
    ASSERT_TRUE(model
                    .updateGitFile(
                        {.id = DiffFileId{"tracked"},
                         .path = "file.txt",
                         .indexContent = "a\nsame\nb\n",
                         .workingContent = "A\nsame\nB\n",
                         .indexIdentity = "index"},
                        Revision{1})
                    .accepted());
    const auto target = model.viewState();
    const auto delta = DiffDeltaCodec{}.derive(base, target);
    const auto replay = DiffDeltaCodec{}.replay(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);

    auto stale = base;
    stale.revision = Revision{99};
    ASSERT_EQ(DiffDeltaCodec{}.replay(stale, delta).error,
              DiffReplayError::StaleRevision);

    const auto commands = diffCommandSet();
    ASSERT_EQ(commands.descriptors().size(), std::size_t{3});
    ASSERT_EQ(commands.descriptors()[0].id,
              std::string_view{"diff.next_hunk"});
    ASSERT_EQ(commands.descriptors()[1].id,
              std::string_view{"diff.previous_hunk"});
    ASSERT_EQ(commands.descriptors()[2].id,
              std::string_view{"diff.open_file"});

    const auto& file = onlyFile(model);
    ASSERT_EQ(nextDiffHunk(file, std::nullopt), std::optional<std::size_t>{0});
    ASSERT_EQ(nextDiffHunk(file, file.hunks.front().targetStart),
              std::optional<std::size_t>{1});
    ASSERT_EQ(nextDiffHunk(file, file.hunks.back().targetStart),
              std::optional<std::size_t>{0});
    ASSERT_EQ(previousDiffHunk(file, std::nullopt),
              std::optional<std::size_t>{1});
    ASSERT_EQ(previousDiffHunk(file, file.hunks.front().targetStart),
              std::optional<std::size_t>{1});
}

} // namespace

int main() {
    RUN(gitTrackedFixtureReconstructsAndMatchesIndependentChangedLines);
    RUN(gitUntrackedRenameDeleteAndIndexChangeRetainIdentity);
    RUN(seededNonGitEventsAdvanceBaselineAndRetainRenameDelete);
    RUN(staleInvalidAndOverBudgetWorkAreFailureAtomic);
    RUN(deltaReplayAndExactCommandNavigationContract);
    return failed == 0 ? 0 : 1;
}
