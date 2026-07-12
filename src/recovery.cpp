#include "ssg/recovery.h"

#include "ssg/platform_files.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ssg {
namespace {

constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{'S'}, std::byte{'S'}, std::byte{'G'}, std::byte{'R'},
    std::byte{'E'}, std::byte{'C'}, std::byte{1},   std::byte{0}};
constexpr std::uintmax_t record_lifecycle_metadata_bytes = 17;

enum class SnapshotKind : std::uint8_t {
    missing,
    regular_file,
    directory,
    symlink,
};

enum class RecordState : std::uint8_t {
    in_progress = 1,
    published = 2,
};

class BudgetExceeded final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    void u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("recovery manifest string is too large");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::byte*>(value.data()), value.size()});
    }

    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (offset_ == bytes_.size()) return false;
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t part = 0;
            if (!u8(part)) return false;
            value |= static_cast<std::uint32_t>(part) << shift;
        }
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8) {
            std::uint8_t part = 0;
            if (!u8(part)) return false;
            value |= static_cast<std::uint64_t>(part) << shift;
        }
        return true;
    }

    [[nodiscard]] bool raw(std::size_t size, std::span<const std::byte>& value) {
        if (size > bytes_.size() - offset_) return false;
        value = bytes_.subspan(offset_, size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool string(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size)) return false;
        std::span<const std::byte> encoded;
        if (!raw(size, encoded)) return false;
        value.assign(reinterpret_cast<const char*>(encoded.data()),
                     encoded.size());
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return offset_ == bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    std::transform(value.begin(), value.end(), std::back_inserter(encoded),
                   [](char byte) {
                       return static_cast<char8_t>(
                           static_cast<unsigned char>(byte));
                   });
    return std::filesystem::path{encoded};
}

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create recovery file: " +
                                 path.string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write recovery file: " +
                                 path.string());
    }
}

[[noreturn]] void throw_sync_error(std::string_view operation,
                                   const std::filesystem::path& path) {
#ifdef _WIN32
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        std::string{operation} + ": " + path.string());
#else
    throw std::system_error(errno, std::generic_category(),
                            std::string{operation} + ": " + path.string());
#endif
}

void sync_path(const std::filesystem::path& path, bool directory) {
#ifdef _WIN32
    if (directory) return;
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_sync_error("failed to open recovery path for flush", path);
    }
    if (!FlushFileBuffers(handle)) {
        const auto failure = GetLastError();
        CloseHandle(handle);
        SetLastError(failure);
        throw_sync_error("failed to flush recovery path", path);
    }
    if (!CloseHandle(handle)) {
        throw_sync_error("failed to close flushed recovery path", path);
    }
#else
    const int flags =
        O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0);
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw_sync_error("failed to open recovery path for sync", path);
    }
    if (::fsync(descriptor) != 0) {
        const int failure = errno;
        ::close(descriptor);
        errno = failure;
        throw_sync_error("failed to sync recovery path", path);
    }
    if (::close(descriptor) != 0) {
        throw_sync_error("failed to close synced recovery path", path);
    }
#endif
}

void sync_tree(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> directories;
    directories.push_back(root);
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        const auto status = entry.symlink_status();
        if (std::filesystem::is_regular_file(status)) {
            sync_path(entry.path(), false);
        } else if (std::filesystem::is_directory(status)) {
            directories.push_back(entry.path());
        }
    }
    for (auto directory = directories.rbegin();
         directory != directories.rend(); ++directory) {
        sync_path(*directory, true);
    }
}

void install_directory_durably(const std::filesystem::path& staging,
                               const std::filesystem::path& installed,
                               const std::filesystem::path& parent) {
#ifdef _WIN32
    if (!MoveFileExW(staging.c_str(), installed.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        throw_sync_error("failed to install recovery directory", installed);
    }
#else
    std::filesystem::rename(staging, installed);
    sync_path(parent, true);
#endif
}

void rename_durably(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        throw_sync_error("failed to durably rename path", source);
    }
#else
    std::filesystem::rename(source, destination);
    sync_path(std::filesystem::absolute(source).parent_path(), true);
    const auto source_parent =
        std::filesystem::absolute(source).parent_path().lexically_normal();
    const auto destination_parent =
        std::filesystem::absolute(destination).parent_path().lexically_normal();
    if (destination_parent != source_parent) {
        sync_path(destination_parent, true);
    }
#endif
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open recovery file: " +
                                 path.string());
    }
    const std::vector<char> encoded{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
    std::vector<std::byte> result(encoded.size());
    std::transform(encoded.begin(), encoded.end(), result.begin(),
                   [](char value) {
                       return static_cast<std::byte>(
                           static_cast<unsigned char>(value));
                   });
    return result;
}

