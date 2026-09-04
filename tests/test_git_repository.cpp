#include <ssg/GitDiffSource.h>
#include "test_helpers.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

std::string run(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " 2>/dev/null";
    std::array<char, 4096> buffer{};
    std::string output;
    auto* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        return output;
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    (void)pclose(pipe);
    return output;
}

int runStatus(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " >/dev/null 2>&1";
    return std::system(full.c_str());
}

std::string trim(std::string text) {
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::set<std::string> porcelainCurrentPaths(const fs::path& root) {
    std::set<std::string> paths;
    std::istringstream input{run(root, "status --porcelain")};
    for (std::string line; std::getline(input, line);) {
        if (line.size() < 4) {
            continue;
        }
        auto payload = line.substr(3);
        auto arrow = payload.find(" -> ");
        if (arrow != std::string::npos) {
            paths.insert(payload.substr(arrow + 4));
        } else {
            paths.insert(payload);
        }
    }
    return paths;
}

std::set<std::string> scanCurrentPaths(const GitDiffScan& scan) {
    std::set<std::string> paths;
    for (const auto& file : scan.files) {
        paths.insert(file.path.generic_string());
    }
    return paths;
}

std::set<fs::path> metadataDirectories(GitRepository& repository) {
    const auto directories = repository.metadataDirectories();
    return {directories.begin(), directories.end()};
}

DiffFileStatus statusFromPorcelainLine(std::string_view line) {
    if (line.size() >= 2 && line[0] == '?' && line[1] == '?') {
        return DiffFileStatus::Added;
    }
    const bool deleted =
        (line.size() >= 2) && (line[0] == 'D' || line[1] == 'D');
    if (deleted) {
        return DiffFileStatus::Deleted;
    }
    const bool added =
        (line.size() >= 2) && (line[0] == 'A' || line[1] == 'A');
    if (added) {
        return DiffFileStatus::Added;
    }
    const bool renamed =
        (line.size() >= 2) && (line[0] == 'R' || line[1] == 'R');
    if (renamed) {
        return DiffFileStatus::Renamed;
    }
    return DiffFileStatus::Modified;
}

std::map<std::string, DiffFileStatus> porcelainStatuses(const fs::path& root) {
    std::map<std::string, DiffFileStatus> statuses;
    std::istringstream input{run(root, "status --porcelain=v1")};
    for (std::string line; std::getline(input, line);) {
        if (line.size() < 4) {
            continue;
        }
        auto payload = line.substr(3);
        auto arrow = payload.find(" -> ");
        if (arrow != std::string::npos) {
            payload = payload.substr(arrow + 4);
        }
        statuses.emplace(std::move(payload), statusFromPorcelainLine(line));
    }
    return statuses;
}

std::map<std::string, DiffFileStatus> modelStatuses(const GitDiffScan& scan) {
    DiffModel model;
    std::uint64_t revisionValue = 1;
    for (const auto& file : scan.files) {
        auto result = model.updateGitFile(
            file, scan.baselineIdentity, std::uint64_t{revisionValue++});
        ASSERT_TRUE(result.accepted());
    }

    std::map<std::string, DiffFileStatus> statuses;
    for (const auto& file : model.viewState().files) {
        auto maybe = model.file(file.id);
        if (!maybe.has_value()) {
            return {};
        }
        statuses.emplace(maybe->get().path.generic_string(), maybe->get().status);
    }
    return statuses;
}

void assertStatusMatches(const fs::path& root,
                         GitRepository& repository,
                         const std::string& expectedPath) {
    auto scan = repository.scanDiff({});
    ASSERT_TRUE(scan.complete);

    const auto porcelain = porcelainStatuses(root);
    const auto actual = modelStatuses(scan);
    ASSERT_EQ(actual, porcelain);
    ASSERT_TRUE(actual.contains(expectedPath));
    ASSERT_EQ(actual.at(expectedPath), porcelain.at(expectedPath));
}

TEST(platformRepositoryMatchesGitStatusAcrossWorkflow) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-repository-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream{root / "a.txt"} << "a0\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add a.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto repository = makePlatformGitRepository(root);
    auto initial = repository->scanDiff({});
    ASSERT_TRUE(initial.complete);
    ASSERT_TRUE(initial.files.empty());

    std::ofstream{root / "a.txt"} << "a1\n";
    auto modified = repository->scanDiff({});
    ASSERT_TRUE(modified.complete);
    ASSERT_EQ(scanCurrentPaths(modified), porcelainCurrentPaths(root));
    ASSERT_EQ(modified.files.size(), std::size_t{1});
    if (!modified.files.empty()) {
        ASSERT_EQ(modified.files.front().baselineContent, std::optional<std::string>{"a0\n"});
        ASSERT_EQ(modified.files.front().workingContent, std::optional<std::string>{"a1\n"});
    }

    std::ofstream{root / ".gitignore"} << "*.tmp\n";
    std::ofstream{root / "ignored.tmp"} << "x\n";
    auto ignored = repository->scanDiff({});
    ASSERT_TRUE(ignored.complete);
    ASSERT_EQ(scanCurrentPaths(ignored), porcelainCurrentPaths(root));
    ASSERT_FALSE(scanCurrentPaths(ignored).contains("ignored.tmp"));

    ASSERT_EQ(runStatus(root, "add -A"), 0);
    ASSERT_EQ(runStatus(root, "commit -m prepare-rename"), 0);

    ASSERT_EQ(runStatus(root, "mv a.txt renamed.txt"), 0);
    auto renamed = repository->scanDiff({});
    ASSERT_TRUE(renamed.complete);
    ASSERT_EQ(scanCurrentPaths(renamed), porcelainCurrentPaths(root));
    bool sawRename = false;
    for (const auto& file : renamed.files) {
        if (file.path == fs::path{"renamed.txt"}) {
            sawRename = file.previousPath == std::optional<fs::path>{"a.txt"};
        }

    }
    ASSERT_TRUE(sawRename);

    auto identityBeforeCommit = renamed.baselineIdentity;
    ASSERT_EQ(runStatus(root, "commit -m rename"), 0);
    auto afterCommit = repository->scanDiff({});
    ASSERT_TRUE(afterCommit.complete);
    ASSERT_TRUE(afterCommit.files.empty());
    ASSERT_NE(afterCommit.baselineIdentity, identityBeforeCommit);

    ASSERT_EQ(runStatus(root, "rm renamed.txt"), 0);
    auto deleted = repository->scanDiff({});
    ASSERT_TRUE(deleted.complete);
    ASSERT_EQ(scanCurrentPaths(deleted), porcelainCurrentPaths(root));
    bool sawDeleted = false;
    for (const auto& file : deleted.files) {
        if (file.path == fs::path{"renamed.txt"}) {
            sawDeleted = !file.workingContent.has_value();
        }
    }
    ASSERT_TRUE(sawDeleted);

    ASSERT_EQ(runStatus(root, "checkout HEAD -- renamed.txt"), 0);
    auto restored = repository->scanDiff({});
    ASSERT_TRUE(restored.complete);
    ASSERT_EQ(scanCurrentPaths(restored), porcelainCurrentPaths(root));
    fs::remove_all(root);
}

