#include <ssg/FollowEditsModel.h>
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ssg;

DiffFileView changedFile(std::string id, std::filesystem::path path,
                          std::size_t newestLine, bool deleted = false) {
    DiffFileView view{DiffFileId{std::move(id)}};
    view.path = std::move(path);
    view.deleted = deleted;
    for (std::size_t line = 0; line < newestLine; ++line) {
        view.currentContent += "line\n";
    }
    if (!deleted) view.currentContent += "new\n";
    view.hunks.push_back(
        {.baselineStart = newestLine,
         .targetStart = newestLine,
         .baselineLines = {"old\n"},
         .targetLines = deleted ? std::vector<std::string>{}
                                 : std::vector<std::string>{"new\n"}});
    return view;
}

TEST(newestIntroducedHunkWinsWhenPriorBottomHunkRemains) {
    auto revisionA = changedFile("file", "file.txt", 0);
    revisionA.currentContent = "top\nsame\nmiddle\nsame\nbottom\n";
    revisionA.hunks = {
        {.baselineStart = 0,
         .targetStart = 0,
         .baselineLines = {"old top\n"},
         .targetLines = {"top\n"}},
        {.baselineStart = 4,
         .targetStart = 4,
         .baselineLines = {"old bottom\n"},
         .targetLines = {"bottom\n"}}};
    auto revisionB = revisionA;
    revisionB.hunks.insert(
        revisionB.hunks.begin() + 1,
        {.baselineStart = 2,
         .targetStart = 2,
         .baselineLines = {"old middle\n"},
         .targetLines = {"middle\n"}});

    FollowEditsModel model;
    ASSERT_TRUE(model
                    .acceptExternalChanges(
                        {{revisionB, revisionA.hunks, std::uint64_t{2}}})
                    .accepted());
    ASSERT_TRUE(model.viewState().activeTarget.has_value());
    ASSERT_EQ(model.viewState().activeTarget->newestHunkLine, std::size_t{2});
}

TEST(burstActivatesOnlyLastFileAndAdvancesOnce) {
    FollowEditsModel model;
    const auto before = model.viewState().generation;
    ASSERT_TRUE(model
                    .acceptExternalChanges(
                        {{changedFile("a", "a.txt", 2), {}, std::uint64_t{1}},
                         {changedFile("b", "b.txt", 5), {}, std::uint64_t{2}},
                         {changedFile("c", "c.txt", 8), {}, std::uint64_t{3}}})
                    .accepted());
    const auto state = model.viewState();
    ASSERT_TRUE(state.activeTarget.has_value());
    ASSERT_EQ(state.activeTarget->id, DiffFileId{"c"});
    ASSERT_EQ(state.generation, before + 1);
}

TEST(burstDoesNotRevealEarlierFileWhenLastFileHasNoNewHunk) {
    FollowEditsModel model;
    auto unchanged = changedFile("b", "b.txt", 5);
    ASSERT_TRUE(model
                    .acceptExternalChanges(
                        {{changedFile("a", "a.txt", 2), {}, std::uint64_t{1}},
                         {unchanged, unchanged.hunks, std::uint64_t{2}}})
                    .accepted());
    const auto state = model.viewState();
    ASSERT_FALSE(state.activeTarget.has_value());
    ASSERT_EQ(state.queuedTargets.size(), std::size_t{1});
    ASSERT_EQ(state.queuedTargets.front().id, DiffFileId{"a"});
}

TEST(programmaticRevealDoesNotPauseButUserNavigationDoes) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .applyNavigation(
                        {.classification = NavigationClass::Programmatic})
                    .accepted());
    ASSERT_EQ(model.viewState().mode, FollowMode::Following);
    ASSERT_TRUE(model
                    .applyNavigation({.classification = NavigationClass::User})
                    .accepted());
    ASSERT_EQ(model.viewState().mode, FollowMode::Paused);
}

TEST(localEditPausesOnlyWhenFollowing) {
    FollowEditsModel model;
    const auto before = model.viewState();
    ASSERT_TRUE(model.notifyLocalEdit().accepted());
    const auto paused = model.viewState();
    ASSERT_EQ(paused.mode, FollowMode::Paused);
    ASSERT_EQ(paused.generation, before.generation + 1);

    ASSERT_TRUE(model.notifyLocalEdit().accepted());
    const auto stillPaused = model.viewState();
    ASSERT_EQ(stillPaused.mode, FollowMode::Paused);
    ASSERT_EQ(stillPaused.generation, paused.generation);
}