SnapshotKind snapshot_kind(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return SnapshotKind::missing;
    }
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to inspect recovery source", path, error);
    }
    if (std::filesystem::is_symlink(status)) return SnapshotKind::symlink;
    if (std::filesystem::is_regular_file(status)) {
        return SnapshotKind::regular_file;
    }
    if (std::filesystem::is_directory(status)) {
        return SnapshotKind::directory;
    }
    throw std::runtime_error("unsupported filesystem node in recovery action: " +
                             path.string());
}

void copy_node(const std::filesystem::path& source,
               const std::filesystem::path& destination,
               SnapshotKind kind) {
    switch (kind) {
    case SnapshotKind::missing:
        return;
    case SnapshotKind::regular_file:
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing);
        return;
    case SnapshotKind::symlink: {
        std::filesystem::create_directories(destination.parent_path());
        const auto target = std::filesystem::read_symlink(source);
        std::error_code status_error;
        const bool directory_target =
            std::filesystem::is_directory(
                std::filesystem::status(source, status_error)) &&
            !status_error;
        if (directory_target) {
            std::filesystem::create_directory_symlink(target, destination);
        } else {
            std::filesystem::create_symlink(target, destination);
        }
        return;
    }
    case SnapshotKind::directory:
        std::filesystem::create_directories(destination);
        for (const auto& entry : std::filesystem::directory_iterator(source)) {
            const auto child_kind = snapshot_kind(entry.path());
            copy_node(entry.path(), destination / entry.path().filename(),
                      child_kind);
        }
        return;
    }
}

void remove_node(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to remove filesystem node", path, error);
    }
}

void restore_snapshot(const std::filesystem::path& destination,
                      const std::filesystem::path& artifact,
                      SnapshotKind kind) {
    remove_node(destination);
    if (kind != SnapshotKind::missing) {
        copy_node(artifact, destination, kind);
    }
}

std::uintmax_t stored_tree_bytes(const std::filesystem::path& root) {
    std::uintmax_t total = 0;
    const auto add_node = [&](const std::filesystem::path& path) {
        const auto kind = snapshot_kind(path);
        if (kind == SnapshotKind::regular_file) {
            const auto size = std::filesystem::file_size(path);
            if (size > std::numeric_limits<std::uintmax_t>::max() - total) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            total += size;
        } else if (kind == SnapshotKind::symlink) {
            const auto size = path_to_utf8(std::filesystem::read_symlink(path))
                                  .size();
            if (size > std::numeric_limits<std::uintmax_t>::max() - total) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            total += size;
        }
    };

    add_node(root);
    if (snapshot_kind(root) == SnapshotKind::directory) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            add_node(entry.path());
        }
    }
    return total;
}

void protect_tree(const std::filesystem::path& root) {
    set_owner_only_permissions(root);
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (!std::filesystem::is_symlink(entry.symlink_status())) {
            set_owner_only_permissions(entry.path());
        }
    }
}

bool path_component_equal(const std::filesystem::path& left,
                          const std::filesystem::path& right) {
#ifdef _WIN32
    const auto& left_text = left.native();
    const auto& right_text = right.native();
    if (left_text.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right_text.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               left_text.data(), static_cast<int>(left_text.size()),
               right_text.data(), static_cast<int>(right_text.size()), TRUE) ==
           CSTR_EQUAL;
#else
    return left == right;
#endif
}

bool path_contains(const std::filesystem::path& parent,
                   const std::filesystem::path& child) {
    const auto normalized_parent =
        std::filesystem::absolute(parent).lexically_normal();
    const auto normalized_child =
        std::filesystem::absolute(child).lexically_normal();
    auto parent_part = normalized_parent.begin();
    auto child_part = normalized_child.begin();
    for (; parent_part != normalized_parent.end();
         ++parent_part, ++child_part) {
        if (child_part == normalized_child.end() ||
            !path_component_equal(*parent_part, *child_part)) {
            return false;
        }
    }
    return true;
}

std::string exception_message(std::exception_ptr failure) {
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "unknown recovery failure";
    }
    return "unknown recovery failure";
}

RecoveryError error(RecoveryErrorCode code,
                    std::string message,
                    std::string rollback = {}) {
    return {code, std::move(message), std::move(rollback)};
}

bool document_kind(RecoveryRecordKind kind) {
    return kind == RecoveryRecordKind::document_close ||
           kind == RecoveryRecordKind::document_reload;
}

bool valid_record_kind(std::uint8_t kind) {
    return kind <=
           static_cast<std::uint8_t>(RecoveryRecordKind::workspace_replace);
}

bool valid_snapshot_kind(std::uint8_t kind) {
    return kind <= static_cast<std::uint8_t>(SnapshotKind::symlink);
}

struct StoredRecord {
    RecoveryRecord record;
    std::optional<JournalDocument> document;
    std::vector<SnapshotKind> snapshots;
    SnapshotKind replacement = SnapshotKind::missing;
    std::filesystem::path directory;
    bool restored_in_memory = false;
};

