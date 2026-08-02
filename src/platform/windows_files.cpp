#include "ssg/platform_files.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <atomic>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

[[noreturn]] void throw_last_error(std::string_view operation,
                                   const std::filesystem::path& path,
                                   DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            std::string(operation) + ": " + path.string());
}

HANDLE as_handle(std::intptr_t value) noexcept {
    return reinterpret_cast<HANDLE>(value);
}

void close_noexcept(std::intptr_t& value) noexcept {
    if (value != -1) {
        OVERLAPPED range{};
        UnlockFileEx(as_handle(value), 0, 1, 0, &range);
        CloseHandle(as_handle(value));
        value = -1;
    }
}

class TemporaryFile {
public:
    explicit TemporaryFile(const std::filesystem::path& target) {
        static std::atomic<std::uint64_t> sequence{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto name = target.filename().wstring() + L".ssg-tmp-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(sequence.fetch_add(1));
            path_ = target.parent_path() / name;
            handle_ =
                CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return;
            }
            if (GetLastError() != ERROR_FILE_EXISTS) {
                throw_last_error("create replacement temporary file", path_);
            }
        }
        throw std::runtime_error("cannot allocate unique replacement temporary file");
    }

    ~TemporaryFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    HANDLE handle() const noexcept { return handle_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    void close_for_publish() {
        if (!CloseHandle(handle_)) {
            handle_ = INVALID_HANDLE_VALUE;
            throw_last_error("close replacement temporary file", path_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

    void published() noexcept { path_.clear(); }

private:
    std::filesystem::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace

FileIdentity file_identity(const std::filesystem::path& path) {
    const HANDLE handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_last_error("open file identity", path);
    }
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw_last_error("read file identity", path, error);
    }
    CloseHandle(handle);
    FileIdentity identity;
    identity.volume = info.VolumeSerialNumber;
    std::memcpy(identity.file.data(), info.FileId.Identifier, sizeof(identity.file));
    return identity;
}

ExclusiveFileLock::ExclusiveFileLock(std::intptr_t native_handle) noexcept
    : native_handle_(native_handle) {}

ExclusiveFileLock::~ExclusiveFileLock() {
    close_noexcept(native_handle_);
}

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : native_handle_(std::exchange(other.native_handle_, -1)) {}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
    if (this != &other) {
        close_noexcept(native_handle_);
        native_handle_ = std::exchange(other.native_handle_, -1);
    }
    return *this;
}

std::optional<ExclusiveFileLock> try_lock_file(
    const std::filesystem::path& path) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_last_error("open lock file", path);
    }
    try {
        set_owner_only_permissions(path);
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    OVERLAPPED range{};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &range)) {
        return ExclusiveFileLock{reinterpret_cast<std::intptr_t>(handle)};
    }
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
        return std::nullopt;
    }
    throw_last_error("acquire file lock", path, error);
}

void set_owner_only_permissions(const std::filesystem::path& path) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD owner_error =
        GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                              &owner, nullptr, nullptr, nullptr, &descriptor);
    if (owner_error != ERROR_SUCCESS) {
        throw_last_error("read file owner", path, owner_error);
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(owner);
    PACL acl = nullptr;
    const DWORD acl_error = SetEntriesInAclW(1, &access, nullptr, &acl);
    if (acl_error != ERROR_SUCCESS) {
        LocalFree(descriptor);
        throw_last_error("build owner-only permissions", path, acl_error);
    }
    SECURITY_DESCRIPTOR owner_only{};
    if (!InitializeSecurityDescriptor(&owner_only,
                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&owner_only, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&owner_only, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) {
        const DWORD error = GetLastError();
        LocalFree(acl);
        LocalFree(descriptor);
        throw_last_error("build owner-only security descriptor", path, error);
    }
    const BOOL set_succeeded =
        SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION |
                                          PROTECTED_DACL_SECURITY_INFORMATION,
                         &owner_only);
    const DWORD set_error = set_succeeded ? ERROR_SUCCESS : GetLastError();
    LocalFree(acl);
    LocalFree(descriptor);
    if (set_error != ERROR_SUCCESS) {
        throw_last_error("set owner-only permissions", path, set_error);
    }
}

std::filesystem::path user_cache_root(std::string_view application_name) {
    const auto validation = validate_workspace_relative_path(
        application_name, PathSyntax::windows);
    if (!validation.valid() ||
        application_name.find_first_of("/\\") != std::string_view::npos) {
        throw std::invalid_argument("cache application name must be one valid component");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD required =
            GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required == 0) {
            throw_last_error("resolve LOCALAPPDATA", {});
        }
        std::wstring root(required, L'\0');
        const DWORD copied =
            GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), required);
        if (copied == 0) {
            throw_last_error("resolve LOCALAPPDATA", {});
        }
        if (copied < required) {
            root.resize(copied);
            return std::filesystem::path{root} /
                   std::filesystem::path{std::u8string(
                       reinterpret_cast<const char8_t*>(application_name.data()),
                       application_name.size())};
        }
    }
    throw std::runtime_error("LOCALAPPDATA changed repeatedly during lookup");
}

