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
        .baselineContent = fixture("tracked.baseline"),
         .workingContent = fixture("tracked.target")},
        "index-a", Revision{1});

    ASSERT_TRUE(result.accepted());
    const auto& view = onlyFile(model);
    ASSERT_EQ(reconstruct(fixture("tracked.baseline"), view.hunks),
              fixture("tracked.target"));
    ASSERT_EQ(normalized(view), fixtureChanges("tracked.changes"));
    ASSERT_EQ(view.baselineIdentity, std::string{"index-a"});
}

TEST(modifiedLineMarksOnlyChangedWordTokens) {
    DiffModel model;
    ASSERT_TRUE(model
                    .updateGitFile(
                        {.id = DiffFileId{"words"},
                         .path = "words.cpp",
                        .baselineContent = "int foo = 1;\n",
                         .workingContent = "int foo = 42;\n"},
                        "index", Revision{1})
                    .accepted());

    const auto& changes = onlyFile(model).changedLines;
    ASSERT_EQ(changes.size(), std::size_t{1});
    ASSERT_EQ(changes.front().kind, DiffLineKind::Modified);
    ASSERT_TRUE(changes.front().targetAddedWordRanges.empty());
    ASSERT_EQ(changes.front().baselineRemovedWordRanges,
              (std::vector<DiffWordRange>{{10, 1}}));
    ASSERT_EQ(changes.front().targetModifiedWordRanges,
              (std::vector<DiffWordRange>{{10, 2}}));
}

TEST(wordDiffWorkLimitIsFailureAtomic) {
    DiffModel model{DiffConfig{.maximumLineCount = 10,
                               .maximumMatrixCells = 100,
                               .maximumWordMatrixCells = 4}};
    const auto before = model.viewState();

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"words"},
                       .path = "words.cpp",
                       .baselineContent = "one two\n",
                       .workingContent = "three four\n"},
                      "index", Revision{1})
                  .error,
              DiffError::WorkLimitExceeded);
    ASSERT_EQ(model.viewState(), before);
}

TEST(wordMarksUseStableByteRangesForInsertionAndUtf8) {
    DiffModel insertion;
    ASSERT_TRUE(insertion
                    .updateGitFile(
                        {.id = DiffFileId{"insertion"},
                         .path = "insertion.cpp",
                         .baselineContent = "int foo;\n",
                         .workingContent = "int new foo;\n"},
                        "index", Revision{1})
                    .accepted());
    const auto& inserted = onlyFile(insertion).changedLines.front();
    ASSERT_EQ(inserted.targetAddedWordRanges,
              (std::vector<DiffWordRange>{{4, 4}}));
    ASSERT_TRUE(inserted.baselineRemovedWordRanges.empty());
    ASSERT_TRUE(inserted.targetModifiedWordRanges.empty());

    DiffModel utf8;
    ASSERT_TRUE(utf8
                    .updateGitFile(
                        {.id = DiffFileId{"utf8"},
                         .path = "utf8.txt",
                         .baselineContent = "\xf0\x9f\x98\x80 x\n",
                         .workingContent = "\xf0\x9f\x98\x80 y\n"},
                        "index", Revision{1})
                    .accepted());
    const auto& changed = onlyFile(utf8).changedLines.front();
    ASSERT_EQ(changed.baselineRemovedWordRanges,
              (std::vector<DiffWordRange>{{5, 1}}));
    ASSERT_EQ(changed.targetModifiedWordRanges,
              (std::vector<DiffWordRange>{{5, 1}}));
}

