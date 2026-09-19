#include <ssg/WorkspaceFileIndex.h>

#include "grid_test_frame.h"
#include "test_helpers.h"
#include "editor_test_support.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

int runStatus(const fs::path& root, std::string_view command) {
    return runGitStatus(root, command);
}

fs::path makeUniqueRoot(std::string_view label) {
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return testSystemRuntimePath(
        "workspace_index_" + std::string{label} + "_" + suffix);
}

void writeFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream{path} << text;
}

std::vector<std::string> ids(const WorkspaceFileIndexResult& result) {
    std::vector<std::string> out;
    out.reserve(result.candidates.size());
    for (const auto& candidate : result.candidates) out.push_back(candidate.id);
    return out;
}

// A fixture tree whose expected contents are enumerated by hand below, so the
// assertion does not go through the same walk it is checking.
fs::path buildFixture(std::string_view label) {
    auto root = makeUniqueRoot(label);
    writeFile(root / ".gitignore", "build/\n*.log\n");
    writeFile(root / "README.md", "readme\n");
    writeFile(root / "debug.log", "noise\n");
    writeFile(root / "src" / "main.cpp", "int main(){}\n");
    writeFile(root / "src" / "util" / "helper.cpp", "void h(){}\n");
    writeFile(root / "build" / "artifact.o", "binary\n");
    writeFile(root / "build" / "deep" / "nested.o", "binary\n");
    return root;
}

TEST(indexListsEveryTrackedFileAndNoDirectories) {
    auto root = buildFixture("index-basic");
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto result = buildWorkspaceFileIndex(root, *matcher);

    // Independently enumerated: every file in the fixture that gitignore keeps.
    const std::vector<std::string> expected{
        ".gitignore", "README.md", "src/main.cpp", "src/util/helper.cpp"};
    ASSERT_EQ(ids(result), expected);
    ASSERT_FALSE(result.truncated);
    fs::remove_all(root);
}

TEST(indexCandidateCarriesPathAsIdFilenameAsLabelAndParentAsDetail) {
    auto root = buildFixture("index-shape");
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto result = buildWorkspaceFileIndex(root, *matcher);

    const PaletteCandidate* nested = nullptr;
    const PaletteCandidate* top = nullptr;
    for (const auto& candidate : result.candidates) {
        if (candidate.id == "src/util/helper.cpp") nested = &candidate;
        if (candidate.id == "README.md") top = &candidate;
    }
    ASSERT_TRUE(nested != nullptr);
    ASSERT_TRUE(top != nullptr);
    if (nested) {
        ASSERT_EQ(nested->label, std::string{"helper.cpp"});
        ASSERT_EQ(nested->detail, std::string{"src/util"});
    }
    // A file at the root has no parent directory to show.
    if (top) {
        ASSERT_EQ(top->label, std::string{"README.md"});
        ASSERT_EQ(top->detail, std::string{});
    }
    fs::remove_all(root);
}

TEST(indexWithoutGitignoreFilteringReappearsTheIgnoredSubtree) {
    auto root = buildFixture("index-unfiltered");
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto result = buildWorkspaceFileIndex(root, *matcher,
                                             {/*respectGitignore=*/false, 20000});

    const std::vector<std::string> expected{
        ".gitignore",         "README.md",
        "build/artifact.o",   "build/deep/nested.o",
        "debug.log",          "src/main.cpp",
        "src/util/helper.cpp"};
    ASSERT_EQ(ids(result), expected);
    fs::remove_all(root);
}

// The rule is prune-during-descent, not filter-after-walk.  Both produce the
// same candidate list, so the only way to tell them apart is to make descending
// observable: an unreadable directory would be walked by a post-filter, and a
// huge one would be paid for.  Here the ignored subtree is made enormous and the
// walk is required to stay fast, which a post-filter cannot do.
TEST(indexNeverDescendsIntoAnIgnoredDirectory) {
    auto root = makeUniqueRoot("index-prune");
    writeFile(root / ".gitignore", "heavy/\n");
    writeFile(root / "kept.txt", "x\n");
    for (int outer = 0; outer < 40; ++outer) {
        for (int inner = 0; inner < 50; ++inner) {
            writeFile(root / "heavy" / std::to_string(outer) /
                          (std::to_string(inner) + ".bin"),
                      "x\n");
        }
    }
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto matcher = makePlatformGitIgnoreMatcher(root);

    auto const start = std::chrono::steady_clock::now();
    auto pruned = buildWorkspaceFileIndex(root, *matcher);
    auto const prunedMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    const std::vector<std::string> expected{".gitignore", "kept.txt"};
    ASSERT_EQ(ids(pruned), expected);

    // The same tree walked WITHOUT pruning must visit the 2000 ignored files,
    // which is the cost a post-hoc filter would always pay.
    auto const unprunedStart = std::chrono::steady_clock::now();
    auto full = buildWorkspaceFileIndex(root, *matcher,
                                           {/*respectGitignore=*/false, 20000});
    auto const unprunedMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - unprunedStart)
            .count();
    ASSERT_EQ(full.candidates.size(), std::size_t{2002});

    // A generous margin: this asserts the ignored subtree was skipped, not a
    // particular speed. Post-filtering would make these two roughly equal.
    ASSERT_TRUE(prunedMicros * 4 < unprunedMicros);
    fs::remove_all(root);
}