// The user's own per-application CONFIGURATION root -- distinct from
// user_cache_root above, which resolves to LOCALAPPDATA (local, disposable,
// never roamed). Config is the thing a user backs up/syncs/hand-edits, so
// this resolves to the ROAMING root (%APPDATA%) instead.
std::filesystem::path user_config_root(std::string_view application_name) {
    const auto validation = validate_workspace_relative_path(
        application_name, PathSyntax::windows);
    if (!validation.valid() ||
        application_name.find_first_of("/\\") != std::string_view::npos) {
        throw std::invalid_argument("config application name must be one valid component");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD required =
            GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (required == 0) {
            throw_last_error("resolve APPDATA", {});
        }
        std::wstring root(required, L'\0');
        const DWORD copied =
            GetEnvironmentVariableW(L"APPDATA", root.data(), required);
        if (copied == 0) {
            throw_last_error("resolve APPDATA", {});
        }
        if (copied < required) {
            root.resize(copied);
            return std::filesystem::path{root} /
                   std::filesystem::path{std::u8string(
                       reinterpret_cast<const char8_t*>(application_name.data()),
                       application_name.size())};
        }
    }
    throw std::runtime_error("APPDATA changed repeatedly during lookup");
}

// The user's own per-application STATE root. Unlike user_config_root above
// (ROAMING, synced), state is app-owned data that survives a restart but is
// neither hand-edited nor roamed -- draft recovery scratch, session remnants.
// Windows has no XDG state analogue, so this resolves to the LOCAL, disposable
// LOCALAPPDATA root like user_cache_root, but under a distinct application
// subtree so it is never mistaken for the cache.
std::filesystem::path user_state_root(std::string_view application_name) {
    const auto validation = validate_workspace_relative_path(
        application_name, PathSyntax::windows);
    if (!validation.valid() ||
        application_name.find_first_of("/\\") != std::string_view::npos) {
        throw std::invalid_argument("state application name must be one valid component");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD required =
            GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required == 0) {
            throw_last_error("resolve LOCALAPPDATA", {});
        }
        std::wstring root(required, L'\0');
        const DWORD copied =
            GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), required);
        if (copied == 0) {
            throw_last_error("resolve LOCALAPPDATA", {});
        }
        if (copied < required) {
            root.resize(copied);
            return std::filesystem::path{root} /
                   std::filesystem::path{std::u8string(
                       reinterpret_cast<const char8_t*>(application_name.data()),
                       application_name.size())};
        }
    }
    throw std::runtime_error("LOCALAPPDATA changed repeatedly during lookup");
}

void replace_file_atomically(const std::filesystem::path& target,
                             std::span<const std::byte> contents) {
    TemporaryFile temporary(target);

    std::size_t written = 0;
    while (written < contents.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(contents.size() - written,
                       static_cast<std::size_t>(MAXDWORD)));
        DWORD count = 0;
        if (!WriteFile(temporary.handle(), contents.data() + written, request,
                       &count, nullptr)) {
            throw_last_error("write replacement temporary file", temporary.path());
        }
        if (count == 0) {
            throw std::runtime_error("replacement write made no progress: " +
                                     target.string());
        }
        written += count;
    }
    if (!FlushFileBuffers(temporary.handle())) {
        throw_last_error("flush replacement temporary file", temporary.path());
    }
    temporary.close_for_publish();

    BOOL replaced = ReplaceFileW(target.c_str(), temporary.path().c_str(), nullptr,
                                 REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    if (!replaced && GetLastError() == ERROR_FILE_NOT_FOUND) {
        replaced = MoveFileExW(temporary.path().c_str(), target.c_str(),
                               MOVEFILE_WRITE_THROUGH);
    }
    if (!replaced) {
        throw_last_error("publish replacement file", target);
    }
    temporary.published();
}

namespace {

FileIoFaultInjector*& faultInjectorSlot() noexcept {
    static FileIoFaultInjector* injector = nullptr;
    return injector;
}

std::optional<FileIoStatus> injectedFailure(
    std::string_view operation, const std::filesystem::path& path) {
    auto* injector = faultInjectorSlot();
    if (injector == nullptr) return std::nullopt;
    const auto status = injector->beforeOperation(operation, path);
    if (status == FileIoStatus::Ok) return std::nullopt;
    return status;
}

FileIoStatus status_for_last_error(DWORD error) noexcept {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return FileIoStatus::NotFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return FileIoStatus::AlreadyExists;
        default:
            return FileIoStatus::IoError;
    }
}

