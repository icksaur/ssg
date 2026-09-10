#include <ssg/WorkspaceCorpus.h>
#include <ssg/TextCodec.h>
#include <ssg/Workspace.h>

#include <algorithm>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace ssg {
namespace {

namespace fs = std::filesystem;

struct Entry {
    fs::path absolute;
    std::string name;
    FileKind kind = FileKind::Other;
};

bool beneath(const fs::path& directory, const fs::path& candidate) {
    auto directoryPart = directory.begin();
    auto candidatePart = candidate.begin();
    for (; directoryPart != directory.end();
         ++directoryPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            *directoryPart != *candidatePart) {
            return false;
        }
    }
    return true;
}

struct Walk {
    const GitIgnoreMatcher* ignore = nullptr;
    const WorkspaceCorpusOptions* options = nullptr;
    std::set<std::string> seen;
    std::vector<std::string> paths;
    bool halted = false;

    [[nodiscard]] bool ignored(const fs::path& relative) const {
        return options->respectGitignore && ignore->usable() &&
               ignore->ignores(relative);
    }

    [[nodiscard]] bool excluded(const fs::path& absolute) const {
        return std::ranges::any_of(
            options->excludedDirectories,
            [&](const fs::path& directory) {
                return beneath(directory, absolute);
            });
    }

    bool add(std::string path) {
        if (seen.contains(path)) return true;
        if (paths.size() >= options->maximumFiles) {
            halted = true;
            return false;
        }
        seen.insert(path);
        paths.push_back(std::move(path));
        return true;
    }

    std::optional<std::vector<Entry>> entries(const fs::path& directory) {
        const auto listed = listDirectory(directory);
        if (!listed.ok() || !listed.complete) {
            halted = true;
            return std::nullopt;
        }

        std::vector<Entry> result;
        for (const auto& directoryEntry : listed.entries) {
            const auto path = directoryEntry.path();
            if (excluded(path)) continue;
            try {
                auto file = statFile(path);
                if (!file) continue;
                auto kind = file->kind;
                if (kind == FileKind::Symlink) {
                    const auto target = statFile(path, SymlinkMode::Follow);
                    if (!target || target->kind != FileKind::Regular) continue;
                    kind = target->kind;
                }
                if (kind != FileKind::Directory &&
                    kind != FileKind::Regular) {
                    continue;
                }
                result.push_back({path, path.filename().string(), kind});
            } catch (const std::system_error&) {
                halted = true;
                return std::nullopt;
            }
        }
        std::ranges::sort(result, {}, &Entry::name);
        return result;
    }

    bool descend(const fs::path& directory, const fs::path& relativeBase) {
        auto listed = entries(directory);
        if (!listed) return false;
        for (const auto& entry : *listed) {
            const auto relative =
                relativeBase.empty() ? fs::path{entry.name}
                                     : relativeBase / entry.name;
            if (entry.kind == FileKind::Directory) {
                if (entry.name == ".git" || ignored(relative)) continue;
                if (!descend(entry.absolute, relative)) return false;
                continue;
            }
            if (ignored(relative)) continue;
            if (!add(relative.generic_string())) return false;
        }
        return true;
    }
};

}  // namespace

WorkspaceCorpus::WorkspaceCorpus(
    fs::path workspaceRoot,
    std::vector<WorkspaceCorpusBuffer> openBuffers,
    const GitIgnoreMatcher& ignore,
    WorkspaceCorpusReader reader,
    WorkspaceCorpusOptions options)
    : root_{std::move(workspaceRoot)},
      reader_{std::move(reader)} {
    std::map<std::string,
             std::function<std::optional<std::string>()>> openByPath;
    for (auto& buffer : openBuffers) {
        if (!buffer.path || buffer.path->empty()) continue;
        const auto normalized =
            fs::path{*buffer.path}.lexically_normal().generic_string();
        openByPath.insert_or_assign(normalized, std::move(buffer.read));
    }

    Walk walk;
    walk.ignore = &ignore;
    walk.options = &options;
    for (auto& [path, read] : openByPath) {
        if (!walk.add(path)) break;
        openBuffers_.emplace(path, std::move(read));
    }
    if (!walk.halted) {
        (void)walk.descend(root_, {});
    }
    std::ranges::sort(walk.paths);
    paths_ = std::move(walk.paths);
}

const std::vector<std::string>& WorkspaceCorpus::paths() const noexcept {
    return paths_;
}

std::optional<WorkspaceCorpusFile> WorkspaceCorpus::read(
    std::size_t index) const {
    if (index >= paths_.size()) return std::nullopt;
    const auto& path = paths_[index];
    const auto open = openBuffers_.find(path);
    if (open != openBuffers_.end()) {
        auto text = open->second();
        if (!text) return std::nullopt;
        return WorkspaceCorpusFile{path, std::move(*text)};
    }

    auto loaded = reader_(root_ / fs::path{path});
    if (!loaded.ok() || containsBinaryNul(loaded.bytes)) {
        return std::nullopt;
    }
    if (!decodeText(loaded.bytes).accepted()) return std::nullopt;
    return WorkspaceCorpusFile{
        path,
        std::string{reinterpret_cast<const char*>(loaded.bytes.data()),
                    loaded.bytes.size()}};
}

}  // namespace ssg
