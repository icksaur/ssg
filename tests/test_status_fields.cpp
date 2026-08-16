#include "test_helpers.h"

#include <ssg/StatusFields.h>

#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, ssg::StatusFieldProvider> providerMap() {
    std::unordered_map<std::string, ssg::StatusFieldProvider> map;
    for (auto& binding : ssg::defaultStatusFieldProviders()) {
        map.emplace(binding.id, std::move(binding.provider));
    }
    return map;
}

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
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/home/user/projects/acme/backend/api";
    context.homeDirectory = "/home/user";
    context.currentBranch = std::string{"main"};

    auto projection = ssg::projectStatusFields(catalog, providers, context);
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
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/srv/work/project";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(catalog, providers, context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/srv/work/project"});
}

TEST(pathFieldUnknownHomeIsNotAbbreviated) {
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/home/user/project";
    context.homeDirectory = "";  // home unknown -> no abbreviation

    auto projection = ssg::projectStatusFields(catalog, providers, context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/home/user/project"});
}

TEST(pathEqualToHomeBecomesTilde) {
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/home/user";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(catalog, providers, context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"~"});
}

// A home-prefix that is not a path-component boundary (e.g. "/home/username2"
// under home "/home/user") must NOT be abbreviated.
TEST(pathFieldRespectsComponentBoundary) {
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/home/username2/project";
    context.homeDirectory = "/home/user";

    auto projection = ssg::projectStatusFields(catalog, providers, context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) ASSERT_EQ(path->value, std::string{"/home/username2/project"});
}

// The configurable CWD prefix glyph (Style::cwdPrefix) is drawn immediately left
// of the abbreviated path.  Empty by default (the other oracles prove no prefix
// leaks in); a set value is prepended verbatim.
TEST(pathFieldPrependsTheConfiguredCwdPrefix) {
    auto const catalog = ssg::p0StatusFieldCatalog();
    auto const providers = providerMap();
    ssg::StatusFieldProviderContext context;
    context.workspaceRoot = "/home/user/project";
    context.homeDirectory = "/home/user";
    context.cwdPrefix = "\xF0\x9F\x93\x81 ";  // folder + space

    auto projection = ssg::projectStatusFields(catalog, providers, context);
    const auto* path = find(projection.header, "path");
    ASSERT_TRUE(path != nullptr);
    if (path) {
        ASSERT_EQ(path->value, std::string{"\xF0\x9F\x93\x81 ~/project"});
    }
}

}  // namespace

int main() {
    RUN(pathFieldAbbreviatesTheHomeDirectory);
    RUN(pathFieldKeepsAPathOutsideHomeVerbatim);
    RUN(pathFieldUnknownHomeIsNotAbbreviated);
    RUN(pathEqualToHomeBecomesTilde);
    RUN(pathFieldRespectsComponentBoundary);
    RUN(pathFieldPrependsTheConfiguredCwdPrefix);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