TEST(gitUntrackedRenameDeleteAndIndexChangeRetainIdentity) {
    DiffModel untracked;
    ASSERT_TRUE(untracked
                    .updateGitFile(
                        {.id = DiffFileId{"untracked"},
                         .path = "new.txt",
                         .workingContent = fixture("untracked.target")},
                        "index-a", Revision{1})
                    .accepted());
    ASSERT_EQ(normalized(onlyFile(untracked)),
              fixtureChanges("untracked.changes"));

    DiffModel renamed;
    ASSERT_TRUE(renamed
                    .updateGitFile(
                        {.id = DiffFileId{"stable"},
                         .path = "new-name.txt",
                         .previousPath = std::filesystem::path{"old-name.txt"},
                         .baselineContent = fixture("tracked.baseline"),
                         .workingContent = fixture("tracked.target")},
                        "index-a", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(renamed).id, DiffFileId{"stable"});
    ASSERT_EQ(onlyFile(renamed).previousPath,
              std::optional<std::filesystem::path>{"old-name.txt"});

    DiffModel removed;
    ASSERT_TRUE(removed
                    .updateGitFile(
                        {.id = DiffFileId{"deleted"},
                         .path = "gone.txt",
                         .baselineContent = fixture("delete.baseline")},
                        "index-a", Revision{1})
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
                         .baselineContent = fixture("tracked.target"),
                         .workingContent = fixture("tracked.target")},
                        "index-b", Revision{2})
                    .accepted());
    ASSERT_TRUE(onlyFile(renamed).hunks.empty());
    ASSERT_EQ(onlyFile(renamed).baselineIdentity, std::string{"index-b"});
}

