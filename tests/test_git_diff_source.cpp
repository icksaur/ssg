#include "ssg/GitDiffSource.h"
#include "test_helpers.h"

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ssg;

struct PathScanExpectation {
    std::vector<std::filesystem::path> requested;
    GitWorkingTreeScan result;
};

class FakeRepository final : public GitRepository {
public:
    std::deque<GitDiffScan> fullScans;
    std::deque<PathScanExpectation> pathScans;
    std::optional<std::string> branch;
    std::size_t fullCalls = 0;
    std::size_t pathCalls = 0;

    GitDiffScan scanDiff(const GitDiffConfig&) override {
        ++fullCalls;
        if (fullScans.empty()) {
            throw std::logic_error("missing full scan");
        }
        auto result = fullScans.front();
        fullScans.pop_front();
        return result;
    }

    GitWorkingTreeScan scanPaths(const std::vector<std::filesystem::path>& paths,
                                 const GitDiffConfig&) override {
        ++pathCalls;
        if (pathScans.empty()) {
            throw std::logic_error("missing path scan");
        }
        auto expected = pathScans.front();
        pathScans.pop_front();
        ASSERT_EQ(paths, expected.requested);
        return expected.result;
    }

    std::optional<std::string> currentBranch() override { return branch; }
};

using OracleState = std::map<std::string, GitDiffScanFile>;

OracleState applyFullOracle(const GitDiffScan& scan) {
    OracleState state;
    for (const auto& file : scan.files) {
        state.insert_or_assign(file.id.value(), file);
    }
    return state;
}

void applyPathOracle(OracleState& state, const GitWorkingTreeScan& scan) {
    std::set<std::string> present;
    for (const auto& file : scan.files) {
        state.insert_or_assign(file.id.value(), file);
        present.insert(file.id.value());
        if (file.previousPath) {
            state.erase(file.previousPath->generic_string());
        }
    }
    for (const auto& path : scan.requestedPaths) {
        auto id = path.generic_string();
        if (present.contains(id)) {
            continue;
        }
        state.erase(id);
    }
}

void assertMatchesModel(const OracleState& expected, const DiffModel& model) {
    auto view = model.viewState();
    ASSERT_EQ(view.files.size(), expected.size());
    for (const auto& file : view.files) {
        auto it = expected.find(file.id.value());
        ASSERT_TRUE(it != expected.end());
        if (it == expected.end()) {
            continue;
        }
        ASSERT_EQ(file.path, it->second.path);
        ASSERT_EQ(file.previousPath, it->second.previousPath);
        ASSERT_EQ(file.deleted, !it->second.workingContent.has_value());
    }
}

TEST(referenceReconcileMatchesScriptedScans) {
    DiffModel model;
    GitDiffSource source{model};
    FakeRepository repository;

    GitDiffScan full1{
        .baselineIdentity = "base-1",
        .files =
            {
                {.id = DiffFileId{"a.cpp"},
                 .path = "a.cpp",
                 .baselineContent = "a\n",
                 .workingContent = "aa\n"},
                {.id = DiffFileId{"renamed.cpp"},
                 .path = "renamed.cpp",
                 .previousPath = std::filesystem::path{"old.cpp"},
                 .baselineContent = "x\n",
                 .workingContent = "xx\n"},
            },
        .complete = true,
    };
    repository.fullScans.push_back(full1);
    auto first = source.refresh(repository);
    ASSERT_TRUE(first.accepted);
    ASSERT_TRUE(first.applied);
    auto oracle = applyFullOracle(full1);
    assertMatchesModel(oracle, model);

    GitWorkingTreeScan removeA{
        .baselineIdentity = "base-1",
        .requestedPaths = {std::filesystem::path{"a.cpp"}},
        .files = {},
        .complete = true,
    };
    repository.pathScans.push_back({removeA.requestedPaths, removeA});
    auto removed = source.refreshPaths(repository, removeA.requestedPaths);
    ASSERT_TRUE(removed.accepted);
    ASSERT_TRUE(removed.applied);
    applyPathOracle(oracle, removeA);
    assertMatchesModel(oracle, model);

    GitWorkingTreeScan renameHalf{
        .baselineIdentity = "base-1",
        .requestedPaths = {std::filesystem::path{"renamed.cpp"}},
        .files =
            {
                {.id = DiffFileId{"renamed.cpp"},
                 .path = "renamed.cpp",
                 .previousPath = std::filesystem::path{"old.cpp"},
                 .baselineContent = "x\n",
                 .workingContent = "xxx\n"},
            },
        .complete = true,
    };
    GitDiffScan fullAfterRename{
        .baselineIdentity = "base-1",
        .files =
            {
                {.id = DiffFileId{"renamed.cpp"},
                 .path = "renamed.cpp",
                 .previousPath = std::filesystem::path{"old.cpp"},
                 .baselineContent = "x\n",
                 .workingContent = "xxx\n"},
                {.id = DiffFileId{"c.cpp"},
                 .path = "c.cpp",
                 .baselineContent = "c\n",
                 .workingContent = "cc\n"},
            },
        .complete = true,
    };
    repository.pathScans.push_back({renameHalf.requestedPaths, renameHalf});
    repository.fullScans.push_back(fullAfterRename);
    auto renameEscalated = source.refreshPaths(repository, renameHalf.requestedPaths);
    ASSERT_TRUE(renameEscalated.accepted);
    ASSERT_TRUE(renameEscalated.applied);
    oracle = applyFullOracle(fullAfterRename);
    assertMatchesModel(oracle, model);

    GitWorkingTreeScan identityChangedPath{
        .baselineIdentity = "base-2",
        .requestedPaths = {std::filesystem::path{"renamed.cpp"}},
        .files = {},
        .complete = true,
    };
    GitDiffScan fullAfterIdentity{
        .baselineIdentity = "base-2",
        .files =
            {
                {.id = DiffFileId{"d.cpp"},
                 .path = "d.cpp",
                 .baselineContent = "d\n",
                 .workingContent = "dd\n"},
            },
        .complete = true,
    };
    repository.pathScans.push_back(
        {identityChangedPath.requestedPaths, identityChangedPath});
    repository.fullScans.push_back(fullAfterIdentity);
    auto identityEscalated =
        source.refreshPaths(repository, identityChangedPath.requestedPaths);
    ASSERT_TRUE(identityEscalated.accepted);
    ASSERT_TRUE(identityEscalated.applied);
    oracle = applyFullOracle(fullAfterIdentity);
    assertMatchesModel(oracle, model);

    auto beforeIncomplete = model.viewState();
    GitDiffScan incomplete{
        .baselineIdentity = "base-2",
        .files =
            {
                {.id = DiffFileId{"ignored.cpp"},
                 .path = "ignored.cpp",
                 .baselineContent = "i\n",
                 .workingContent = "ii\n"},
            },
        .complete = false,
    };
    repository.fullScans.push_back(incomplete);
    auto noPublish = source.refresh(repository);
    ASSERT_TRUE(noPublish.accepted);
    ASSERT_FALSE(noPublish.applied);
    ASSERT_TRUE(noPublish.shouldRetry());
    ASSERT_EQ(model.viewState(), beforeIncomplete);
}

