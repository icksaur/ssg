#include <ssg/platform_files.h>
#include "test_helpers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-platform-files-" + std::to_string(seed));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};


void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::optional<std::string> tryReadText(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(handle);
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool succeeded =
        ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
                 &read, nullptr);
    CloseHandle(handle);
    if (!succeeded) {
        return std::nullopt;
    }
    contents.resize(read);
    return contents;
#else
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
#endif
}

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

TEST(pathPolicyDecisionTable) {
    using enum ssg::PathError;
    using enum ssg::PathSyntax;

    struct Case {
        std::string path;
        ssg::PathSyntax syntax;
        ssg::LongPathPolicy longPaths;
        ssg::PathError expected;
    };

    const std::array cases{
        Case{"src/main.cpp", Linux, ssg::LongPathPolicy::Legacy, None},
        Case{"dir\\name", Linux, ssg::LongPathPolicy::Legacy, None},
        Case{"", Linux, ssg::LongPathPolicy::Legacy, Empty},
        Case{"/etc/passwd", Linux, ssg::LongPathPolicy::Legacy, Absolute},
        Case{"a/../b", Linux, ssg::LongPathPolicy::Legacy, Traversal},
        Case{std::string{"bad\0name", 8}, Linux, ssg::LongPathPolicy::Legacy,
             InvalidCharacter},
        Case{std::string{"bad\xff", 4}, Linux, ssg::LongPathPolicy::Legacy,
             InvalidUtf8},
        Case{"src\\main.cpp", Windows, ssg::LongPathPolicy::Legacy, None},
        Case{"CON", Windows, ssg::LongPathPolicy::Legacy, ReservedName},
        Case{"aux.txt", Windows, ssg::LongPathPolicy::Legacy, ReservedName},
        Case{"COM9.log", Windows, ssg::LongPathPolicy::Legacy, ReservedName},
        Case{"COM10.log", Windows, ssg::LongPathPolicy::Legacy, None},
        Case{"bad<name", Windows, ssg::LongPathPolicy::Legacy, InvalidCharacter},
        Case{"name.", Windows, ssg::LongPathPolicy::Legacy, TrailingDotOrSpace},
        Case{"name ", Windows, ssg::LongPathPolicy::Legacy, TrailingDotOrSpace},
        Case{"..\\name", Windows, ssg::LongPathPolicy::Legacy, Traversal},
        Case{"C:\\name", Windows, ssg::LongPathPolicy::Legacy, Absolute},
        Case{std::string(260, 'a'), Windows, ssg::LongPathPolicy::Legacy,
             ComponentTooLong},
    };

    for (const auto& test : cases) {
        ASSERT_EQ(ssg::validateWorkspaceRelativePath(
                      test.path, test.syntax, test.longPaths)
                      .error,
                  test.expected);
    }

    const std::string longPath = std::string(130, 'a') + "\\" +
                                  std::string(130, 'b');
    ASSERT_EQ(ssg::validateWorkspaceRelativePath(
                  longPath, Windows, ssg::LongPathPolicy::Legacy)
                  .error,
              PathTooLong);
    ASSERT_EQ(ssg::validateWorkspaceRelativePath(
                  longPath, Windows, ssg::LongPathPolicy::Extended)
                  .error,
              None);
}

