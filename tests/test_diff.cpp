#include "ssg/diff.h"
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
            static_cast<std::ptrdiff_t>(hunk.baseline_start) + offset;
        lines.erase(lines.begin() + start,
                    lines.begin() + start +
                        static_cast<std::ptrdiff_t>(hunk.baseline_lines.size()));
        lines.insert(lines.begin() + start, hunk.target_lines.begin(),
                     hunk.target_lines.end());
        offset += static_cast<std::ptrdiff_t>(hunk.target_lines.size()) -
                  static_cast<std::ptrdiff_t>(hunk.baseline_lines.size());
    }
    std::string result;
    for (const auto& line : lines) {
        result += line;
    }
    return result;
}

std::vector<std::string> normalized(const DiffFileView& view) {
    std::vector<std::string> result;
    for (const auto& change : view.changed_lines) {
        const auto oldLine = change.baseline_line
                                  ? std::to_string(*change.baseline_line)
                                  : "-";
        const auto newLine =
            change.target_line ? std::to_string(*change.target_line) : "-";
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
         .index_content = fixture("tracked.baseline"),
         .working_content = fixture("tracked.target"),
         .index_identity = "index-a"},
        Revision{1});

    ASSERT_TRUE(result.accepted());
    const auto& view = onlyFile(model);
    ASSERT_EQ(reconstruct(fixture("tracked.baseline"), view.hunks),
              fixture("tracked.target"));
    ASSERT_EQ(normalized(view), fixtureChanges("tracked.changes"));
    ASSERT_EQ(view.baseline_identity, std::string{"index-a"});
}

TEST(gitUntrackedRenameDeleteAndIndexChangeRetainIdentity) {
    DiffModel untracked;
    ASSERT_TRUE(untracked
                    .updateGitFile(
                        {.id = DiffFileId{"untracked"},
                         .path = "new.txt",
                         .working_content = fixture("untracked.target"),
                         .index_identity = "index-a"},
                        Revision{1})
                    .accepted());
    ASSERT_EQ(normalized(onlyFile(untracked)),
              fixtureChanges("untracked.changes"));

    DiffModel renamed;
    ASSERT_TRUE(renamed
                    .updateGitFile(
                        {.id = DiffFileId{"stable"},
                         .path = "new-name.txt",
                         .previous_path = std::filesystem::path{"old-name.txt"},
                         .index_content = fixture("tracked.baseline"),
                         .working_content = fixture("tracked.target"),
                         .index_identity = "index-a"},
                        Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(renamed).id, DiffFileId{"stable"});
    ASSERT_EQ(onlyFile(renamed).previous_path,
              std::optional<std::filesystem::path>{"old-name.txt"});

    DiffModel removed;
    ASSERT_TRUE(removed
                    .updateGitFile(
                        {.id = DiffFileId{"deleted"},
                         .path = "gone.txt",
                         .index_content = fixture("delete.baseline"),
                         .index_identity = "index-a"},
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
                         .index_content = fixture("tracked.target"),
                         .working_content = fixture("tracked.target"),
                         .index_identity = "index-b"},
                        Revision{2})
                    .accepted());
    ASSERT_TRUE(onlyFile(renamed).hunks.empty());
    ASSERT_EQ(onlyFile(renamed).baseline_identity, std::string{"index-b"});
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
                         .previous_path = std::filesystem::path{"old.txt"},
                         .content = fixture("tracked.target")},
                        Revision{2})
                    .accepted());
    ASSERT_EQ(onlyFile(model).id, DiffFileId{"seed"});
    ASSERT_EQ(onlyFile(model).previous_path,
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
    ASSERT_EQ(onlyFile(model).hunks.front().baseline_lines.size(),
              std::size_t{0});
    ASSERT_EQ(onlyFile(model).hunks.front().target_lines,
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
    DiffModel model{DiffConfig{.maximum_line_count = 4,
                               .maximum_matrix_cells = 4}};
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
                       .index_content = "a\n",
                       .working_content = "b\n",
                       .index_identity = "index"},
                      Revision{2})
                  .error,
              DiffError::DuplicateFile);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"git"},
                       .path = "git.txt",
                       .index_content = "a\n",
                       .working_content = "b\n"},
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
                         .index_content = "a\nsame\nb\n",
                         .working_content = "A\nsame\nB\n",
                         .index_identity = "index"},
                        Revision{1})
                    .accepted());
    const auto target = model.viewState();
    const auto delta = deriveDiffDelta(base, target);
    const auto replay = replayDiffDelta(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);

    auto stale = base;
    stale.revision = Revision{99};
    ASSERT_EQ(replayDiffDelta(stale, delta).error,
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
    ASSERT_EQ(nextDiffHunk(file, file.hunks.front().target_start),
              std::optional<std::size_t>{1});
    ASSERT_EQ(nextDiffHunk(file, file.hunks.back().target_start),
              std::optional<std::size_t>{0});
    ASSERT_EQ(previousDiffHunk(file, std::nullopt),
              std::optional<std::size_t>{1});
    ASSERT_EQ(previousDiffHunk(file, file.hunks.front().target_start),
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
