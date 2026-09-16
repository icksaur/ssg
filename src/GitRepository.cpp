#include <ssg/GitDiffSource.h>

#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef SSG_LIBGIT2
#include <git2.h>
#endif

namespace ssg {
namespace {

// A file that cannot be read yields no text: this reader backs git status
// probes where an unreadable path and an absent one are equally "nothing to
// diff". The seam still forces the distinction to be stated rather than assumed.
std::string readWorktreeText(const std::filesystem::path& path) {
    auto result = readFile(path);
    if (!result.ok()) return {};
    return {reinterpret_cast<const char*>(result.bytes.data()),
            result.bytes.size()};
}

#ifdef SSG_LIBGIT2

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::string oidHex(const git_oid* oid) {
    if (oid == nullptr || git_oid_is_zero(oid)) {
        return "0";
    }
    std::array<char, GIT_OID_MAX_HEXSIZE + 1> buffer{};
    git_oid_tostr(buffer.data(), buffer.size(), oid);
    return {buffer.data()};
}

bool loadBlob(git_repository* repository, const git_oid* oid,
              std::size_t maxBytes, std::optional<std::string>& content) {
    if (oid == nullptr || git_oid_is_zero(oid)) {
        content.reset();
        return true;
    }
    git_blob* blob = nullptr;
    if (git_blob_lookup(&blob, repository, oid) != 0) {
        return false;
    }
    auto blobGuard = std::unique_ptr<git_blob, decltype(&git_blob_free)>(
        blob, &git_blob_free);
    auto size = git_blob_rawsize(blob);
    if (static_cast<std::uintmax_t>(size) > maxBytes) {
        return false;
    }
    auto bytes = static_cast<const char*>(git_blob_rawcontent(blob));
    content = std::string{bytes, bytes + size};
    return true;
}

bool collectDiffFiles(git_repository* repository, git_diff* diff,
                      const std::filesystem::path& root,
                      const GitDiffConfig& config,
                      std::vector<GitDiffFile>& files) {
    git_diff_find_options findOptions = GIT_DIFF_FIND_OPTIONS_INIT;
    findOptions.flags = GIT_DIFF_FIND_RENAMES;
    (void)git_diff_find_similar(diff, &findOptions);
    auto count = git_diff_num_deltas(diff);
    if (count > config.maxFiles) {
        return false;
    }
    files.clear();
    files.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto delta = git_diff_get_delta(diff, index);
        if (delta == nullptr) {
            return false;
        }
        const bool deleted = delta->status == GIT_DELTA_DELETED;
        std::filesystem::path currentPath =
            deleted ? std::filesystem::path{delta->old_file.path}
                    : std::filesystem::path{delta->new_file.path};
        std::optional<std::filesystem::path> previousPath;
        if (delta->status == GIT_DELTA_RENAMED) {
            previousPath = std::filesystem::path{delta->old_file.path};
        }
        GitDiffFile file{.id = DiffFileId{currentPath.generic_string()},
                             .path = currentPath,
                             .previousPath = previousPath};
        if (!loadBlob(repository, &delta->old_file.id, config.maxBytesPerFile,
                      file.baselineContent)) {
            return false;
        }
        if (!deleted) {
            auto absolute = root / currentPath;
            if (statFile(absolute)) {
                auto text = readWorktreeText(absolute);
                if (text.size() > config.maxBytesPerFile) {
                    return false;
                }
                file.workingContent = std::move(text);
            } else if (!loadBlob(repository, &delta->new_file.id,
                                 config.maxBytesPerFile, file.workingContent)) {
                return false;
            }
        }
        files.push_back(std::move(file));
    }
    return true;
}

class Libgit2Repository final : public GitRepository {
public:
    explicit Libgit2Repository(std::filesystem::path root)
        : root_(std::move(root)), rootUtf8_{pathUtf8(root_)} {
        git_repository* repository = nullptr;
        const int openResult =
            git_repository_open_ext(&repository, rootUtf8_.c_str(), 0, nullptr);
        if (openResult == 0) {
            available_ = true;
            git_repository_free(repository);
        }
    }

    bool isUsable() const override { return available_; }

    GitDiffScan scanDiff(const GitDiffConfig& config) override {
        GitDiffScan result;
        if (!available_) {
            return result;
        }
        git_repository* repository = nullptr;
        const int openResult =
            git_repository_open_ext(&repository, rootUtf8_.c_str(), 0, nullptr);
        if (openResult != 0) {
            if (openResult != GIT_ENOTFOUND) {
                result.complete = false;
            }
            return result;
        }
        auto repositoryGuard = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repository, &git_repository_free);

        git_index* index = nullptr;
        if (git_repository_index(&index, repository) != 0) {
            result.complete = false;
            return result;
        }
        auto indexGuard = std::unique_ptr<git_index, decltype(&git_index_free)>(
            index, &git_index_free);