FileIoResult last_error_failure(DWORD error, std::string_view operation,
                                const std::filesystem::path& path) {
    return {status_for_last_error(error),
            std::string(operation) + ": " + path.string() + ": " +
                std::system_category().message(static_cast<int>(error))};
}

} // namespace

FileIoFaultInjector* installFileIoFaultInjector(
    FileIoFaultInjector* injector) noexcept {
    return std::exchange(faultInjectorSlot(), injector);
}

FileReadResult readFile(const std::filesystem::path& path) {
    if (const auto injected = injectedFailure("readFile", path)) {
        return {*injected, {}, "injected fault: readFile"};
    }

    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto failure =
            last_error_failure(GetLastError(), "open file for reading", path);
        return {failure.status, {}, failure.message};
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(handle);
        return {FileIoStatus::IoError, {},
                "path is a directory, not a file: " + path.string()};
    }

    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            const auto failure = last_error_failure(error, "read file", path);
            return {failure.status, {}, failure.message};
        }
        if (count == 0) break;
        bytes.insert(bytes.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::size_t>(count));
    }
    CloseHandle(handle);
    return {FileIoStatus::Ok, std::move(bytes), {}};
}

FileIoResult createFileExclusively(const std::filesystem::path& target,
                                   std::span<const std::byte> contents) {
    if (const auto injected = injectedFailure("createFileExclusively", target)) {
        return {*injected, "injected fault: createFileExclusively"};
    }

    // CREATE_NEW is the Windows counterpart of O_CREAT|O_EXCL: the filesystem,
    // not this process, decides whether the name was already taken.
    const HANDLE handle =
        CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return last_error_failure(GetLastError(), "create file", target);
    }

    std::size_t written = 0;
    while (written < contents.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(contents.size() - written,
                       static_cast<std::size_t>(MAXDWORD)));
        DWORD count = 0;
        if (!WriteFile(handle, contents.data() + written, request, &count,
                       nullptr)) {
            const DWORD error = GetLastError();
            // The handle is held open across the delete so the file cannot be
            // confused with one another process created at the same name.
            CloseHandle(handle);
            DeleteFileW(target.c_str());
            return last_error_failure(error, "write created file", target);
        }
        if (count == 0) {
            CloseHandle(handle);
            DeleteFileW(target.c_str());
            return {FileIoStatus::IoError,
                    "write made no progress: " + target.string()};
        }
        written += count;
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        DeleteFileW(target.c_str());
        return last_error_failure(error, "flush created file", target);
    }
    CloseHandle(handle);
    return {FileIoStatus::Ok, {}};
}

FileIoResult renameFileNoClobber(const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
    if (const auto injected = injectedFailure("renameFileNoClobber", source)) {
        return {*injected, "injected fault: renameFileNoClobber"};
    }

    // Deliberately WITHOUT MOVEFILE_REPLACE_EXISTING, so an occupied
    // destination fails rather than being overwritten.
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        return last_error_failure(GetLastError(), "rename file", destination);
    }
    return {FileIoStatus::Ok, {}};
}

FileIoResult removeFile(const std::filesystem::path& path) {
    if (const auto injected = injectedFailure("removeFile", path)) {
        return {*injected, "injected fault: removeFile"};
    }

    if (!DeleteFileW(path.c_str())) {
        return last_error_failure(GetLastError(), "remove file", path);
    }
    return {FileIoStatus::Ok, {}};
}

FileIoResult syncDirectory(const std::filesystem::path& path) {
    if (const auto injected = injectedFailure("syncDirectory", path)) {
        return {*injected, "injected fault: syncDirectory"};
    }

    // Windows has no directory-handle flush equivalent; its metadata writes for
    // MoveFileEx/ReplaceFile are ordered by the filesystem, and the write-through
    // flags used elsewhere in this file cover the cases that matter.
    std::error_code code;
    if (!std::filesystem::is_directory(path, code) || code) {
        return {FileIoStatus::NotFound, "not a directory: " + path.string()};
    }
    return {FileIoStatus::Ok, {}};
}

FileIoResult copyFileDurably(const std::filesystem::path& source,
                             const std::filesystem::path& destination) {
    if (const auto injected = injectedFailure("copyFileDurably", source)) {
        return {*injected, "injected fault: copyFileDurably"};
    }

    auto contents = readFile(source);
    if (!contents.ok()) {
        return {contents.status, contents.message};
    }
    return createFileExclusively(
        destination,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(contents.bytes.data()),
            contents.bytes.size()});
}

} // namespace ssg
