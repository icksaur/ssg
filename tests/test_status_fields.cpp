#include "test_helpers.h"

#include <ssg/StatusFields.h>
#include <ssg/StatusFields.h>

#include <string>

namespace {

const ssg::StatusField* find(const std::vector<ssg::StatusField>& fields,
                             std::string_view id) {
    for (const auto& field : fields) {
        if (field.id == id) return &field;
    }
    return nullptr;
}

// The header path field abbreviates a leading home directory to "~", so a
// home-rooted workspace path stays short enough to leave room for the branch
// field beside it. Regression: the branch used to be pushed out of the header
// by the full absolute path.
TEST(pathFieldAbbreviatesTheHomeDirectory) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/home/user/projects/acme/backend/api";
    context.homeDirectory = "/home/user";
    context.currentBranch = std::string{"main"};

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    const auto* branch = find(projection.header, "branch");
    ASSERT_TRUE(path != nullptr);
    ASSERT_TRUE(branch != nullptr);
    if (path) {
        ASSERT_EQ(path->value, std::string{"~/projects/acme/backend/api"});
    }
    if (branch) ASSERT_EQ(branch->value, std::string{"\xE2\x8E\x87 main"});
}

TEST(pathFieldKeepsAPathOutsideHomeVerbatim) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/srv/work/project";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/srv/work/project"});
}

TEST(pathFieldUnknownHomeIsNotAbbreviated) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/home/user/project";
    context.homeDirectory = "";  // home unknown -> no abbreviation

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/home/user/project"});
}

TEST(pathEqualToHomeBecomesTilde) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/home/user";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"~"});
}

// A home-prefix that is not a path-component boundary (e.g. "/home/username2"
// under home "/home/user") must NOT be abbreviated.
TEST(pathFieldRespectsComponentBoundary) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/home/username2/project";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/home/username2/project"});
}

// The configurable CWD prefix glyph (Style::cwdPrefix) is drawn immediately left
// of the abbreviated path.  Empty by default (the other oracles prove no prefix
// leaks in); a set value is prepended verbatim.
TEST(pathFieldPrependsTheConfiguredCwdPrefix) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/home/user/project";
    context.homeDirectory = "/home/user";
    context.cwdPrefix = "\xF0\x9F\x93\x81 ";  // folder + space

    auto projection = ssg::projectStatusFields(context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) {
        ASSERT_EQ(path->value, std::string{"\xF0\x9F\x93\x81 ~/project"});
    }
}

// A branch name projects into the header field verbatim (glyph-prefixed);
// absent or empty leaves the header without a branch field (EMPTY-DROP).
TEST(branchFieldProjectsWhenKnownAndDropsWhenAbsentOrEmpty) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/work";
    ASSERT_TRUE(find(ssg::projectStatusFields(context).header, "branch") ==
                nullptr);

    context.currentBranch = std::string{""};
    ASSERT_TRUE(find(ssg::projectStatusFields(context).header, "branch") ==
                nullptr);

    context.currentBranch = std::string{"feature/x"};
    const auto* branch =
        find(ssg::projectStatusFields(context).header, "branch");
    ASSERT_TRUE(branch != nullptr);
    if (branch) {
        ASSERT_EQ(branch->value, std::string{"\xE2\x8E\x87 feature/x"});
    }
}

// The footer status and follow fields project their context values verbatim,
// each dropped (EMPTY-DROP) when its source string is empty.
TEST(statusAndFollowFieldsProjectFromContextAndDropWhenEmpty) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/work";
    auto empty = ssg::projectStatusFields(context);
    ASSERT_TRUE(find(empty.footer, "status") == nullptr);
    ASSERT_TRUE(find(empty.footer, "follow") == nullptr);

    context.statusValue = "3 changes";
    context.followMode = "following";
    auto projected = ssg::projectStatusFields(context);
    const auto* status = find(projected.footer, "status");
    const auto* follow = find(projected.footer, "follow");
    ASSERT_TRUE(status != nullptr);
    ASSERT_TRUE(follow != nullptr);
    if (status) ASSERT_EQ(status->value, std::string{"3 changes"});
    if (follow) ASSERT_EQ(follow->value, std::string{"following"});
}

// Field order is a fixed, stable product vocabulary (FIXED-STATUS): the header
// is always path-then-branch and the footer always status-then-follow, in
// that order, regardless of which fields happen to be present.
TEST(headerAndFooterFieldsKeepTheirFixedStableOrder) {
    ssg::StatusFieldContext context;
    context.workspaceRoot = "/work";
    context.currentBranch = std::string{"main"};
    context.statusValue = "ready";
    context.followMode = "paused";

    auto projection = ssg::projectStatusFields(context);
    ASSERT_EQ(projection.header.size(), std::size_t{2});
    ASSERT_EQ(projection.header[0].id, std::string{"path"});
    ASSERT_EQ(projection.header[1].id, std::string{"branch"});
    ASSERT_EQ(projection.footer.size(), std::size_t{2});
    ASSERT_EQ(projection.footer[0].id, std::string{"status"});
    ASSERT_EQ(projection.footer[1].id, std::string{"follow"});
}

TEST(gridDisplayDecoratesOnlyThePathField) {
    ssg::Style style;
    style.cwdPrefix = "cwd: ";
    for (const std::string_view id :
         {ssg::kPathStatusFieldId, ssg::kBranchStatusFieldId,
          ssg::kStatusValueFieldId, ssg::kFollowStatusFieldId}) {
        const auto expected =
            id == ssg::kPathStatusFieldId ? "cwd: value" : "value";
        ASSERT_EQ(ssg::statusFieldGridDisplay(id, "value", style),
                  std::string{expected});
    }
    ASSERT_EQ(ssg::statusFieldGridDisplay("custom", "value", style),
              std::string{"value"});
}

}  // namespace

SSG_TEST_SUITE(test_status_fields) {
    RUN(pathFieldAbbreviatesTheHomeDirectory);
    RUN(pathFieldKeepsAPathOutsideHomeVerbatim);
    RUN(pathFieldUnknownHomeIsNotAbbreviated);
    RUN(pathEqualToHomeBecomesTilde);
    RUN(pathFieldRespectsComponentBoundary);
    RUN(pathFieldPrependsTheConfiguredCwdPrefix);
    RUN(branchFieldProjectsWhenKnownAndDropsWhenAbsentOrEmpty);
    RUN(statusAndFollowFieldsProjectFromContextAndDropWhenEmpty);
    RUN(headerAndFooterFieldsKeepTheirFixedStableOrder);
    RUN(gridDisplayDecoratesOnlyThePathField);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