        git_oid headOid{};
        if (git_reference_name_to_id(&headOid, repository, "HEAD") != 0) {
            std::memset(&headOid, 0, sizeof(headOid));
        }
        result.baselineIdentity =
            oidHex(&headOid) + ":" + oidHex(git_index_checksum(index));

        git_object* headObject = nullptr;
        if (git_revparse_single(&headObject, repository, "HEAD^{tree}") == 0) {
        }
        auto objectGuard = std::unique_ptr<git_object, decltype(&git_object_free)>(
            headObject, &git_object_free);
        auto headTree = reinterpret_cast<git_tree*>(headObject);

        git_diff_options options = GIT_DIFF_OPTIONS_INIT;
        options.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                        GIT_DIFF_RECURSE_UNTRACKED_DIRS;
        git_diff* diff = nullptr;
        if (git_diff_tree_to_workdir_with_index(&diff, repository, headTree,
                                                &options) != 0) {
            result.complete = false;
            return result;
        }
        auto diffGuard =
            std::unique_ptr<git_diff, decltype(&git_diff_free)>(diff, &git_diff_free);
        if (!collectDiffFiles(repository, diff, root_, config, result.files)) {
            result.complete = false;
            result.files.clear();
        }
        return result;
    }

    GitWorkingTreeScan scanPaths(const std::vector<std::filesystem::path>& paths,
                                 const GitDiffConfig& config) override {
        GitWorkingTreeScan result;
        result.requestedPaths = paths;
        if (!available_) {
            return result;
        }
        git_repository* repository = nullptr;
        const int openResult =
            git_repository_open_ext(&repository, rootUtf8_.c_str(), 0, nullptr);
        if (openResult != 0) {
            if (openResult != GIT_ENOTFOUND) {
                result.complete = false;
            }
            return result;
        }
        auto repositoryGuard = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repository, &git_repository_free);

        git_index* index = nullptr;
        if (git_repository_index(&index, repository) != 0) {
            result.complete = false;
            return result;
        }
        auto indexGuard = std::unique_ptr<git_index, decltype(&git_index_free)>(
            index, &git_index_free);

        git_oid headOid{};
        if (git_reference_name_to_id(&headOid, repository, "HEAD") != 0) {
            std::memset(&headOid, 0, sizeof(headOid));
        }
        result.baselineIdentity =
            oidHex(&headOid) + ":" + oidHex(git_index_checksum(index));

        git_object* headObject = nullptr;
        if (git_revparse_single(&headObject, repository, "HEAD^{tree}") == 0) {
        }
        auto objectGuard = std::unique_ptr<git_object, decltype(&git_object_free)>(
            headObject, &git_object_free);
        auto headTree = reinterpret_cast<git_tree*>(headObject);

        git_diff_options options = GIT_DIFF_OPTIONS_INIT;
        options.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                        GIT_DIFF_RECURSE_UNTRACKED_DIRS;
        std::vector<std::string> specStorage;
        std::vector<char*> specs;
        specStorage.reserve(paths.size());
        specs.reserve(paths.size());
        for (const auto& path : paths) {
            specStorage.push_back(path.generic_string());
        }
        for (auto& spec : specStorage) {
            specs.push_back(spec.data());
        }
        options.pathspec.count = specs.size();
        options.pathspec.strings = specs.data();

        git_diff* diff = nullptr;
        if (git_diff_tree_to_workdir_with_index(&diff, repository, headTree,
                                                &options) != 0) {
            result.complete = false;
            return result;
        }
        auto diffGuard =
            std::unique_ptr<git_diff, decltype(&git_diff_free)>(diff, &git_diff_free);
        if (!collectDiffFiles(repository, diff, root_, config, result.files)) {
            result.complete = false;
            result.files.clear();
        }
        return result;
    }

    std::optional<std::string> currentBranch() override {
        if (!available_) {
            return std::nullopt;
        }
        git_repository* repository = nullptr;
        const int openResult =
            git_repository_open_ext(&repository, rootUtf8_.c_str(), 0, nullptr);
        if (openResult != 0) {
            return std::nullopt;
        }
        auto repositoryGuard = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repository, &git_repository_free);

        git_reference* head = nullptr;
        if (git_repository_head(&head, repository) != 0) {
            return std::nullopt;
        }
        auto headGuard = std::unique_ptr<git_reference, decltype(&git_reference_free)>(
            head, &git_reference_free);

        if (git_repository_head_detached(repository) == 1) {
            const git_oid* oid = git_reference_target(head);
            if (oid == nullptr || git_oid_is_zero(oid)) {
                return std::optional<std::string>{"detached"};
            }
            auto hex = oidHex(oid);
            return std::optional<std::string>{
                hex.substr(0, std::min<std::size_t>(hex.size(), 7))};
        }

        auto shorthand = git_reference_shorthand(head);
        if (shorthand == nullptr || std::string_view{shorthand}.empty()) {
            return std::nullopt;
        }
        return std::optional<std::string>{shorthand};
    }

    std::vector<std::filesystem::path> metadataDirectories() override {
        std::vector<std::filesystem::path> directories;
        if (!available_) {
            return directories;
        }
        git_repository* repository = nullptr;
        if (git_repository_open_ext(&repository, rootUtf8_.c_str(), 0,
                                    nullptr) != 0) {
            return directories;
        }
        auto repositoryGuard =
            std::unique_ptr<git_repository, decltype(&git_repository_free)>(
                repository, &git_repository_free);
        std::set<std::filesystem::path> unique;
        for (const char* raw : {git_repository_path(repository),
                                git_repository_commondir(repository)}) {
            if (raw == nullptr) {
                continue;
            }
            std::error_code error;
            auto directory = canonicalPath(raw, error);
            const auto status =
                error ? std::optional<FileStat>{} : statFile(directory);
            if (!error && status && status->kind == FileKind::Directory) {
                unique.insert(std::move(directory));
            }
        }
        directories.assign(unique.begin(), unique.end());
        return directories;
    }

