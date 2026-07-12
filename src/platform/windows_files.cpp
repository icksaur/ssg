#include "ssg/platform_files.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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

} // namespace ssg
