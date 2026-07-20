#include "ssg/follow_edits.h"
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
    view.hunks.push_back(
        {.baselineStart = newestLine,
         .targetStart = newestLine,
         .baselineLines = {"old\n"},
         .targetLines = deleted ? std::vector<std::string>{}
                                 : std::vector<std::string>{"new\n"}});
    return view;
}

DiffViewState currentDiff(std::initializer_list<DiffFileView> files,
                           std::uint64_t revision) {
    return {Revision{revision}, files};
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

std::string rowFor(const FollowEditsViewState& state, std::uint64_t client) {
    for (const auto& view : state.clients) {
        if (view.client == ClientId{client}) {
            return std::to_string(view.offset.firstRow);
        }
    }
    return "-";
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
        ASSERT_EQ(fields.size(), std::size_t{8});
        const auto& operation = fields[0];
        if (operation == "attach") {
            ASSERT_TRUE(model.attachClient(
                                 ClientId{std::stoull(fields[1])},
                                 ViewportDimensions{80,
                                     static_cast<std::uint32_t>(
                                         std::stoul(fields[2]))})
                            .accepted());
        } else if (operation == "change") {
            const auto separator = fields[1].find(':');
            const auto id = fields[1].substr(0, separator);
            const auto lineNumber =
                std::stoull(fields[1].substr(separator + 1));
            ASSERT_TRUE(model
                            .acceptExternalChange(
                                changedFile(id, id + ".txt", lineNumber),
                                Revision{std::stoull(fields[2])})
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
                            .applyNavigation(
                                {.client = ClientId{std::stoull(fields[1])},
                                 .classification = classification,
                                 .offset = FollowScrollOffset{
                                     std::stoull(fields[2]), 0}})
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
        ASSERT_EQ(rowFor(state, 1), fields[6]);
        ASSERT_EQ(rowFor(state, 2), fields[7]);
    }
}

TEST(dirtyConflictUsesDiskDiffTargetWithoutBufferPolicy) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .acceptExternalChange(
                        changedFile("dirty", "dirty.txt", 6), Revision{1})
                    .accepted());
    ASSERT_EQ(targetId(model.viewState().activeTarget), "dirty");
}

TEST(queueIsBoundedAndSameFileReplacesInPlace) {
    FollowEditsModel model{{.queueCapacity = 2,
                            .resumeBinding = "Ctrl+Shift+F"}};
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "a", 1),
                                            Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("b", "b", 2),
                                            Revision{2})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "renamed-a", 9),
                                            Revision{3})
                    .accepted());

    const auto state = model.viewState();
    ASSERT_EQ(queueIds(state), "b,a");
    ASSERT_EQ(state.queuedTargets.back().path,
              std::filesystem::path{"renamed-a"});

    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("c", "c", 4),
                                            Revision{4})
                    .accepted());
    ASSERT_EQ(queueIds(model.viewState()), "a,c");
}

TEST(resumeResolvesRenameDeleteAndSkipsRevertedOrMissingTargets) {
    FollowEditsModel model;
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("rename", "old", 1),
                                            Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("revert", "revert", 2),
                                            Revision{2})
                    .accepted());
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("delete", "gone", 3),
                                            Revision{3})
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
                                            Revision{4})
                    .accepted());
    const auto before = model.viewState();
    ASSERT_TRUE(model.resume(currentDiff({}, 5)).accepted());
    const auto after = model.viewState();
    ASSERT_EQ(after.mode, FollowMode::Following);
    ASSERT_EQ(after.activeTarget, before.activeTarget);
    ASSERT_EQ(after.clients, before.clients);
    ASSERT_TRUE(after.queuedTargets.empty());
}

TEST(staleChangesAndInvalidClientsAreFailureAtomic) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .acceptExternalChange(changedFile("a", "a", 1),
                                            Revision{2})
                    .accepted());
    const auto before = model.viewState();
    ASSERT_EQ(model
                  .acceptExternalChange(changedFile("b", "b", 2),
                                          Revision{2})
                  .error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_EQ(model
                  .applyNavigation(
                      {.client = ClientId{99},
                       .classification = NavigationClass::User,
                       .offset = FollowScrollOffset{3, 0}})
                  .error,
              FollowEditsError::UnknownClient);
    ASSERT_EQ(model.viewState(), before);

    ASSERT_TRUE(model.pause().accepted());
    const auto paused = model.viewState();
    ASSERT_EQ(model.resume(currentDiff({}, 1)).error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.viewState(), paused);
}

TEST(commandViewDeltaAndFooterAreComplete) {
    const auto commands = followEditsCommandSet().descriptors();
    ASSERT_EQ(commands.size(), std::size_t{2});
    ASSERT_EQ(commands[0].id, "follow_edits.resume");
    ASSERT_EQ(commands[1].id, "follow_edits.pause");

    FollowEditsModel model{{.queueCapacity = 2,
                            .resumeBinding = "Ctrl+Shift+F"}};
    const auto before = model.viewState();
    ASSERT_TRUE(model.pause().accepted());
    const auto after = model.viewState();
    const auto delta = deriveFollowEditsDelta(before, after);
    ASSERT_EQ(delta.baseGeneration, before.generation);
    ASSERT_EQ(delta.generation, after.generation);
    ASSERT_EQ(delta.replacement, after);

    const auto footer = model.footerProjection();
    ASSERT_EQ(footer.mode, "paused");
    ASSERT_EQ(footer.resumeBinding, std::optional<std::string>{"Ctrl+Shift+F"});
    ASSERT_EQ(footer.resumeCommand,
              std::optional<std::string>{"follow_edits.resume"});
}

TEST(configurationRejectsInvalidQueueCapacity) {
    ASSERT_THROWS(FollowEditsModel(
                      {.queueCapacity = 0,
                       .resumeBinding = "Ctrl+Shift+F"}),
                  std::invalid_argument);
}

}  // namespace

int main() {
    RUN(independentTransitionTableCoversSharedFollowPolicy);
    RUN(dirtyConflictUsesDiskDiffTargetWithoutBufferPolicy);
    RUN(queueIsBoundedAndSameFileReplacesInPlace);
    RUN(resumeResolvesRenameDeleteAndSkipsRevertedOrMissingTargets);
    RUN(staleChangesAndInvalidClientsAreFailureAtomic);
    RUN(commandViewDeltaAndFooterAreComplete);
    RUN(configurationRejectsInvalidQueueCapacity);
    return failed == 0 ? 0 : 1;
}
