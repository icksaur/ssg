#include "ssg/platform_files.h"
#include "test_helpers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

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
        ssg::LongPathPolicy long_paths;
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
                      test.path, test.syntax, test.long_paths)
                      .error,
                  test.expected);
    }

    const std::string long_path = std::string(130, 'a') + "\\" +
                                  std::string(130, 'b');
    ASSERT_EQ(ssg::validateWorkspaceRelativePath(
                  long_path, Windows, ssg::LongPathPolicy::Legacy)
                  .error,
              PathTooLong);
    ASSERT_EQ(ssg::validateWorkspaceRelativePath(
                  long_path, Windows, ssg::LongPathPolicy::Extended)
                  .error,
              None);
}

TEST(identityIsStableAcrossReopenAndRename) {
    TemporaryDirectory temporary;
    const auto original = temporary.path() / "original";
    const auto renamed = temporary.path() / "renamed";
    writeText(original, "content");

    const auto before = ssg::fileIdentity(original);
    ASSERT_EQ(ssg::fileIdentity(original), before);
    std::filesystem::rename(original, renamed);
    ASSERT_EQ(ssg::fileIdentity(renamed), before);

    const auto other = temporary.path() / "other";
    writeText(other, "content");
    ASSERT_NE(ssg::fileIdentity(other), before);
}

TEST(lockContentionAndReleaseFollowRaii) {
    TemporaryDirectory temporary;
    const auto lock_path = temporary.path() / "session.lock";

    auto first = ssg::tryLockFile(lock_path);
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(ssg::tryLockFile(lock_path).has_value());
    first.reset();
    ASSERT_TRUE(ssg::tryLockFile(lock_path).has_value());
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

TEST(atomicReplacementNeverExposesPartialBytes) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "document";
    const std::string old_contents(64 * 1024, 'o');
    const std::string new_contents(64 * 1024, 'n');
    writeText(target, old_contents);

    std::atomic<bool> stop = false;
    std::atomic<bool> partial = false;
    std::thread reader([&] {
        while (!stop.load()) {
            const auto observed = tryReadText(target);
            if (observed.has_value() && *observed != old_contents &&
                *observed != new_contents) {
                partial = true;
            }
        }
    });
    for (int iteration = 0; iteration < 50; ++iteration) {
        const auto& contents = iteration % 2 == 0 ? new_contents : old_contents;
        ssg::replaceFileAtomically(target, bytes(contents));
    }
    stop = true;
    reader.join();
    ASSERT_FALSE(partial.load());
}

} // namespace

int main() {
    RUN(pathPolicyDecisionTable);
    RUN(identityIsStableAcrossReopenAndRename);
    RUN(lockContentionAndReleaseFollowRaii);
    RUN(ownerOnlyPermissionsAreApplied);
    RUN(cacheRootContainsValidatedApplicationComponent);
    RUN(atomicReplacementPublishesCompleteBytes);
    RUN(atomicReplacementNeverExposesPartialBytes);
    std::cout << "Passed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
