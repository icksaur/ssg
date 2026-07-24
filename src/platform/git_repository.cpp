#include "ssg/GitDiffSource.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef SSG_LIBGIT2
#include <git2.h>
#endif

namespace ssg {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

#ifdef SSG_LIBGIT2

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
    if (size < 0 || static_cast<std::size_t>(size) > maxBytes) {
        return false;
    }
    auto bytes = static_cast<const char*>(git_blob_rawcontent(blob));
    content = std::string{bytes, bytes + size};
    return true;
}

bool collectDiffFiles(git_repository* repository, git_diff* diff,
                      const std::filesystem::path& root,
                      const GitDiffConfig& config,
                      std::vector<GitDiffScanFile>& files) {
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
        GitDiffScanFile file{.id = DiffFileId{currentPath.generic_string()},
                             .path = currentPath,
                             .previousPath = previousPath};
        if (!loadBlob(repository, &delta->old_file.id, config.maxBytesPerFile,
                      file.baselineContent)) {
            return false;
        }
        if (!deleted) {
            auto absolute = root / currentPath;
            if (std::filesystem::exists(absolute)) {
                auto text = readFile(absolute);
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
        : root_(std::move(root)) {
        git_repository* repository = nullptr;
        const int openResult =
            git_repository_open_ext(&repository, root_.c_str(), 0, nullptr);
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
            git_repository_open_ext(&repository, root_.c_str(), 0, nullptr);
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
            git_repository_open_ext(&repository, root_.c_str(), 0, nullptr);
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
            git_repository_open_ext(&repository, root_.c_str(), 0, nullptr);
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

private:
    std::filesystem::path root_;
    bool available_ = false;
};

class Libgit2Scope {
public:
    Libgit2Scope() { (void)git_libgit2_init(); }
    ~Libgit2Scope() { (void)git_libgit2_shutdown(); }
};

#endif

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

}  // namespace ssg