TEST(identityIsStableAcrossReopenAndRename) {
    TemporaryDirectory temporary;
    const auto original = temporary.path() / "original";
    const auto renamed = temporary.path() / "renamed";
    writeText(original, "content");

    const auto initial = ssg::statFile(original);
    ASSERT_TRUE(initial.has_value());
    ASSERT_EQ(initial->kind, ssg::FileKind::Regular);
    ASSERT_EQ(initial->size, std::uintmax_t{7});
    ASSERT_EQ(initial->mtime, std::filesystem::last_write_time(original));
    ASSERT_FALSE(ssg::statFile(temporary.path() / "missing").has_value());
    ASSERT_EQ(ssg::statFile(temporary.path())->kind,
              ssg::FileKind::Directory);
#ifndef _WIN32
    const auto link = temporary.path() / "link";
    const auto chained = temporary.path() / "chained";
    std::filesystem::create_symlink(original, link);
    std::filesystem::create_symlink(link.filename(), chained);
    ASSERT_EQ(ssg::statFile(link)->kind, ssg::FileKind::Symlink);
    ASSERT_NE(ssg::statFile(link)->identity, initial->identity);
    ASSERT_EQ(ssg::statFile(link, ssg::SymlinkMode::Follow)->identity,
              initial->identity);
    ASSERT_EQ(ssg::statFile(chained, ssg::SymlinkMode::Follow)->identity,
              initial->identity);
    const auto cycleA = temporary.path() / "cycle-a";
    const auto cycleB = temporary.path() / "cycle-b";
    std::filesystem::create_symlink(cycleB.filename(), cycleA);
    std::filesystem::create_symlink(cycleA.filename(), cycleB);
    ASSERT_FALSE(
        ssg::statFile(cycleA, ssg::SymlinkMode::Follow).has_value());
#endif
    const auto before = initial->identity;
    ASSERT_EQ(ssg::statFile(original)->identity, before);
    std::filesystem::rename(original, renamed);
    ASSERT_EQ(ssg::statFile(renamed)->identity, before);

    const auto other = temporary.path() / "other";
    writeText(other, "content");
    ASSERT_NE(ssg::statFile(other)->identity, before);
}

TEST(durableAppendSyncAndRenamePreserveBytes) {
    TemporaryDirectory temporary;
    const auto original = temporary.path() / "journal";
    const auto renamed = temporary.path() / "renamed";

    ASSERT_TRUE(ssg::appendFileDurably(original, bytes("first")).ok());
    ASSERT_TRUE(ssg::appendFileDurably(original, bytes("-second")).ok());
    ASSERT_TRUE(ssg::syncFile(original).ok());
    ASSERT_EQ(readText(original), "first-second");

    ASSERT_TRUE(ssg::renamePathDurably(original, renamed).ok());
    ASSERT_FALSE(std::filesystem::exists(original));
    ASSERT_EQ(readText(renamed), "first-second");
}

TEST(lockContentionAndReleaseFollowRaii) {
    TemporaryDirectory temporary;
    const auto lockPath = temporary.path() / "session.lock";

    auto first = ssg::tryLockFile(lockPath);
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(ssg::tryLockFile(lockPath).has_value());
    first.reset();
    ASSERT_TRUE(ssg::tryLockFile(lockPath).has_value());
}

TEST(ownerOnlyPermissionsAreApplied) {
    TemporaryDirectory temporary;
    const auto file = temporary.path() / "private";
    writeText(file, "secret");
    ssg::setOwnerOnlyPermissions(file);

#ifdef _WIN32
    PACL acl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result =
        GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION |
                                  DACL_SECURITY_INFORMATION,
                              &owner, nullptr,
                              &acl, nullptr, &descriptor);
    ASSERT_EQ(result, static_cast<DWORD>(ERROR_SUCCESS));
    ASSERT_TRUE(acl != nullptr);
    bool only_owner_is_allowed = acl != nullptr;
    if (acl != nullptr) {
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw_ace = nullptr;
            if (!GetAce(acl, index, &raw_ace)) {
                only_owner_is_allowed = false;
                continue;
            }
            const auto* header = static_cast<ACE_HEADER*>(raw_ace);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
                if (!EqualSid(owner, const_cast<DWORD*>(&ace->SidStart))) {
                    only_owner_is_allowed = false;
                }
            }
        }
    }
    ASSERT_TRUE(only_owner_is_allowed);
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
#else
    const auto permissions = std::filesystem::status(file).permissions();
    ASSERT_TRUE((permissions & std::filesystem::perms::owner_read) !=
                std::filesystem::perms::none);
    ASSERT_TRUE((permissions & std::filesystem::perms::owner_write) !=
                std::filesystem::perms::none);
    ASSERT_EQ(permissions & (std::filesystem::perms::group_all |
                             std::filesystem::perms::others_all),
              std::filesystem::perms::none);