TEST(indexNeverOffersTheGitDirectory) {
    auto root = makeUniqueRoot("index-gitdir");
    writeFile(root / "kept.txt", "x\n");
    ASSERT_EQ(runStatus(root, "init -q"), 0);
    ASSERT_TRUE(fs::exists(root / ".git"));

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto result = buildWorkspaceFileIndex(root, *matcher);

    for (const auto& id : ids(result)) {
        ASSERT_TRUE(id.rfind(".git/", 0) != 0);
    }
    ASSERT_EQ(ids(result), (std::vector<std::string>{"kept.txt"}));
    fs::remove_all(root);
}

TEST(indexTruncatesDeterministicallyAtTheCap) {
    auto root = makeUniqueRoot("index-cap");
    for (int index = 0; index < 40; ++index) {
        // Zero-padded so lexicographic order is also numeric order, making the
        // expected prefix unambiguous.
        writeFile(root / ("file-" + std::string(2 - std::to_string(index).size(), '0') +
                          std::to_string(index) + ".txt"),
                  "x\n");
    }

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto first = buildWorkspaceFileIndex(root, *matcher,
                                            {/*respectGitignore=*/true, 5});
    ASSERT_TRUE(first.truncated);
    ASSERT_EQ(first.candidates.size(), std::size_t{5});
    ASSERT_EQ(ids(first), (std::vector<std::string>{"file-00.txt", "file-01.txt",
                                                    "file-02.txt", "file-03.txt",
                                                    "file-04.txt"}));

    // Deterministic means repeatable: the same cap yields the same survivors.
    auto second = buildWorkspaceFileIndex(root, *matcher,
                                             {/*respectGitignore=*/true, 5});
    ASSERT_EQ(ids(first), ids(second));
    fs::remove_all(root);
}

TEST(indexOutsideAGitRepositoryListsEverything) {
    auto root = buildFixture("index-norepo");
    auto matcher = makePlatformGitIgnoreMatcher(root);
    ASSERT_FALSE(matcher->usable());

    auto result = buildWorkspaceFileIndex(root, *matcher);
    // No usable repository means no ignore rules apply, not an error.
    ASSERT_EQ(result.candidates.size(), std::size_t{7});
    fs::remove_all(root);
}

TEST(indexDoesNotDescendIntoSymlinkedDirectories) {
    auto root = makeUniqueRoot("index-symlink");
    writeFile(root / "real" / "file.txt", "x\n");
    std::error_code error;
    fs::create_directory_symlink(root / "real", root / "link", error);
    if (error) {  // Filesystem without symlink support; nothing to assert.
        fs::remove_all(root);
        return;
    }

    auto matcher = makePlatformGitIgnoreMatcher(root);
    auto result = buildWorkspaceFileIndex(root, *matcher);
    ASSERT_EQ(ids(result), (std::vector<std::string>{"real/file.txt"}));
    fs::remove_all(root);
}

TEST(indexOffersSymlinkedFilesThatTheEditorCanOpen) {
    auto root = makeUniqueRoot("index-symlink-file");
    auto workspace = root / "workspace";
    writeFile(workspace / "real.txt", "linked\n");
    std::error_code error;
    fs::create_symlink(workspace / "real.txt", workspace / "linked.txt", error);
    if (error) {
        fs::remove_all(root);
        return;
    }
    fs::create_symlink("linked.txt", workspace / "chained.txt", error);
    if (error) {
        fs::remove_all(root);
        return;
    }

    auto matcher = makePlatformGitIgnoreMatcher(workspace);
    auto result = buildWorkspaceFileIndex(workspace, *matcher);
    const auto linked = std::find_if(
        result.candidates.begin(), result.candidates.end(),
        [](const PaletteCandidate& candidate) {
            return candidate.id == "linked.txt";
        });
    ASSERT_TRUE(linked != result.candidates.end());
    ASSERT_TRUE(std::any_of(
        result.candidates.begin(), result.candidates.end(),
        [](const PaletteCandidate& candidate) {
            return candidate.id == "chained.txt";
        }));

    auto created = createEditor(
        {workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (created.accepted() && linked != result.candidates.end()) {
        ASSERT_TRUE(
            ssg::test::openFile(*created.session, linked->id).accepted());
        ASSERT_EQ(ssg::test::activeDocumentText(*created.session), std::string{"linked\n"});
    }
    fs::remove_all(root);
}

TEST(indexSkipsSymlinkCyclesWithoutDroppingRegularFiles) {
    auto root = makeUniqueRoot("index-symlink-cycle");
    writeFile(root / "kept.txt", "kept\n");
    std::error_code error;
    fs::create_symlink("second", root / "first", error);
    if (!error) fs::create_symlink("first", root / "second", error);
    if (error) {
        fs::remove_all(root);
        return;
    }

    auto matcher = makePlatformGitIgnoreMatcher(root);
    const auto result = buildWorkspaceFileIndex(root, *matcher);
    ASSERT_EQ(ids(result), (std::vector<std::string>{"kept.txt"}));
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_workspace_file_index) {
    RUN(indexListsEveryTrackedFileAndNoDirectories);
    RUN(indexCandidateCarriesPathAsIdFilenameAsLabelAndParentAsDetail);
    RUN(indexWithoutGitignoreFilteringReappearsTheIgnoredSubtree);
    RUN(indexNeverDescendsIntoAnIgnoredDirectory);
    RUN(indexNeverOffersTheGitDirectory);
    RUN(indexTruncatesDeterministicallyAtTheCap);
    RUN(indexOutsideAGitRepositoryListsEverything);
    RUN(indexDoesNotDescendIntoSymlinkedDirectories);
    RUN(indexOffersSymlinkedFilesThatTheEditorCanOpen);
    RUN(indexSkipsSymlinkCyclesWithoutDroppingRegularFiles);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
