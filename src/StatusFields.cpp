#include <ssg/StatusFields.h>

#include <string_view>
#include <utility>

namespace ssg {
namespace {

std::string composeBranchField(std::string_view branchName) {
    return std::string{"\xE2\x8E\x87 "} + std::string{branchName};
}

// Replaces a leading home directory with "~" so a home-rooted workspace path
// shows compactly (e.g. "~/repo/ssg"), leaving header room for the branch. A
// path outside home, or an unknown home, is returned unchanged.
std::string abbreviateHome(const std::filesystem::path& path,
                           std::string_view home) {
    auto text = path.generic_string();
    if (home.empty() || text.size() < home.size() ||
        text.compare(0, home.size(), home) != 0) {
        return text;
    }
    if (text.size() == home.size()) return "~";
    if (text[home.size()] == '/') return "~" + text.substr(home.size());
    return text;
}

} // namespace

StatusFieldProjection projectStatusFields(
    StatusFieldContext const& context) {
    StatusFieldProjection projection;

    // path: header, collapses first. Always computable (home abbreviation plus
    // the client cwd prefix); dropped only if the composed value is empty.
    if (auto value = context.cwdPrefix +
                     abbreviateHome(context.workspaceRoot, context.homeDirectory);
        !value.empty()) {
        projection.header.push_back(
            {std::string{kPathStatusFieldId}, "path", std::move(value)});
    }

    // branch: header, collapses second. Present only when a branch name is known.
    if (context.currentBranch && !context.currentBranch->empty()) {
        projection.header.push_back(
            {std::string{kBranchStatusFieldId}, "branch",
             composeBranchField(*context.currentBranch)});
    }

    // status: footer, collapses first.
    if (!context.statusValue.empty()) {
        projection.footer.push_back(
            {std::string{kStatusValueFieldId}, "status", context.statusValue});
    }

    // follow: footer, collapses second.
    if (!context.followMode.empty()) {
        projection.footer.push_back(
            {std::string{kFollowStatusFieldId}, "follow edits",
             context.followMode});
    }

    return projection;
}

} // namespace ssg