TEST(platformRepositoryResolvesNormalNestedAndLinkedWorktreeMetadata) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        fs::temp_directory_path() / ("ssg-git-metadata-" + uniqueSuffix);
    const auto nested = root / "nested" / "workspace";
    const auto linked = root.parent_path() / ("ssg-git-linked-" + uniqueSuffix);
    fs::remove_all(root);
    fs::remove_all(linked);
    fs::create_directories(nested);
    std::ofstream{root / "tracked.txt"} << "base\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto rootRepository = makePlatformGitRepository(root);
    const auto rootDirectories = metadataDirectories(*rootRepository);
    ASSERT_FALSE(rootDirectories.empty());
    ASSERT_TRUE(rootDirectories.contains(fs::canonical(root / ".git")));

    auto nestedRepository = makePlatformGitRepository(nested);
    ASSERT_EQ(metadataDirectories(*nestedRepository), rootDirectories);

    ASSERT_EQ(runStatus(root, "worktree add -b linked-branch \"" +
                              linked.string() + "\""),
              0);
    auto linkedRepository = makePlatformGitRepository(linked);
    const auto linkedDirectories = metadataDirectories(*linkedRepository);
    ASSERT_TRUE(linkedDirectories.contains(
        fs::canonical(root / ".git" / "worktrees" / linked.filename())));
    ASSERT_TRUE(linkedDirectories.contains(fs::canonical(root / ".git")));
    ASSERT_NE(linkedDirectories, rootDirectories);
    fs::remove_all(root);
    fs::remove_all(linked);
}