private:
    std::filesystem::path root_;
    std::string rootUtf8_;
    bool available_ = false;
};

class Libgit2Scope {
public:
    Libgit2Scope() { (void)git_libgit2_init(); }
    ~Libgit2Scope() { (void)git_libgit2_shutdown(); }
};

class Libgit2IgnoreMatcher final : public GitIgnoreMatcher {
public:
    explicit Libgit2IgnoreMatcher(const std::filesystem::path& workspaceRoot) {
        git_repository* repository = nullptr;
        const auto workspaceRootUtf8 = pathUtf8(workspaceRoot);
        // Flags 0 searches upward, so a workspace nested inside a repository
        // still finds it -- which is exactly the case that makes rebasing
        // necessary below.
        if (git_repository_open_ext(&repository, workspaceRootUtf8.c_str(), 0,
                                    nullptr) != 0) {
            return;
        }
        repository_.reset(repository);

        const char* workdir = git_repository_workdir(repository);
        if (workdir == nullptr) {  // A bare repository has no work tree.
            repository_.reset();
            return;
        }
        std::error_code error;
        auto relative = std::filesystem::relative(
            workspaceRoot, std::filesystem::path{workdir}, error);
        if (error) {
            repository_.reset();
            return;
        }
        if (relative != std::filesystem::path{"."}) {
            prefix_ = relative.generic_string();
        }
        // A workspace outside the work tree would rebase to "../" paths, which
        // libgit2 cannot answer.  Report unusable rather than silently answer
        // against the wrong prefix.
        if (prefix_.starts_with("..")) {
            repository_.reset();
            prefix_.clear();
        }
    }

    bool usable() const override { return repository_ != nullptr; }

    bool ignores(const std::filesystem::path& workspaceRelative) const override {
        if (!usable()) return false;
        auto relative = workspaceRelative.generic_string();
        if (relative.empty()) return false;
        auto query = prefix_.empty() ? relative : prefix_ + "/" + relative;
        int ignored = 0;
        if (git_ignore_path_is_ignored(&ignored, repository_.get(),
                                       query.c_str()) != 0) {
            return false;
        }
        return ignored != 0;
    }

private:
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repository_{
        nullptr, &git_repository_free};
    // The workspace root relative to the repository work directory; empty when
    // they coincide.
    std::string prefix_;
};

#endif

class InertGitIgnoreMatcher final : public GitIgnoreMatcher {
public:
    bool usable() const override { return false; }
    bool ignores(const std::filesystem::path&) const override { return false; }
};

class InertGitRepository final : public GitRepository {
public:
    bool isUsable() const override { return false; }
    GitDiffScan scanDiff(const GitDiffConfig&) override { return {}; }
    GitWorkingTreeScan scanPaths(const std::vector<std::filesystem::path>& paths,
                                 const GitDiffConfig&) override {
        GitWorkingTreeScan scan;
        scan.requestedPaths = paths;
        return scan;
    }
    std::optional<std::string> currentBranch() override { return std::nullopt; }
    std::vector<std::filesystem::path> metadataDirectories() override {
        return {};
    }
};

}  // namespace

std::unique_ptr<GitRepository> makePlatformGitRepository(
    const std::filesystem::path& canonicalRoot) {
#ifdef SSG_LIBGIT2
    static Libgit2Scope scope;
    return std::make_unique<Libgit2Repository>(canonicalRoot);
#else
    (void)canonicalRoot;
    return std::make_unique<InertGitRepository>();
#endif
}

std::unique_ptr<GitIgnoreMatcher> makePlatformGitIgnoreMatcher(
    const std::filesystem::path& workspaceRoot) {
#ifdef SSG_LIBGIT2
    static Libgit2Scope scope;
    auto matcher = std::make_unique<Libgit2IgnoreMatcher>(workspaceRoot);
    if (matcher->usable()) return matcher;
    return std::make_unique<InertGitIgnoreMatcher>();
#else
    (void)workspaceRoot;
    return std::make_unique<InertGitIgnoreMatcher>();
#endif
}

}  // namespace ssg