DiffViewState currentDiff(std::initializer_list<DiffFileView> files,
                           std::uint64_t revision) {
    return {std::uint64_t{revision}, files};
}

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> fields;
    std::istringstream input{std::string{line}};
    for (std::string field; std::getline(input, field, '\t');) {
        fields.push_back(std::move(field));
    }
    return fields;
}

std::string targetId(const std::optional<FollowTarget>& target) {
    return target ? target->id.value() : "-";
}

std::string queueIds(const FollowEditsViewState& state) {
    if (state.queuedTargets.empty()) {
        return "-";
    }
    std::string result;
    for (const auto& target : state.queuedTargets) {
        if (!result.empty()) {
            result += ",";
        }
        result += target.id.value();
    }
    return result;
}

TEST(independentTransitionTableCoversSharedFollowPolicy) {
    std::ifstream input{
        std::filesystem::path{SSG_FOLLOW_EDITS_FIXTURE_DIR} / "transitions.tsv"};
    ASSERT_TRUE(input.good());

    FollowEditsModel model{{.queueCapacity = 4,
                            .resumeBinding = "Ctrl+Shift+F"}};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = split(line);
        ASSERT_EQ(fields.size(), std::size_t{6});
        const auto& operation = fields[0];
        if (operation == "change") {
            const auto separator = fields[1].find(':');
            const auto id = fields[1].substr(0, separator);
            const auto lineNumber =
                std::stoull(fields[1].substr(separator + 1));
            ASSERT_TRUE(model
                            .acceptExternalChange(
                                changedFile(id, id + ".txt", lineNumber),
                                std::uint64_t{std::stoull(fields[2])})
                            .accepted());
        } else if (operation == "navigate_user" ||
                   operation == "navigate_programmatic" ||
                   operation == "navigate_non_navigation") {
            auto classification = NavigationClass::User;
            if (operation == "navigate_programmatic") {
                classification = NavigationClass::Programmatic;
            } else if (operation == "navigate_non_navigation") {
                classification = NavigationClass::NonNavigation;
            }
            ASSERT_TRUE(model
                            .applyNavigation({.classification = classification})
                            .accepted());
        } else if (operation == "resume") {
            ASSERT_TRUE(model
                            .resume(currentDiff(
                                {changedFile("a", "a.txt", 20),
                                 changedFile("b", "b.txt", 30)},
                                3))
                            .accepted());
        } else if (operation == "pause") {
            ASSERT_TRUE(model.pause().accepted());
        } else {
            throw std::runtime_error{"unknown transition operation"};
        }

        const auto state = model.viewState();
        ASSERT_EQ(state.mode == FollowMode::Following ? "following" : "paused",
                  fields[3]);
        ASSERT_EQ(targetId(state.activeTarget), fields[4]);
        ASSERT_EQ(queueIds(state), fields[5]);
    }
}

TEST(dirtyConflictUsesDiskDiffTargetWithoutBufferPolicy) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .acceptExternalChange(
                        changedFile("dirty", "dirty.txt", 6), std::uint64_t{1})
                    .accepted());
    ASSERT_EQ(targetId(model.viewState().activeTarget), "dirty");
}

TEST(queueIsBoundedAndSameFileReplacesInPlace) {
    FollowEditsModel model{{.queueCapacity = 2,
                            .resumeBinding = "Ctrl+Shift+F"}};
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "a", 1),
                                            std::uint64_t{1})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("b", "b", 2),
                                            std::uint64_t{2})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "renamed-a", 9),
                                            std::uint64_t{3})
                    .accepted());

    const auto state = model.viewState();
    ASSERT_EQ(queueIds(state), "b,a");
    ASSERT_EQ(state.queuedTargets.back().path,
              std::filesystem::path{"renamed-a"});

    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("c", "c", 4),
                                            std::uint64_t{4})
                    .accepted());
    ASSERT_EQ(queueIds(model.viewState()), "a,c");
}