TEST(platformRepositoryStatusClassificationMatchesGitPorcelain) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root =
        fs::temp_directory_path() / ("ssg-git-status-classification-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);

    std::ofstream{root / "tracked.txt"} << "base\n";
    std::ofstream{root / "rename-me.txt"} << "original\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt rename-me.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto repository = makePlatformGitRepository(root);

    std::ofstream{root / "tracked.txt"} << "changed\n";
    assertStatusMatches(root, *repository, "tracked.txt");
    ASSERT_EQ(porcelainStatuses(root).at("tracked.txt"), DiffFileStatus::Modified);
    ASSERT_EQ(runStatus(root, "reset --hard HEAD"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);

    std::ofstream{root / "added.txt"} << "added\n";
    assertStatusMatches(root, *repository, "added.txt");
    ASSERT_EQ(porcelainStatuses(root).at("added.txt"), DiffFileStatus::Added);
    ASSERT_EQ(runStatus(root, "reset --hard HEAD"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);

    ASSERT_EQ(runStatus(root, "rm tracked.txt"), 0);
    assertStatusMatches(root, *repository, "tracked.txt");
    ASSERT_EQ(porcelainStatuses(root).at("tracked.txt"), DiffFileStatus::Deleted);
    ASSERT_EQ(runStatus(root, "reset --hard HEAD"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);

    ASSERT_EQ(runStatus(root, "mv rename-me.txt renamed.txt"), 0);
    assertStatusMatches(root, *repository, "renamed.txt");
    ASSERT_EQ(porcelainStatuses(root).at("renamed.txt"), DiffFileStatus::Renamed);
    ASSERT_EQ(runStatus(root, "reset --hard HEAD"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);

    ASSERT_EQ(runStatus(root, "mv rename-me.txt renamed.txt"), 0);
    std::ofstream{root / "renamed.txt"} << "changed-after-rename\n";
    ASSERT_EQ(runStatus(root, "add renamed.txt"), 0);
    assertStatusMatches(root, *repository, "renamed.txt");

    fs::remove_all(root);
}

TEST(platformRepositoryOpenFailureIsIncomplete) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto base = fs::temp_directory_path() / ("ssg-git-repository-openfail-" + uniqueSuffix);
    auto parent = base / "parent";
    auto root = parent / "repo";
    fs::remove_all(base);
    fs::create_directories(root);
    std::ofstream{root / "a.txt"} << "a0\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add a.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto repository = makePlatformGitRepository(root);
    std::ofstream{root / "a.txt"} << "a1\n";
    auto changed = repository->scanDiff({});
    ASSERT_TRUE(changed.complete);
    ASSERT_EQ(changed.files.size(), std::size_t{1});

    std::error_code error;
    fs::permissions(parent, fs::perms::none, fs::perm_options::replace,
                    error);
    ASSERT_FALSE(error);

    auto failedScan = repository->scanDiff({});
    fs::permissions(parent, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    ASSERT_FALSE(error);

    ASSERT_FALSE(failedScan.complete);
    fs::remove_all(base);
}

TEST(platformRepositoryCurrentBranchMatchesGitBranchAndDetachedHead) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-branch-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream{root / "a.txt"} << "a0\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add a.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto repository = makePlatformGitRepository(root);
    auto named = repository->currentBranch();
    ASSERT_TRUE(named.has_value());
    ASSERT_EQ(named.value_or(""), trim(run(root, "branch --show-current")));

    ASSERT_EQ(runStatus(root, "checkout --detach"), 0);
    auto detached = repository->currentBranch();
    ASSERT_TRUE(detached.has_value());
    ASSERT_EQ(detached.value_or(""), trim(run(root, "rev-parse --short HEAD")));

    fs::remove_all(root);
}

TEST(platformRepositoryCurrentBranchIsAbsentOutsideGitRepo) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-non-git-branch-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream{root / "plain.txt"} << "x\n";

    auto repository = makePlatformGitRepository(root);
    ASSERT_FALSE(repository->currentBranch().has_value());
    fs::remove_all(root);
}

// Reference-implementation oracle: real `git check-ignore` decides, and the
// matcher must agree on every probe.  A hand-listed expectation would encode my
// reading of gitignore semantics; git itself encodes the real ones.
int gitSaysIgnored(const fs::path& repoRoot, std::string_view repoRelative) {
    auto command = "check-ignore -q \"" + std::string{repoRelative} + "\"";
    return runStatus(repoRoot, command) == 0 ? 1 : 0;
}

fs::path makeUniqueRoot(std::string_view label) {
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return fs::temp_directory_path() /
           ("ssg-" + std::string{label} + "-" + suffix);
}

TEST(ignoreMatcherAgreesWithGitCheckIgnoreAtTheRepositoryRoot) {
    auto root = makeUniqueRoot("ignore-root");
    fs::create_directories(root / "src");
    fs::create_directories(root / "build" / "nested");
    std::ofstream{root / ".gitignore"} << "build/\n*.log\n";
    std::ofstream{root / "src" / "main.cpp"} << "int main(){}\n";
    std::ofstream{root / "build" / "nested" / "out.o"} << "x\n";
    std::ofstream{root / "debug.log"} << "x\n";
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto matcher = makePlatformGitIgnoreMatcher(root);
    ASSERT_TRUE(matcher->usable());

    for (std::string_view probe :
         {"src/main.cpp", "build", "build/nested/out.o", "debug.log",
          ".gitignore"}) {
        ASSERT_EQ(matcher->ignores(fs::path{probe}) ? 1 : 0,
                  gitSaysIgnored(root, probe));
    }
    fs::remove_all(root);
}

// The workspace root need not be the repository root.  The matcher takes
// WORKSPACE-relative paths, so it must rebase them onto the work directory
// before asking libgit2; without that a root-anchored pattern silently matches
// against the wrong prefix and the answers look plausible but are wrong.
TEST(ignoreMatcherRebasesWhenTheWorkspaceIsNestedInsideTheRepository) {
    auto root = makeUniqueRoot("ignore-nested");
    fs::create_directories(root / "sub" / "build");
    fs::create_directories(root / "sub" / "keep");
    // Anchored at the REPOSITORY root: it must NOT match sub/build, and it must
    // match the top-level build.  This is the pattern that exposes a missing
    // rebase in either direction.
    std::ofstream{root / ".gitignore"} << "/build/\nsub/ignored.txt\n";
    fs::create_directories(root / "build");
    std::ofstream{root / "sub" / "build" / "kept.txt"} << "x\n";
    std::ofstream{root / "sub" / "keep" / "file.txt"} << "x\n";
    std::ofstream{root / "sub" / "ignored.txt"} << "x\n";
    ASSERT_EQ(runStatus(root, "init -q"), 0);

    auto workspace = root / "sub";
    auto matcher = makePlatformGitIgnoreMatcher(workspace);
    ASSERT_TRUE(matcher->usable());

    // Probes are workspace-relative; git is asked the repository-relative form.
    ASSERT_EQ(matcher->ignores(fs::path{"ignored.txt"}) ? 1 : 0,
              gitSaysIgnored(root, "sub/ignored.txt"));
    ASSERT_EQ(matcher->ignores(fs::path{"build/kept.txt"}) ? 1 : 0,
              gitSaysIgnored(root, "sub/build/kept.txt"));
    ASSERT_EQ(matcher->ignores(fs::path{"keep/file.txt"}) ? 1 : 0,
              gitSaysIgnored(root, "sub/keep/file.txt"));
    // Concretely: the root-anchored /build/ rule does not reach the nested one.
    ASSERT_FALSE(matcher->ignores(fs::path{"build/kept.txt"}));
    ASSERT_TRUE(matcher->ignores(fs::path{"ignored.txt"}));
    fs::remove_all(root);
}

TEST(ignoreMatcherIsUnusableAndNeverIgnoresOutsideARepository) {
    auto root = makeUniqueRoot("ignore-norepo");
    fs::create_directories(root);
    std::ofstream{root / "plain.txt"} << "x\n";

    auto matcher = makePlatformGitIgnoreMatcher(root);
    ASSERT_FALSE(matcher->usable());
    ASSERT_FALSE(matcher->ignores(fs::path{"plain.txt"}));
    ASSERT_FALSE(matcher->ignores(fs::path{"anything/at/all"}));
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_git_repository) {
    RUN(platformRepositoryMatchesGitStatusAcrossWorkflow);
    RUN(platformRepositoryResolvesNormalNestedAndLinkedWorktreeMetadata);
    RUN(platformRepositoryStatusClassificationMatchesGitPorcelain);
    RUN(platformRepositoryOpenFailureIsIncomplete);
    RUN(platformRepositoryCurrentBranchMatchesGitBranchAndDetachedHead);
    RUN(platformRepositoryCurrentBranchIsAbsentOutsideGitRepo);
    RUN(ignoreMatcherAgreesWithGitCheckIgnoreAtTheRepositoryRoot);
    RUN(ignoreMatcherRebasesWhenTheWorkspaceIsNestedInsideTheRepository);
    RUN(ignoreMatcherIsUnusableAndNeverIgnoresOutsideARepository);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