#endif
}

TEST(cacheRootContainsValidatedApplicationComponent) {
    const auto root = ssg::userCacheRoot("ssg-test");
    ASSERT_FALSE(root.empty());
    ASSERT_EQ(root.filename(), std::filesystem::path{"ssg-test"});
    ASSERT_THROWS(ssg::userCacheRoot("../escape"), std::invalid_argument);
}

#ifndef _WIN32
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_{name} {
        if (const char* prior = std::getenv(name); prior != nullptr) {
            hadPrevious_ = true;
            previous_ = prior;
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }

    ~ScopedEnvVar() {
        if (hadPrevious_) {
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    bool hadPrevious_ = false;
    std::string previous_;
};

TEST(configRootPrefersXdgConfigHomeWhenSetAndAbsolute) {
    ScopedEnvVar xdg("XDG_CONFIG_HOME", "/tmp/ssg-xdg-config-test");
    ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
    ASSERT_EQ(ssg::userConfigRoot("ssg"),
              std::filesystem::path{"/tmp/ssg-xdg-config-test/ssg"});
}

TEST(configRootFallsBackToHomeDotConfigWhenXdgUnsetOrRelative) {
    {
        ScopedEnvVar xdg("XDG_CONFIG_HOME", nullptr);
        ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
        ASSERT_EQ(ssg::userConfigRoot("ssg"),
                  std::filesystem::path{"/tmp/ssg-home-test/.config/ssg"});
    }
    {
        // A relative XDG_CONFIG_HOME is ignored -- only an absolute override
        // is honored, matching userCacheRoot's existing XDG_CACHE_HOME rule.
        ScopedEnvVar xdg("XDG_CONFIG_HOME", "relative/path");
        ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
        ASSERT_EQ(ssg::userConfigRoot("ssg"),
                  std::filesystem::path{"/tmp/ssg-home-test/.config/ssg"});
    }
}

TEST(configRootThrowsWhenHomeAndXdgAreBothUnset) {
    ScopedEnvVar xdg("XDG_CONFIG_HOME", nullptr);
    ScopedEnvVar home("HOME", nullptr);
    ASSERT_THROWS(ssg::userConfigRoot("ssg"), std::runtime_error);
}

TEST(stateRootPrefersXdgStateHomeWhenSetAndAbsolute) {
    ScopedEnvVar xdg("XDG_STATE_HOME", "/tmp/ssg-xdg-state-test");
    ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
    ASSERT_EQ(ssg::userStateRoot("ssg"),
              std::filesystem::path{"/tmp/ssg-xdg-state-test/ssg"});
}

TEST(stateRootFallsBackToHomeDotLocalStateWhenXdgUnsetOrRelative) {
    {
        ScopedEnvVar xdg("XDG_STATE_HOME", nullptr);
        ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
        ASSERT_EQ(ssg::userStateRoot("ssg"),
                  std::filesystem::path{"/tmp/ssg-home-test/.local/state/ssg"});
    }
    {
        // A relative XDG_STATE_HOME is ignored -- only an absolute override is
        // honored, matching the config/cache primitives' XDG rule.
        ScopedEnvVar xdg("XDG_STATE_HOME", "relative/path");
        ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
        ASSERT_EQ(ssg::userStateRoot("ssg"),
                  std::filesystem::path{"/tmp/ssg-home-test/.local/state/ssg"});
    }
}

TEST(stateRootThrowsWhenHomeAndXdgAreBothUnset) {
    ScopedEnvVar xdg("XDG_STATE_HOME", nullptr);
    ScopedEnvVar home("HOME", nullptr);
    ASSERT_THROWS(ssg::userStateRoot("ssg"), std::runtime_error);
}

TEST(stateRootIsDistinctFromConfigAndCacheRoots) {
    // State (app-owned, survives restart), config (synced/hand-edited), and
    // cache (disposable) must resolve to three different directories so no
    // primitive's cleanup can destroy another's data.
    ScopedEnvVar stateXdg("XDG_STATE_HOME", nullptr);
    ScopedEnvVar configXdg("XDG_CONFIG_HOME", nullptr);
    ScopedEnvVar cacheXdg("XDG_CACHE_HOME", nullptr);
    ScopedEnvVar home("HOME", "/tmp/ssg-home-test");
    ASSERT_TRUE(ssg::userStateRoot("ssg-test") != ssg::userConfigRoot("ssg-test"));
    ASSERT_TRUE(ssg::userStateRoot("ssg-test") != ssg::userCacheRoot("ssg-test"));
}
#endif

TEST(configRootRejectsMultiComponentApplicationName) {
    ASSERT_THROWS(ssg::userConfigRoot("../escape"), std::invalid_argument);
}

TEST(stateRootRejectsMultiComponentApplicationName) {
    ASSERT_THROWS(ssg::userStateRoot("../escape"), std::invalid_argument);
}

TEST(configRootIsDistinctFromCacheRootForTheSameApplication) {
    // Config (backed up/synced/hand-edited) and cache (local/disposable) must
    // never resolve to the same directory, even for the same application
    // name -- otherwise a cache-clearing operation could destroy user config.
    ASSERT_TRUE(ssg::userConfigRoot("ssg-test") != ssg::userCacheRoot("ssg-test"));
}

TEST(atomicReplacementPublishesCompleteBytes) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "document";
    writeText(target, "old");

    const std::string replacement(64 * 1024, 'n');
    ssg::replaceFileAtomically(target, bytes(replacement));
    ASSERT_EQ(readText(target), replacement);

    for (const auto& entry : std::filesystem::directory_iterator(temporary.path())) {
        ASSERT_EQ(entry.path().filename(), target.filename());
    }
}

TEST(directorySeamCreatesListsBoundsAndRemovesTrees) {
    TemporaryDirectory temporary;
    const auto tree = temporary.path() / "tree";
    const auto nested = tree / "nested";

    ASSERT_EQ(ssg::createDirectoriesDurably(nested).status,
              ssg::FileIoStatus::Ok);
    ASSERT_EQ(ssg::createDirectoriesDurably(nested).status,
              ssg::FileIoStatus::AlreadyExists);
    ASSERT_EQ(ssg::ensureDirectory(nested).status, ssg::FileIoStatus::Ok);
    writeText(tree / "root.txt", "root");
    writeText(nested / "nested.txt", "nested");

    const auto children = ssg::listDirectory(tree);
    ASSERT_TRUE(children.ok());
    ASSERT_TRUE(children.complete);
    ASSERT_EQ(children.entries.size(), std::size_t{2});

    const auto bounded = ssg::listDirectory(
        tree, ssg::DirectoryTraversal::Recursive, 2);
    ASSERT_TRUE(bounded.ok());
    ASSERT_FALSE(bounded.complete);
    ASSERT_EQ(bounded.entries.size(), std::size_t{2});

    ASSERT_EQ(ssg::removeTree(tree).status, ssg::FileIoStatus::Ok);
    ASSERT_EQ(ssg::removeTree(tree).status, ssg::FileIoStatus::NotFound);
    ASSERT_EQ(ssg::removeTreeIfPresent(tree).status, ssg::FileIoStatus::Ok);
}

TEST(treeBytesCountsRegularFilesWithoutFollowingSymlinks) {
    TemporaryDirectory temporary;
    const auto tree = temporary.path() / "tree";
    ASSERT_TRUE(ssg::ensureDirectory(tree / "nested").ok());
    writeText(tree / "root.txt", "abc");
    writeText(tree / "nested" / "child.txt", "1234");

    std::error_code error;
    std::filesystem::create_symlink(tree / "root.txt", tree / "file-link", error);
    if (!error) {
        std::filesystem::create_directory_symlink(
            tree / "nested", tree / "directory-link", error);
    }

    ASSERT_EQ(ssg::treeBytes(tree), std::uintmax_t{7});
    ASSERT_EQ(ssg::treeBytes(temporary.path() / "missing"), std::uintmax_t{0});
}

 TEST(directoryCreationHasOneLeafWinner) {
    TemporaryDirectory temporary;
    const auto leaf = temporary.path() / "claimed";
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<ssg::FileIoStatus> statuses(8);
    std::vector<std::thread> threads;
    threads.reserve(statuses.size());
    for (std::size_t index = 0; index < statuses.size(); ++index) {
        threads.emplace_back([&, index] {
            ++ready;
            while (!start.load()) {
                std::this_thread::yield();
            }
            statuses[index] = ssg::createDirectoriesDurably(leaf).status;
        });
    }
    while (ready.load() != static_cast<int>(statuses.size())) {
        std::this_thread::yield();
    }
    start = true;
    for (auto& thread : threads) thread.join();

    ASSERT_EQ(std::count(statuses.begin(), statuses.end(),
                         ssg::FileIoStatus::Ok),
              std::ptrdiff_t{1});
    ASSERT_EQ(std::count(statuses.begin(), statuses.end(),
                         ssg::FileIoStatus::AlreadyExists),
              static_cast<std::ptrdiff_t>(statuses.size() - 1));
}

TEST(atomicReplacementNeverExposesPartialBytes) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "document";
    const std::string oldContents(64 * 1024, 'o');
    const std::string newContents(64 * 1024, 'n');
    writeText(target, oldContents);

    std::atomic<bool> stop = false;
    std::atomic<bool> partial = false;
    std::thread reader([&] {
        while (!stop.load()) {
            const auto observed = tryReadText(target);
            if (observed.has_value() && *observed != oldContents &&
                *observed != newContents) {
                partial = true;
            }
        }
    });
    for (int iteration = 0; iteration < 50; ++iteration) {
        const auto& contents = iteration % 2 == 0 ? newContents : oldContents;
        ssg::replaceFileAtomically(target, bytes(contents));
    }
    stop = true;
    reader.join();
    ASSERT_FALSE(partial.load());
}

} // namespace