TEST(resumeResolvesRenameDeleteAndSkipsRevertedOrMissingTargets) {
    FollowEditsModel model;
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("rename", "old", 1),
                                            std::uint64_t{1})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("revert", "revert", 2),
                                            std::uint64_t{2})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("delete", "gone", 3),
                                            std::uint64_t{3})
                    .accepted());

    auto renamed = changedFile("rename", "new", 7);
    renamed.previousPath = "old";
    auto deleted = changedFile("delete", "gone", 8, true);
    deleted.previousPath = "gone";
    ASSERT_TRUE(
        model.resume(currentDiff({renamed, deleted}, 4)).accepted());
    ASSERT_EQ(targetId(model.viewState().activeTarget), "delete");
    ASSERT_TRUE(model.viewState().activeTarget->deleted);

    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("missing", "x", 1),
                                            std::uint64_t{4})
                    .accepted());
    const auto before = model.viewState();
    ASSERT_TRUE(model.resume(currentDiff({}, 5)).accepted());
    const auto after = model.viewState();
    ASSERT_EQ(after.mode, FollowMode::Following);
    ASSERT_EQ(after.activeTarget, before.activeTarget);
    ASSERT_TRUE(after.queuedTargets.empty());
}

TEST(resumePreservesNewestIntroducedHunkInsteadOfChoosingBottomHunk) {
    auto prior = changedFile("file", "file.txt", 0);
    prior.currentContent = "top\nmiddle\nbottom\n";
    prior.hunks = {
        {.baselineStart = 0,
         .targetStart = 0,
         .baselineLines = {"old top\n"},
         .targetLines = {"top\n"}},
        {.baselineStart = 2,
         .targetStart = 2,
         .baselineLines = {"old bottom\n"},
         .targetLines = {"bottom\n"}}};
    auto current = prior;
    current.hunks.insert(
        current.hunks.begin() + 1,
        {.baselineStart = 1,
         .targetStart = 1,
         .baselineLines = {"old middle\n"},
         .targetLines = {"middle\n"}});

    FollowEditsModel model;
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChanges(
                        {{current, prior.hunks, std::uint64_t{1}}})
                    .accepted());
    ASSERT_TRUE(model.resume(currentDiff({current}, 2)).accepted());
    ASSERT_TRUE(model.viewState().activeTarget.has_value());
    ASSERT_EQ(model.viewState().activeTarget->newestHunkLine, std::size_t{1});
}

TEST(staleChangesAreFailureAtomic) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "a", 1),
                                            std::uint64_t{2})
                    .accepted());
    const auto before = model.viewState();
    ASSERT_EQ(model
                  .acceptExternalChange(changedFile("b", "b", 2),
                                          std::uint64_t{2})
                  .error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_TRUE(model.pause().accepted());
    const auto paused = model.viewState();
    ASSERT_EQ(model.resume(currentDiff({}, 1)).error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.viewState(), paused);
}

TEST(footerProjectionTracksPauseAndResume) {
    FollowEditsModel model{{.queueCapacity = 2,
                            .resumeBinding = "Ctrl+Shift+F"}};
    const auto followingFooter = model.footerProjection();
    ASSERT_EQ(followingFooter.mode, "following");
    ASSERT_EQ(followingFooter.resumeCommand,
              std::optional<std::string>{"follow_edits.toggle"});

    ASSERT_TRUE(model.pause().accepted());

    const auto footer = model.footerProjection();
    ASSERT_EQ(footer.mode, "paused");
    ASSERT_EQ(footer.resumeBinding, std::optional<std::string>{"Ctrl+Shift+F"});
    ASSERT_EQ(footer.resumeCommand,
              std::optional<std::string>{"follow_edits.toggle"});
}

TEST(configurationRejectsInvalidQueueCapacity) {
    ASSERT_THROWS(FollowEditsModel(
                      {.queueCapacity = 0,
                       .resumeBinding = "Ctrl+Shift+F"}),
                  std::invalid_argument);
}

}  // namespace

SSG_TEST_SUITE(test_follow_edits) {
    RUN(independentTransitionTableCoversSharedFollowPolicy);
    RUN(newestIntroducedHunkWinsWhenPriorBottomHunkRemains);
    RUN(burstActivatesOnlyLastFileAndAdvancesOnce);
    RUN(burstDoesNotRevealEarlierFileWhenLastFileHasNoNewHunk);
    RUN(programmaticRevealDoesNotPauseButUserNavigationDoes);
    RUN(localEditPausesOnlyWhenFollowing);
    RUN(dirtyConflictUsesDiskDiffTargetWithoutBufferPolicy);
    RUN(queueIsBoundedAndSameFileReplacesInPlace);
    RUN(resumeResolvesRenameDeleteAndSkipsRevertedOrMissingTargets);
    RUN(resumePreservesNewestIntroducedHunkInsteadOfChoosingBottomHunk);
    RUN(staleChangesAreFailureAtomic);
    RUN(footerProjectionTracksPauseAndResume);
    RUN(configurationRejectsInvalidQueueCapacity);
    return failed == 0 ? 0 : 1;
}