TEST(gitFileStatusIsComputedWithDeterministicPrecedence) {
    DiffModel added;
    ASSERT_TRUE(added
                    .updateGitFile(
                        {.id = DiffFileId{"added"},
                         .path = "added.txt",
                         .workingContent = "new\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(added).status, DiffFileStatus::Added);

    DiffModel modified;
    ASSERT_TRUE(modified
                    .updateGitFile(
                        {.id = DiffFileId{"modified"},
                         .path = "modified.txt",
                         .baselineContent = "old\n",
                         .workingContent = "new\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(modified).status, DiffFileStatus::Modified);

    DiffModel deleted;
    ASSERT_TRUE(deleted
                    .updateGitFile(
                        {.id = DiffFileId{"deleted"},
                         .path = "deleted.txt",
                         .baselineContent = "old\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(deleted).status, DiffFileStatus::Deleted);

    DiffModel renamed;
    ASSERT_TRUE(renamed
                    .updateGitFile(
                        {.id = DiffFileId{"renamed"},
                         .path = "new-name.txt",
                         .previousPath = std::filesystem::path{"old-name.txt"},
                         .baselineContent = "same\n",
                         .workingContent = "same\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(renamed).status, DiffFileStatus::Renamed);

    DiffModel renamedModified;
    ASSERT_TRUE(renamedModified
                    .updateGitFile(
                        {.id = DiffFileId{"renamed-modified"},
                         .path = "new-name.txt",
                         .previousPath = std::filesystem::path{"old-name.txt"},
                         .baselineContent = "old\n",
                         .workingContent = "new\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(renamedModified).status, DiffFileStatus::Renamed);

    DiffModel deletedRenamed;
    ASSERT_TRUE(deletedRenamed
                    .updateGitFile(
                        {.id = DiffFileId{"deleted-renamed"},
                         .path = "new-name.txt",
                         .previousPath = std::filesystem::path{"old-name.txt"},
                         .baselineContent = "old\n"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(deletedRenamed).status, DiffFileStatus::Deleted);

    DiffModel deletedAddedLooking;
    ASSERT_TRUE(deletedAddedLooking
                    .updateGitFile(
                        {.id = DiffFileId{"deleted-added"},
                         .path = "edge.txt"},
                        "head", Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(deletedAddedLooking).status, DiffFileStatus::Deleted);
}

TEST(externalDiffsUseExplicitAppOwnedBaselineAndRetainRenameDelete) {
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
                         .baselineContent = fixture("tracked.baseline"),
                         .targetContent = fixture("tracked.target")},
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
                        .baselineContent = fixture("tracked.baseline"),
                        .targetContent = fixture("tracked.target") + "tail\n"},
                        Revision{3})
                    .accepted());
    ASSERT_EQ(reconstruct(fixture("tracked.baseline"), onlyFile(model).hunks),
              fixture("tracked.target") + "tail\n");
    ASSERT_TRUE(onlyFile(model).hunks.size() > std::size_t{1});

    ASSERT_TRUE(model
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Remove,
                         .id = DiffFileId{"seed"},
                        .path = "new.txt",
                        .baselineContent = fixture("tracked.baseline")},
                        Revision{4})
                    .accepted());
    ASSERT_TRUE(onlyFile(model).deleted);
    ASSERT_EQ(diffOpenFile(onlyFile(model)).path,
              std::filesystem::path{"new.txt"});
}

TEST(nonGitStatusUsesSourceAgnosticClassification) {
    DiffModel seeded;
    ASSERT_TRUE(seeded.seedNonGit(
                         {{.id = DiffFileId{"seed"},
                           .path = "seed.txt",
                           .content = "seed\n"}},
                         Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(seeded).status, DiffFileStatus::Added);

    DiffModel created;
    ASSERT_TRUE(created
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Create,
                         .id = DiffFileId{"created"},
                         .path = "created.txt",
                         .baselineContent = "",
                         .targetContent = std::string{"new\n"}},
                        Revision{1})
                    .accepted());
    ASSERT_EQ(onlyFile(created).status, DiffFileStatus::Added);

    ASSERT_TRUE(created
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Rename,
                         .id = DiffFileId{"created"},
                         .path = "renamed.txt",
                         .previousPath = std::filesystem::path{"created.txt"},
                         .baselineContent = "new\n",
                         .targetContent = std::string{"new\n"}},
                        Revision{2})
                    .accepted());
    ASSERT_EQ(onlyFile(created).status, DiffFileStatus::Renamed);

    ASSERT_TRUE(created
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Modify,
                         .id = DiffFileId{"created"},
                         .path = "renamed.txt",
                         .previousPath = std::filesystem::path{"stale-old.txt"},
                         .baselineContent = "new\n",
                         .targetContent = std::string{"changed\n"}},
                        Revision{3})
                    .accepted());
    ASSERT_EQ(onlyFile(created).status, DiffFileStatus::Modified);

    ASSERT_TRUE(created
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Remove,
                         .id = DiffFileId{"created"},
                         .path = "renamed.txt",
                         .baselineContent = "changed\n"},
                        Revision{4})
                    .accepted());
    ASSERT_EQ(onlyFile(created).status, DiffFileStatus::Deleted);
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
                      .baselineContent = "a\n",
                      .targetContent = "b\n"},
                      Revision{1})
                  .error,
              DiffError::StaleRevision);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Remove,
                       .id = DiffFileId{"missing"},
                      .path = "missing.txt",
                      .baselineContent = "a\n"},
                      Revision{2})
                  .error,
              DiffError::UnknownFile);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"seed"},
                       .path = "a.txt",
                      .baselineContent = "a\n",
                       .workingContent = "b\n"},
                      "index", Revision{2})
                  .error,
              DiffError::DuplicateFile);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .updateGitFile(
                      {.id = DiffFileId{"git"},
                       .path = "git.txt",
                       .baselineContent = "a\n",
                       .workingContent = "b\n"},
                      "", Revision{2})
                  .error,
              DiffError::BaselineIdentityRequired);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Create,
                       .id = DiffFileId{"seed"},
                      .path = "a.txt",
                      .baselineContent = ""},
                      Revision{2})
                  .error,
              DiffError::ContentRequired);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNonGitEvent(
                      {.kind = NonGitDiffEventKind::Modify,
                       .id = DiffFileId{"seed"},
                       .path = "a.txt",
                      .baselineContent = "a\n",
                      .targetContent = "b\nc\nd\ne\nf\n"},
                      Revision{2})
                  .error,
              DiffError::WorkLimitExceeded);
    ASSERT_EQ(model.viewState(), before);
}