SSG_TEST_SUITE(test_platform_files) {
    RUN(pathPolicyDecisionTable);
    RUN(identityIsStableAcrossReopenAndRename);
    RUN(durableAppendSyncAndRenamePreserveBytes);
    RUN(lockContentionAndReleaseFollowRaii);
    RUN(ownerOnlyPermissionsAreApplied);
    RUN(cacheRootContainsValidatedApplicationComponent);
    RUN(atomicReplacementPublishesCompleteBytes);
    RUN(atomicReplacementNeverExposesPartialBytes);
    RUN(directorySeamCreatesListsBoundsAndRemovesTrees);
    RUN(treeBytesCountsRegularFilesWithoutFollowingSymlinks);
    RUN(directoryCreationHasOneLeafWinner);
#ifndef _WIN32
    RUN(configRootPrefersXdgConfigHomeWhenSetAndAbsolute);
    RUN(configRootFallsBackToHomeDotConfigWhenXdgUnsetOrRelative);
    RUN(configRootThrowsWhenHomeAndXdgAreBothUnset);
    RUN(stateRootPrefersXdgStateHomeWhenSetAndAbsolute);
    RUN(stateRootFallsBackToHomeDotLocalStateWhenXdgUnsetOrRelative);
    RUN(stateRootThrowsWhenHomeAndXdgAreBothUnset);
    RUN(stateRootIsDistinctFromConfigAndCacheRoots);
#endif
    RUN(configRootRejectsMultiComponentApplicationName);
    RUN(stateRootRejectsMultiComponentApplicationName);
    RUN(configRootIsDistinctFromCacheRootForTheSameApplication);
    std::cout << "Passed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