std::vector<std::byte> encode_manifest(const StoredRecord& stored) {
    ByteWriter writer;
    writer.raw(manifest_magic);
    writer.u8(static_cast<std::uint8_t>(stored.record.kind));
    writer.u64(stored.record.stored_bytes);

    if (stored.document) {
        writer.u8(1);
        const auto encoded =
            encode_checkpoint_record({std::vector<JournalDocument>{
                *stored.document}});
        if (encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("recovery document is too large");
        }
        writer.u32(static_cast<std::uint32_t>(encoded.size()));
        writer.raw(encoded);
    } else {
        writer.u8(0);
    }

    if (stored.record.affected_paths.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many recovery paths");
    }
    writer.u32(
        static_cast<std::uint32_t>(stored.record.affected_paths.size()));
    for (std::size_t index = 0;
         index != stored.record.affected_paths.size(); ++index) {
        writer.string(path_to_utf8(stored.record.affected_paths[index]));
        writer.u8(static_cast<std::uint8_t>(stored.snapshots[index]));
    }
    writer.u8(static_cast<std::uint8_t>(stored.replacement));
    return writer.take();
}

StoredRecord decode_manifest(const RecoveryRecordId& id,
                             const std::filesystem::path& directory) {
    const auto encoded = read_bytes(directory / "manifest.bin");
    ByteReader reader(encoded);
    std::span<const std::byte> magic;
    if (!reader.raw(manifest_magic.size(), magic) ||
        !std::equal(magic.begin(), magic.end(), manifest_magic.begin())) {
        throw std::runtime_error("invalid recovery manifest magic");
    }

    std::uint8_t encoded_kind = 0;
    std::uint64_t stored_bytes = 0;
    std::uint8_t has_document = 0;
    if (!reader.u8(encoded_kind) || !valid_record_kind(encoded_kind) ||
        !reader.u64(stored_bytes) || !reader.u8(has_document) ||
        has_document > 1) {
        throw std::runtime_error("invalid recovery manifest header");
    }

    std::optional<JournalDocument> document;
    if (has_document != 0) {
        std::uint32_t document_size = 0;
        std::span<const std::byte> document_bytes;
        if (!reader.u32(document_size) ||
            !reader.raw(document_size, document_bytes)) {
            throw std::runtime_error("truncated recovery document");
        }
        const auto replayed = replay_journal(document_bytes);
        if (replayed.discarded_tail ||
            replayed.valid_bytes != document_bytes.size() ||
            replayed.recovery.documents.size() != 1) {
            throw std::runtime_error("invalid recovery document");
        }
        document = replayed.recovery.documents.front();
    }

    std::uint32_t path_count = 0;
    if (!reader.u32(path_count)) {
        throw std::runtime_error("truncated recovery path list");
    }
    std::vector<std::filesystem::path> paths;
    std::vector<SnapshotKind> snapshots;
    paths.reserve(path_count);
    snapshots.reserve(path_count);
    for (std::uint32_t index = 0; index != path_count; ++index) {
        std::string path;
        std::uint8_t kind = 0;
        if (!reader.string(path) || !reader.u8(kind) ||
            !valid_snapshot_kind(kind)) {
            throw std::runtime_error("invalid recovery path entry");
        }
        paths.push_back(path_from_utf8(path));
        snapshots.push_back(static_cast<SnapshotKind>(kind));
    }

    std::uint8_t replacement = 0;
    if (!reader.u8(replacement) || !valid_snapshot_kind(replacement) ||
        !reader.empty()) {
        throw std::runtime_error("invalid recovery manifest tail");
    }

    const auto kind = static_cast<RecoveryRecordKind>(encoded_kind);
    if (document_kind(kind) != document.has_value()) {
        throw std::runtime_error("recovery record payload does not match kind");
    }
    return {{id,
             kind,
             stored_bytes,
             document ? std::optional<JournalDocumentKey>{document->key}
                      : std::nullopt,
             std::move(paths)},
            std::move(document),
            std::move(snapshots),
            static_cast<SnapshotKind>(replacement),
            directory,
            false};
}

} // namespace

RecoveryRecordId::RecoveryRecordId(std::string value) : value_(std::move(value)) {
    if (value_.empty() || value_ == "." || value_ == ".." ||
        value_.find('/') != std::string::npos ||
        value_.find('\\') != std::string::npos) {
        throw std::invalid_argument("invalid recovery record ID");
    }
}