TEST(hunkNavigationWrapsAroundChanges) {
    DiffModel model;
    ASSERT_TRUE(model
                    .updateGitFile(
                        {.id = DiffFileId{"tracked"},
                         .path = "file.txt",
                         .baselineContent = "a\nsame\nb\n",
                         .workingContent = "A\nsame\nB\n"},
                        "index", Revision{1})
                    .accepted());
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

TEST(documentDiffLookupUsesIdentityOnly) {
    DiffFileView fileA{DiffFileId{"a.cpp"}};
    fileA.path = "a.cpp";
    DiffFileView fileB{DiffFileId{"b.cpp"}};
    fileB.path = "b.cpp";
    DiffViewState diff{Revision{8}, {fileA, fileB}};

    DocumentViewState document{Revision{8}, "x", ByteOffset{0}};
    document.diffFileIdentity = std::string{"b.cpp"};
    auto const selected = diff.fileForDocument(document);
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected->get().id, DiffFileId{"b.cpp"});

    DocumentViewState missing{Revision{8}, "x", ByteOffset{0}};
    missing.diffFileIdentity = std::string{"missing.cpp"};
    ASSERT_FALSE(diff.fileForDocument(missing).has_value());

    DocumentViewState revisionMismatch{Revision{7}, "x", ByteOffset{0}};
    revisionMismatch.diffFileIdentity = std::string{"b.cpp"};
    auto const matchedWithDifferentRevision =
        diff.fileForDocument(revisionMismatch);
    ASSERT_TRUE(matchedWithDifferentRevision.has_value());
    ASSERT_EQ(matchedWithDifferentRevision->get().id, DiffFileId{"b.cpp"});
}

TEST(gitRemoveFileRejectsStaleOrEqualRevisionAndRemovesOnNextRevision) {
    DiffModel model;
    ASSERT_TRUE(model
                    .updateGitFile(
                        {.id = DiffFileId{"tracked"},
                         .path = "tracked.txt",
                         .baselineContent = "a\n",
                         .workingContent = "b\n"},
                        "head-a", Revision{1})
                    .accepted());
    ASSERT_EQ(model.viewState().files.size(), std::size_t{1});

    const auto before = model.viewState();
    ASSERT_EQ(model.removeFile(DiffFileId{"tracked"}, Revision{1}).error,
              DiffError::StaleRevision);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_TRUE(model.removeFile(DiffFileId{"tracked"}, Revision{2}).accepted());
    ASSERT_TRUE(model.viewState().files.empty());
    ASSERT_EQ(model.viewState().revision, Revision{2});
}

TEST(gitScanClassificationDistinguishesGitFromNonGitEntries) {
    // The invariant the git-scan reconciliation relies on: isGitFile is true
    // only for git-sourced entries, so a rescan can leave draft-vs-disk and
    // external-modification (non-git) entries untouched instead of evicting
    // them.
    DiffModel model;
    ASSERT_TRUE(model
                    .updateGitFile({.id = DiffFileId{"tracked"},
                                    .path = "src/file.cpp",
                                    .baselineContent = std::string{"a\n"},
                                    .workingContent = std::string{"b\n"}},
                                   "index-a", Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .applyNonGitEvent({NonGitDiffEventKind::Create,
                                       DiffFileId{"draft:note.txt"},
                                       "note.txt", std::nullopt,
                                       std::string{"disk\n"},
                                       std::string{"draft\n"}},
                                      Revision{2})
                    .accepted());

    ASSERT_TRUE(model.isGitFile(DiffFileId{"tracked"}));
    ASSERT_FALSE(model.isGitFile(DiffFileId{"draft:note.txt"}));
    ASSERT_FALSE(model.isGitFile(DiffFileId{"unknown"}));
}

}  // namespace

SSG_TEST_SUITE(test_diff) {
    RUN(gitTrackedFixtureReconstructsAndMatchesIndependentChangedLines);
    RUN(modifiedLineMarksOnlyChangedWordTokens);
    RUN(wordDiffWorkLimitIsFailureAtomic);
    RUN(wordMarksUseStableByteRangesForInsertionAndUtf8);
    RUN(gitUntrackedRenameDeleteAndIndexChangeRetainIdentity);
    RUN(gitFileStatusIsComputedWithDeterministicPrecedence);
    RUN(externalDiffsUseExplicitAppOwnedBaselineAndRetainRenameDelete);
    RUN(nonGitStatusUsesSourceAgnosticClassification);
    RUN(staleInvalidAndOverBudgetWorkAreFailureAtomic);
    RUN(hunkNavigationWrapsAroundChanges);
    RUN(documentDiffLookupUsesIdentityOnly);
    RUN(gitRemoveFileRejectsStaleOrEqualRevisionAndRemovesOnNextRevision);
    RUN(gitScanClassificationDistinguishesGitFromNonGitEntries);
    return failed == 0 ? 0 : 1;
}