TEST(fullScanRejectsAtomicallyWithoutPartialPublication) {
    DiffModel model;
    GitDiffSource source{model};
    FakeRepository repository;

    GitDiffScan seed{
        .baselineIdentity = "base-1",
        .files = {
            {.id = DiffFileId{"seed.cpp"},
             .path = "seed.cpp",
             .baselineContent = "s\n",
             .workingContent = "ss\n"},
        },
        .complete = true,
    };
    repository.fullScans.push_back(seed);
    auto seeded = source.refresh(repository);
    ASSERT_TRUE(seeded.accepted);
    ASSERT_TRUE(seeded.applied);
    auto before = model.viewState();

    GitDiffScan invalid{
        .baselineIdentity = "base-2",
        .files = {
            {.id = DiffFileId{"good.cpp"},
             .path = "good.cpp",
             .baselineContent = "g\n",
             .workingContent = "gg\n"},
            {.id = DiffFileId{"bad.cpp"},
             .path = "../bad.cpp",
             .baselineContent = "b\n",
             .workingContent = "bb\n"},
        },
        .complete = true,
    };
    repository.fullScans.push_back(invalid);
    auto rejected = source.refresh(repository);
    ASSERT_FALSE(rejected.accepted);
    ASSERT_FALSE(rejected.applied);
    ASSERT_TRUE(rejected.shouldRetry());
    ASSERT_EQ(model.viewState(), before);
}

TEST(incompleteScanKeepsPublishedDiffSet) {
    DiffModel model;
    GitDiffSource source{model};
    FakeRepository repository;

    GitDiffScan first{
        .baselineIdentity = "base-1",
        .files = {
            {.id = DiffFileId{"a.cpp"},
             .path = "a.cpp",
             .baselineContent = "a\n",
             .workingContent = "aa\n"},
        },
        .complete = true,
    };
    repository.fullScans.push_back(first);
    auto initial = source.refresh(repository);
    ASSERT_TRUE(initial.accepted);
    ASSERT_TRUE(initial.applied);
    auto published = model.viewState();

    GitDiffScan openFailure{
        .baselineIdentity = "base-1",
        .files = {},
        .complete = false,
    };
    repository.fullScans.push_back(openFailure);
    auto retry = source.refresh(repository);
    ASSERT_TRUE(retry.accepted);
    ASSERT_FALSE(retry.applied);
    ASSERT_TRUE(retry.shouldRetry());
    ASSERT_EQ(model.viewState(), published);
}

}  // namespace

int main() {
    RUN(referenceReconcileMatchesScriptedScans);
    RUN(fullScanRejectsAtomicallyWithoutPartialPublication);
    RUN(incompleteScanKeepsPublishedDiffSet);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