class RecoveryActions::Impl {
public:
    Impl(std::filesystem::path recovery_root,
         RecoveryConfig config,
         RecoveryFaultInjector* fault_injector)
        : recovery_root_(
              std::filesystem::absolute(std::move(recovery_root))
                  .lexically_normal()),
          config_(config),
          fault_injector_(fault_injector) {
        if (config_.maximum_records == 0) {
            throw std::invalid_argument(
                "recovery maximum record count must be greater than zero");
        }
        if (config_.maximum_bytes == 0) {
            throw std::invalid_argument(
                "recovery maximum byte count must be greater than zero");
        }
        std::filesystem::create_directories(recovery_root_);
        set_owner_only_permissions(recovery_root_);
        load_records();
    }

    [[nodiscard]] std::vector<RecoveryRecord> records() const {
        std::vector<RecoveryRecord> result;
        result.reserve(records_.size());
        for (const auto& stored : records_) result.push_back(stored.record);
        return result;
    }

    RecoveryActionResult close_document(
        std::optional<JournalDocument>& document,
        ScratchStore& scratch,
        std::chrono::milliseconds durability_timeout) {
        if (!document) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          "cannot close an absent document")};
        }
        if (durability_timeout <= std::chrono::milliseconds::zero()) {
            return {{},
                    error(RecoveryErrorCode::durability_failed,
                          "dirty close requires a finite positive durability "
                          "timeout")};
        }
        if (document->dirty) {
            try {
                scratch.update_document(*document);
                if (!scratch.wait_until_durable(durability_timeout)) {
                    const auto state = scratch.durability_state();
                    const auto detail =
                        state.failure.empty() ? "durability timed out"
                                              : state.failure;
                    return {{},
                            error(RecoveryErrorCode::durability_failed,
                                  "dirty close rejected: " + detail)};
                }
            } catch (...) {
                return {{},
                        error(RecoveryErrorCode::durability_failed,
                              "dirty close rejected: " +
                                  exception_message(
                                      std::current_exception()))};
            }
        }

        const auto previous = *document;
        return prepare_and_perform(
            RecoveryRecordKind::document_close, previous, {}, std::nullopt,
            [&] {
                before(RecoveryStep::mutate_document);
                document.reset();
            },
            [&](const StoredRecord&) {
                before(RecoveryStep::rollback_document);
                document = previous;
            });
    }

    RecoveryActionResult reload_document(
        std::optional<JournalDocument>& document,
        JournalDocument replacement) {
        if (!document) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          "cannot reload an absent document")};
        }
        const auto previous = *document;
        return prepare_and_perform(
            RecoveryRecordKind::document_reload, previous, {}, std::nullopt,
            [&] {
                before(RecoveryStep::mutate_document);
                document = std::move(replacement);
            },
            [&](const StoredRecord&) {
                before(RecoveryStep::rollback_document);
                document = previous;
            });
    }

    RecoveryActionResult overwrite_file(
        const std::filesystem::path& path,
        std::span<const std::byte> replacement) {
        const std::vector<std::byte> owned_replacement(replacement.begin(),
                                                       replacement.end());
        return prepare_and_perform(
            RecoveryRecordKind::file_overwrite, std::nullopt, {path},
            std::nullopt,
            [&] {
                before(RecoveryStep::mutate_filesystem);
                replace_file_atomically(path, owned_replacement);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::rollback_filesystem);
                restore_snapshot(path, artifact_path(stored, 0),
                                 stored.snapshots[0]);
            });
    }

    RecoveryActionResult rename_path(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) {
        if (path_contains(source, destination) ||
            path_contains(destination, source)) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          "rename source and destination must not overlap")};
        }
        try {
            if (snapshot_kind(source) == SnapshotKind::missing) {
                return {{},
                        error(RecoveryErrorCode::preparation_failed,
                              "rename source does not exist")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          exception_message(std::current_exception()))};
        }

        return prepare_and_perform(
            RecoveryRecordKind::path_rename, std::nullopt,
            {source, destination}, std::nullopt,
            [&] {
                before(RecoveryStep::mutate_filesystem);
                remove_node(destination);
                before(RecoveryStep::mutate_filesystem);
                std::filesystem::create_directories(source.parent_path());
                std::filesystem::create_directories(destination.parent_path());
                rename_durably(source, destination);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::rollback_filesystem);
                restore_rename(stored);
            });
    }

    RecoveryActionResult delete_path(const std::filesystem::path& path) {
        try {
            if (snapshot_kind(path) == SnapshotKind::missing) {
                return {{},
                        error(RecoveryErrorCode::preparation_failed,
                              "delete target does not exist")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          exception_message(std::current_exception()))};
        }
        return prepare_and_perform(
            RecoveryRecordKind::path_delete, std::nullopt, {path},
            std::nullopt,
            [&] {
                before(RecoveryStep::mutate_filesystem);
                remove_node(path);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::rollback_filesystem);
                restore_snapshot(path, artifact_path(stored, 0),
                                 stored.snapshots[0]);
            });
    }

    RecoveryActionResult replace_workspace(
        const std::filesystem::path& workspace,
        const std::filesystem::path& replacement) {
        try {
            if (snapshot_kind(replacement) == SnapshotKind::missing) {
                return {{},
                        error(RecoveryErrorCode::preparation_failed,
                              "replacement workspace does not exist")};
            }
            if (path_contains(workspace, replacement) ||
                path_contains(replacement, workspace)) {
                return {{},
                        error(RecoveryErrorCode::preparation_failed,
                              "workspace and replacement must not overlap")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          exception_message(std::current_exception()))};
        }

        return prepare_and_perform(
            RecoveryRecordKind::workspace_replace, std::nullopt, {workspace},
            replacement,
            [&] {
                before(RecoveryStep::mutate_filesystem);
                remove_node(workspace);
                before(RecoveryStep::mutate_filesystem);
                const auto installed = records_under_preparation_;
                if (installed == nullptr) {
                    throw std::logic_error(
                        "workspace replacement record is unavailable");
                }
                copy_node(installed->directory / "replacement", workspace,
                          installed->replacement);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::rollback_filesystem);
                restore_snapshot(workspace, artifact_path(stored, 0),
                                 stored.snapshots[0]);
            });
    }

    RecoveryRestoreResult restore_document(
        const RecoveryRecordId& id,
        std::optional<JournalDocument>& document) {
        const auto found = find_record(id);
        if (found == records_.end()) {
            return {error(RecoveryErrorCode::record_not_found,
                          "recovery record was not found")};
        }
        if (!document_kind(found->record.kind)) {
            return {error(RecoveryErrorCode::record_kind_mismatch,
                          "recovery record does not restore a document")};
        }
        try {
            before(RecoveryStep::restore_document);
            if (!restored_in_this_instance(*found)) {
                document = found->document;
                mark_restored(*found);
            }
        } catch (...) {
            return {error(RecoveryErrorCode::restoration_failed,
                          "document restoration failed: " +
                              exception_message(std::current_exception()))};
        }
        return cleanup_restored(found);
    }

    RecoveryRestoreResult restore_filesystem(const RecoveryRecordId& id) {
        const auto found = find_record(id);
        if (found == records_.end()) {
            return {error(RecoveryErrorCode::record_not_found,
                          "recovery record was not found")};
        }
        if (document_kind(found->record.kind)) {
            return {error(RecoveryErrorCode::record_kind_mismatch,
                          "recovery record does not restore filesystem state")};
        }
        if (!found->restored_in_memory &&
            !std::filesystem::exists(restored_marker(*found))) {
            try {
                before(RecoveryStep::restore_filesystem);
                restore_filesystem_state(*found);
                sync_filesystem_state(*found);
                mark_restored(*found);
            } catch (...) {
                return {error(RecoveryErrorCode::restoration_failed,
                              "filesystem restoration failed: " +
                                  exception_message(
                                      std::current_exception()))};
            }
        }
        return cleanup_restored(found);
    }

