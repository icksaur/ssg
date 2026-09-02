#include "test_helpers.h"

#include <ssg/FilesystemWatcher.h>
#include <ssg/platform_files.h>

#include <concepts>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace {

using Path = std::filesystem::path;

static_assert(std::same_as<decltype(&ssg::validateWorkspaceRelativePath),
                           ssg::PathValidation (*)(std::string_view,
                                                   ssg::PathSyntax,
                                                   ssg::LongPathPolicy) noexcept>);
static_assert(std::same_as<decltype(&ssg::fileIdentity),
                           ssg::FileIdentity (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::tryLockFile),
                           std::optional<ssg::ExclusiveFileLock> (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::setOwnerOnlyPermissions),
                           void (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::userCacheRoot),
                           Path (*)(std::string_view)>);
static_assert(std::same_as<decltype(&ssg::userConfigRoot),
                           Path (*)(std::string_view)>);
static_assert(std::same_as<decltype(&ssg::userStateRoot),
                           Path (*)(std::string_view)>);
static_assert(std::same_as<decltype(&ssg::replaceFileAtomically),
                           void (*)(const Path&, std::span<const std::byte>)>);
static_assert(std::same_as<decltype(&ssg::readFile),
                           ssg::FileReadResult (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::createFileExclusively),
                           ssg::FileIoResult (*)(const Path&,
                                                 std::span<const std::byte>)>);
static_assert(std::same_as<decltype(&ssg::renameFileNoClobber),
                           ssg::FileIoResult (*)(const Path&, const Path&)>);
static_assert(std::same_as<decltype(&ssg::removeFile),
                           ssg::FileIoResult (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::syncDirectory),
                           ssg::FileIoResult (*)(const Path&)>);
static_assert(std::same_as<decltype(&ssg::copyFileDurably),
                           ssg::FileIoResult (*)(const Path&, const Path&)>);
static_assert(std::same_as<decltype(&ssg::installFileIoFaultInjector),
                           ssg::FileIoFaultInjector* (*)(
                               ssg::FileIoFaultInjector*) noexcept>);
static_assert(std::same_as<decltype(&ssg::makePlatformFilesystemWatcher),
                           std::unique_ptr<ssg::FilesystemWatcher> (*)(
                               const Path&, ssg::WatcherConfig)>);

static_assert(std::movable<ssg::ExclusiveFileLock>);
static_assert(!std::copyable<ssg::ExclusiveFileLock>);

}  // namespace

SSG_TEST_SUITE(test_platform_interface) {
    // Taking each address in an executable forces the selected platform
    // implementation to provide the public symbol, not merely parse the header.
    [[maybe_unused]] auto validateWorkspaceRelativePath =
        &ssg::validateWorkspaceRelativePath;
    [[maybe_unused]] auto fileIdentity = &ssg::fileIdentity;
    [[maybe_unused]] auto tryLockFile = &ssg::tryLockFile;
    [[maybe_unused]] auto setOwnerOnlyPermissions = &ssg::setOwnerOnlyPermissions;
    [[maybe_unused]] auto userCacheRoot = &ssg::userCacheRoot;
    [[maybe_unused]] auto userConfigRoot = &ssg::userConfigRoot;
    [[maybe_unused]] auto userStateRoot = &ssg::userStateRoot;
    [[maybe_unused]] auto replaceFileAtomically = &ssg::replaceFileAtomically;
    [[maybe_unused]] auto readFile = &ssg::readFile;
    [[maybe_unused]] auto createFileExclusively =
        &ssg::createFileExclusively;
    [[maybe_unused]] auto renameFileNoClobber = &ssg::renameFileNoClobber;
    [[maybe_unused]] auto removeFile = &ssg::removeFile;
    [[maybe_unused]] auto syncDirectory = &ssg::syncDirectory;
    [[maybe_unused]] auto copyFileDurably = &ssg::copyFileDurably;
    [[maybe_unused]] auto installFileIoFaultInjector =
        &ssg::installFileIoFaultInjector;
    [[maybe_unused]] auto watcher = &ssg::makePlatformFilesystemWatcher;
    return 0;
}
