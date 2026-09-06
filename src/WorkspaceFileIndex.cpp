#include <ssg/WorkspaceFileIndex.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <string>
#include <system_error>

namespace ssg {
namespace {

namespace fs = std::filesystem;

struct Entry {
    fs::path absolute;
    std::string name;
    bool directory = false;
};

// One directory level, sorted, so descent order does not depend on the order the
// filesystem happens to return entries in.
std::vector<Entry> sortedEntries(const fs::path& directory) {
    std::vector<Entry> entries;
    const auto listed = listDirectory(directory);
    if (!listed.ok()) return entries;
    for (const auto& entry : listed.entries) {
        const auto status = statFile(entry.path());
        if (!status || status->kind == FileKind::Symlink) continue;
        const bool directoryEntry = status->kind == FileKind::Directory;
        if (!directoryEntry && status->kind != FileKind::Regular) continue;
        entries.push_back({entry.path(), entry.path().filename().string(),
                           directoryEntry});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& left, const Entry& right) {
                  return left.name < right.name;
              });
    return entries;
}

PaletteCandidate candidateFor(const fs::path& relative) {
    // id is what file.open takes; label is the filename, which earns the
    // start-of-string bonus for filename queries and paints left; detail is the
    // parent directory, which paints right in the existing detail column.
    auto const parent = relative.parent_path();
    return PaletteCandidate{relative.generic_string(),
                            relative.filename().string(),
                            parent.generic_string()};
}

struct Walk {
    const GitIgnoreMatcher* ignore = nullptr;
    bool respectGitignore = true;
    std::size_t maximumFiles = 0;
    WorkspaceFileIndexResult result;

    [[nodiscard]] bool ignored(const fs::path& relative) const {
        return respectGitignore && ignore != nullptr && ignore->usable() &&
               ignore->ignores(relative);
    }

    // Returns false once the cap is reached so every enclosing level unwinds
    // immediately rather than continuing to walk a tree whose results are
    // already discarded.
    bool descend(const fs::path& directory, const fs::path& relativeBase) {
        for (const auto& entry : sortedEntries(directory)) {
            if (result.candidates.size() >= maximumFiles) {
                result.truncated = true;
                return false;
            }
            auto const relative =
                relativeBase.empty() ? fs::path{entry.name}
                                     : relativeBase / entry.name;
            if (entry.directory) {
                // .git is pruned unconditionally: git does not list it in
                // .gitignore (it is implicit), and it must never be offered as
                // openable content.
                if (entry.name == ".git") continue;
                if (ignored(relative)) continue;
                if (!descend(entry.absolute, relative)) return false;
                continue;
            }
            if (ignored(relative)) continue;
            result.candidates.push_back(candidateFor(relative));
        }
        return true;
    }
};

}  // namespace

WorkspaceFileIndexResult buildWorkspaceFileIndex(
    const fs::path& workspaceRoot, const GitIgnoreMatcher& ignore,
    const WorkspaceFileIndexOptions& options) {
    Walk walk;
    walk.ignore = &ignore;
    walk.respectGitignore = options.respectGitignore;
    walk.maximumFiles = options.maximumFiles;
    (void)walk.descend(workspaceRoot, fs::path{});
    return std::move(walk.result);
}

}  // namespace ssg