private:
    using RecordIterator = std::vector<StoredRecord>::iterator;

    void before(RecoveryStep step) {
        if (fault_injector_ != nullptr) fault_injector_->before_step(step);
    }

    void load_records() {
        for (const auto& entry :
             std::filesystem::directory_iterator(recovery_root_)) {
            if (!entry.is_directory()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(".staging-", 0) == 0) {
                remove_node(entry.path());
                continue;
            }
            RecoveryRecordId id{name};
            std::optional<StoredRecord> decoded;
            std::optional<RecordState> state;
            try {
                decoded.emplace(decode_manifest(id, entry.path()));
                state = read_record_state(*decoded);
            } catch (...) {
                remove_node(entry.path());
                continue;
            }
            auto stored = std::move(*decoded);
            if (!state) {
                remove_node(entry.path());
                continue;
            }
            if (*state == RecordState::in_progress &&
                document_kind(stored.record.kind)) {
                remove_node(entry.path());
                continue;
            }
            if (*state == RecordState::in_progress &&
                !document_kind(stored.record.kind) &&
                !std::filesystem::exists(restored_marker(stored))) {
                try {
                    restore_filesystem_state(stored);
                    sync_filesystem_state(stored);
                    mark_restored(stored);
                    remove_node(entry.path());
                    continue;
                } catch (...) {
                }
            }
            records_.push_back(std::move(stored));
            std::uint64_t numeric = 0;
            const auto parsed =
                std::from_chars(name.data(), name.data() + name.size(), numeric);
            if (parsed.ec == std::errc{} &&
                parsed.ptr == name.data() + name.size()) {
                next_id_ = std::max(next_id_, numeric);
            }
        }
        std::sort(records_.begin(), records_.end(),
                  [](const StoredRecord& left, const StoredRecord& right) {
                      return left.record.id.value() <
                             right.record.id.value();
                  });
        while (records_.size() > config_.maximum_records ||
               total_stored_bytes() > config_.maximum_bytes) {
            evict_oldest();
        }
    }

    RecoveryRecordId make_id() {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        next_id_ = std::max(next_id_ + 1, now);
        std::ostringstream encoded;
        encoded << std::setfill('0') << std::setw(20) << next_id_;
        return RecoveryRecordId{encoded.str()};
    }

    std::uintmax_t total_stored_bytes() const {
        std::uintmax_t total = 0;
        for (const auto& record : records_) {
            if (record.record.stored_bytes >
                std::numeric_limits<std::uintmax_t>::max() - total) {
                return std::numeric_limits<std::uintmax_t>::max();
            }
            total += record.record.stored_bytes;
        }
        return total;
    }

    void validate_recovery_separation(
        const std::vector<std::filesystem::path>& paths,
        const std::optional<std::filesystem::path>& replacement) const {
        for (const auto& path : paths) {
            if (path_contains(path, recovery_root_) ||
                path_contains(recovery_root_, path)) {
                throw std::invalid_argument(
                    "recovery root and action path must not overlap");
            }
        }
        if (replacement &&
            (path_contains(*replacement, recovery_root_) ||
             path_contains(recovery_root_, *replacement))) {
            throw std::invalid_argument(
                "recovery root and replacement path must not overlap");
        }
    }

    StoredRecord prepare_record(
        RecoveryRecordKind kind,
        std::optional<JournalDocument> document,
        std::vector<std::filesystem::path> paths,
        const std::optional<std::filesystem::path>& replacement) {
        validate_recovery_separation(paths, replacement);
        before(RecoveryStep::prepare_artifact);

        const auto id = make_id();
        const auto staging =
            recovery_root_ / (".staging-" + std::string{id.value()});
        const auto installed = recovery_root_ / std::string{id.value()};
        remove_node(staging);
        std::filesystem::create_directories(staging / "artifacts");

        StoredRecord stored{
            {id,
             kind,
             0,
             document
                 ? std::optional<JournalDocumentKey>{document->key}
                 : std::nullopt,
             std::move(paths)},
            std::move(document),
            {},
            SnapshotKind::missing,
            installed,
            false};

        try {
            for (std::size_t index = 0;
                 index != stored.record.affected_paths.size(); ++index) {
                const auto kind =
                    snapshot_kind(stored.record.affected_paths[index]);
                stored.snapshots.push_back(kind);
                copy_node(stored.record.affected_paths[index],
                          staging / "artifacts" / std::to_string(index), kind);
            }
            if (replacement) {
                stored.replacement = snapshot_kind(*replacement);
                copy_node(*replacement, staging / "replacement",
                          stored.replacement);
            }

            auto manifest = encode_manifest(stored);
            write_bytes(staging / "manifest.bin", manifest);
            const auto payload_bytes = stored_tree_bytes(staging);
            if (payload_bytes >
                std::numeric_limits<std::uintmax_t>::max() -
                    record_lifecycle_metadata_bytes) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            stored.record.stored_bytes =
                payload_bytes + record_lifecycle_metadata_bytes;
            manifest = encode_manifest(stored);
            write_bytes(staging / "manifest.bin", manifest);
            protect_tree(staging);
            sync_tree(staging);

            before(RecoveryStep::install_record);
            install_directory_durably(staging, installed, recovery_root_);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
            throw;
        }

        if (stored.record.stored_bytes > config_.maximum_bytes) {
            remove_node(installed);
            throw BudgetExceeded(
                "recovery record exceeds the configured byte budget");
        }

        return stored;
    }

    template <typename Mutation, typename Rollback>
    RecoveryActionResult prepare_and_perform(
        RecoveryRecordKind kind,
        std::optional<JournalDocument> document,
        std::vector<std::filesystem::path> paths,
        std::optional<std::filesystem::path> replacement,
        Mutation&& mutation,
        Rollback&& rollback) {
        std::optional<StoredRecord> prepared;
        try {
            prepared.emplace(prepare_record(kind, std::move(document),
                                            std::move(paths), replacement));
        } catch (const BudgetExceeded& failure) {
            return {{},
                    error(RecoveryErrorCode::budget_exceeded, failure.what())};
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          "recovery record preparation failed: " +
                              exception_message(std::current_exception()))};
        }

        try {
            mark_record_state(*prepared, RecordState::in_progress);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(prepared->directory, ignored);
            return {{},
                    error(RecoveryErrorCode::preparation_failed,
                          "recovery record activation failed: " +
                              exception_message(std::current_exception()))};
        }

        records_under_preparation_ = &*prepared;
        std::exception_ptr action_failure;
        try {
            mutation();
            if (!document_kind(prepared->record.kind)) {
                sync_filesystem_state(*prepared);
            }
            before(RecoveryStep::publish_record);
            mark_record_state(*prepared, RecordState::published);
            before(RecoveryStep::publish_record);
        } catch (...) {
            action_failure = std::current_exception();
        }
        records_under_preparation_ = nullptr;

        if (!action_failure) {
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            if (const auto cleanup_failure = enforce_budgets()) {
                return {id,
                        error(RecoveryErrorCode::cleanup_failed,
                              "recovery action succeeded, but old record "
                              "eviction failed: " +
                                  *cleanup_failure)};
            }
            return {id, {}};
        }

        try {
            mark_record_state(*prepared, RecordState::in_progress);
            rollback(*prepared);
            if (!document_kind(prepared->record.kind)) {
                sync_filesystem_state(*prepared);
            }
        } catch (...) {
            const auto rollback_failure = std::current_exception();
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            auto rollback_message = exception_message(rollback_failure);
            if (const auto cleanup_failure = enforce_budgets()) {
                rollback_message +=
                    "; old record eviction failed: " + *cleanup_failure;
            }
            return {id,
                    error(RecoveryErrorCode::action_and_rollback_failed,
                          "recovery action failed: " +
                              exception_message(action_failure),
                          std::move(rollback_message))};
        }

        try {
            before(RecoveryStep::cleanup_record);
            mark_restored(*prepared);
            remove_node(prepared->directory);
            return {{},
                    error(RecoveryErrorCode::action_failed,
                          "recovery action failed: " +
                              exception_message(action_failure))};
        } catch (...) {
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            return {id,
                    error(RecoveryErrorCode::cleanup_failed,
                          "recovery action failed and was rolled back, but "
                          "record cleanup failed: " +
                              exception_message(std::current_exception()))};
        }
    }

    void evict_oldest() {
        if (records_.empty()) {
            throw BudgetExceeded(
                "recovery record cannot fit the configured budgets");
        }
        remove_node(records_.front().directory);
        records_.erase(records_.begin());
    }

    std::optional<std::string> enforce_budgets() {
        while (records_.size() > config_.maximum_records ||
               total_stored_bytes() > config_.maximum_bytes) {
            try {
                evict_oldest();
            } catch (...) {
                return exception_message(std::current_exception());
            }
        }
        return std::nullopt;
    }

    static std::filesystem::path artifact_path(const StoredRecord& stored,
                                               std::size_t index) {
        return stored.directory / "artifacts" / std::to_string(index);
    }

    static std::filesystem::path restored_marker(
        const StoredRecord& stored) {
        return stored.directory / "restored";
    }

    static std::filesystem::path state_path(const StoredRecord& stored) {
        return stored.directory / "state";
    }

    static std::optional<RecordState> read_record_state(
        const StoredRecord& stored) {
        std::error_code error;
        if (!std::filesystem::exists(state_path(stored), error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "failed to inspect recovery record state",
                    state_path(stored), error);
            }
            return std::nullopt;
        }
        const auto encoded = read_bytes(state_path(stored));
        if (encoded.size() != 1) {
            throw std::runtime_error("invalid recovery record state");
        }
        const auto value =
            std::to_integer<std::uint8_t>(encoded.front());
        if (value != static_cast<std::uint8_t>(RecordState::in_progress) &&
            value != static_cast<std::uint8_t>(RecordState::published)) {
            throw std::runtime_error("invalid recovery record state");
        }
        return static_cast<RecordState>(value);
    }

    static void mark_record_state(const StoredRecord& stored,
                                  RecordState state) {
        const std::array encoded{
            static_cast<std::byte>(static_cast<std::uint8_t>(state))};
        replace_file_atomically(state_path(stored), encoded);
        set_owner_only_permissions(state_path(stored));
    }

    void mark_restored(StoredRecord& stored) {
        replace_file_atomically(restored_marker(stored), instance_id_);
        set_owner_only_permissions(restored_marker(stored));
        stored.restored_in_memory = true;
    }

    bool restored_in_this_instance(const StoredRecord& stored) const {
        if (stored.restored_in_memory) return true;
        std::error_code error;
        if (!std::filesystem::exists(restored_marker(stored), error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "failed to inspect restored recovery marker",
                    restored_marker(stored), error);
            }
            return false;
        }
        return read_bytes(restored_marker(stored)) ==
               std::vector<std::byte>(instance_id_.begin(),
                                      instance_id_.end());
    }

    void restore_rename(const StoredRecord& stored) {
        const auto& source = stored.record.affected_paths[0];
        const auto& destination = stored.record.affected_paths[1];
        const auto source_now = snapshot_kind(source);
        const auto destination_now = snapshot_kind(destination);
        if (source_now == SnapshotKind::missing &&
            destination_now != SnapshotKind::missing) {
            remove_node(source);
            std::filesystem::create_directories(source.parent_path());
            rename_durably(destination, source);
        } else if (source_now == SnapshotKind::missing) {
            restore_snapshot(source, artifact_path(stored, 0),
                             stored.snapshots[0]);
        }
        before(RecoveryStep::restore_filesystem);
        restore_snapshot(destination, artifact_path(stored, 1),
                         stored.snapshots[1]);
        before(RecoveryStep::restore_filesystem);
    }

    void restore_filesystem_state(const StoredRecord& stored) {
        switch (stored.record.kind) {
        case RecoveryRecordKind::file_overwrite:
        case RecoveryRecordKind::path_delete:
        case RecoveryRecordKind::workspace_replace:
            restore_snapshot(stored.record.affected_paths[0],
                             artifact_path(stored, 0), stored.snapshots[0]);
            return;
        case RecoveryRecordKind::path_rename:
            restore_rename(stored);
            return;
        case RecoveryRecordKind::document_close:
        case RecoveryRecordKind::document_reload:
            throw std::logic_error(
                "document recovery record used for filesystem restoration");
        }
    }

    static void sync_filesystem_state(const StoredRecord& stored) {
        for (const auto& path : stored.record.affected_paths) {
            const auto kind = snapshot_kind(path);
            if (kind == SnapshotKind::regular_file) {
                sync_path(path, false);
            } else if (kind == SnapshotKind::directory) {
                sync_tree(path);
            }
            sync_path(std::filesystem::absolute(path).parent_path(), true);
        }
    }

    RecordIterator find_record(const RecoveryRecordId& id) {
        return std::find_if(records_.begin(), records_.end(),
                            [&](const StoredRecord& stored) {
                                return stored.record.id == id;
                            });
    }

    RecoveryRestoreResult cleanup_restored(RecordIterator record) {
        try {
            before(RecoveryStep::cleanup_record);
            remove_node(record->directory);
            records_.erase(record);
            return {};
        } catch (...) {
            return {error(RecoveryErrorCode::cleanup_failed,
                          "recovery cleanup failed: " +
                              exception_message(std::current_exception()))};
        }
    }

    std::filesystem::path recovery_root_;
    RecoveryConfig config_;
    RecoveryFaultInjector* fault_injector_;
    std::vector<StoredRecord> records_;
    std::uint64_t next_id_ = 0;
    std::array<std::byte, 16> instance_id_ =
        UntitledDocumentId::generate().bytes();
    StoredRecord* records_under_preparation_ = nullptr;
};

RecoveryActions RecoveryActions::create(
    const std::filesystem::path& recovery_root,
    RecoveryConfig config) {
    return RecoveryActions{
        std::make_unique<Impl>(recovery_root, config, nullptr)};
}

RecoveryActions RecoveryActions::create(
    const std::filesystem::path& recovery_root,
    RecoveryConfig config,
    RecoveryFaultInjector& fault_injector) {
    return RecoveryActions{
        std::make_unique<Impl>(recovery_root, config, &fault_injector)};
}

RecoveryActions::RecoveryActions(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

RecoveryActions::~RecoveryActions() = default;
RecoveryActions::RecoveryActions(RecoveryActions&&) noexcept = default;
RecoveryActions& RecoveryActions::operator=(RecoveryActions&&) noexcept =
    default;

std::vector<RecoveryRecord> RecoveryActions::records() const {
    return impl_->records();
}

RecoveryActionResult RecoveryActions::close_document(
    std::optional<JournalDocument>& document,
    ScratchStore& scratch,
    std::chrono::milliseconds durability_timeout) {
    return impl_->close_document(document, scratch, durability_timeout);
}

RecoveryActionResult RecoveryActions::reload_document(
    std::optional<JournalDocument>& document,
    JournalDocument replacement) {
    return impl_->reload_document(document, std::move(replacement));
}

RecoveryActionResult RecoveryActions::overwrite_file(
    const std::filesystem::path& path,
    std::span<const std::byte> replacement) {
    return impl_->overwrite_file(path, replacement);
}

RecoveryActionResult RecoveryActions::rename_path(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    return impl_->rename_path(source, destination);
}

RecoveryActionResult RecoveryActions::delete_path(
    const std::filesystem::path& path) {
    return impl_->delete_path(path);
}

RecoveryActionResult RecoveryActions::replace_workspace(
    const std::filesystem::path& workspace,
    const std::filesystem::path& replacement) {
    return impl_->replace_workspace(workspace, replacement);
}

RecoveryRestoreResult RecoveryActions::restore_document(
    const RecoveryRecordId& record,
    std::optional<JournalDocument>& document) {
    return impl_->restore_document(record, document);
}

RecoveryRestoreResult RecoveryActions::restore_filesystem(
    const RecoveryRecordId& record) {
    return impl_->restore_filesystem(record);
}

} // namespace ssg
