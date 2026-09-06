#include <ssg/RecoveryManager.h>

#include <ssg/platform_files.h>

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
#include <utility>
#include <vector>

namespace ssg {
namespace {

constexpr std::array<std::byte, 8> kManifestMagic{
    std::byte{'S'}, std::byte{'S'}, std::byte{'G'}, std::byte{'R'},
    std::byte{'E'}, std::byte{'C'}, std::byte{1},   std::byte{0}};
constexpr std::uintmax_t kRecordLifecycleMetadataBytes = 17;

enum class SnapshotKind : std::uint8_t {
    Missing,
    RegularFile,
    Directory,
    Symlink,
};

enum class RecordState : std::uint8_t {
    InProgress = 1,
    Published = 2,
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

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    std::transform(value.begin(), value.end(), std::back_inserter(encoded),
                   [](char byte) {
                       return static_cast<char8_t>(
                           static_cast<unsigned char>(byte));
                   });
    return std::filesystem::path{encoded};
}

// Not routed to the seam's write primitives: these writes land in a STAGING
// directory whose durability is established afterwards by syncTree() before the
// record is installed. An atomic replace here would add a rename and a parent
// fsync in the middle of that two-phase commit, reordering the very sequence
// the recovery design depends on. Durability is not weaker; it is established
// one level up.
void writeBytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
    // seam-exempt: staged write, durability established by syncTree below
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

void syncPath(const std::filesystem::path& path, bool directory) {
    const auto result = directory ? syncDirectory(path) : syncFile(path);
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
}

void syncTree(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> directories;
    directories.push_back(root);
    const auto listed =
        listDirectory(root, DirectoryTraversal::Recursive);
    if (!listed.ok() || !listed.complete) {
        throw std::runtime_error("failed to list recovery tree: " +
                                 listed.message);
    }
    for (const auto& entry : listed.entries) {
        const auto status = statFile(entry.path());
        if (!status) continue;
        if (status->kind == FileKind::Regular) {
            syncPath(entry.path(), false);
        } else if (status->kind == FileKind::Directory) {
            directories.push_back(entry.path());
        }
    }
    for (auto directory = directories.rbegin();
         directory != directories.rend(); ++directory) {
        syncPath(*directory, true);
    }
}

void installDirectoryDurably(const std::filesystem::path& staging,
                               const std::filesystem::path& installed,
                               const std::filesystem::path& parent) {
    (void)parent;
    const auto result = renamePathDurably(staging, installed);
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
}

void renameDurably(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
    const auto result = renamePathDurably(source, destination);
    if (!result.ok()) {
        throw std::runtime_error(result.message);
    }
}

std::vector<std::byte> readBytes(const std::filesystem::path& path) {
    auto result = readFile(path);
    if (!result.ok()) {
        throw std::runtime_error("failed to open recovery file: " +
                                 path.string() + ": " + result.message);
    }
    std::vector<std::byte> bytes(result.bytes.size());
    std::transform(result.bytes.begin(), result.bytes.end(), bytes.begin(),
                   [](std::uint8_t value) {
                       return static_cast<std::byte>(value);
                   });
    return bytes;
}

SnapshotKind snapshotKind(const std::filesystem::path& path) {
    const auto status = statFile(path);
    if (!status) return SnapshotKind::Missing;
    if (status->kind == FileKind::Symlink) return SnapshotKind::Symlink;
    if (status->kind == FileKind::Regular) {
        return SnapshotKind::RegularFile;
    }
    if (status->kind == FileKind::Directory) {
        return SnapshotKind::Directory;
    }
    throw std::runtime_error("unsupported filesystem node in recovery action: " +
                             path.string());
}

void createDirectories(const std::filesystem::path& path) {
    const auto created = createDirectoriesDurably(path);
    if (!created.ok() && created.status != FileIoStatus::AlreadyExists) {
        throw std::runtime_error(created.message);
    }
}

void copyNode(const std::filesystem::path& source,
               const std::filesystem::path& destination,
               SnapshotKind kind) {
    switch (kind) {
    case SnapshotKind::Missing:
        return;
    case SnapshotKind::RegularFile:
        createDirectories(destination.parent_path());
        // seam-exempt: restoring a recovery snapshot MUST overwrite the current file
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing);
        return;
    case SnapshotKind::Symlink: {
        createDirectories(destination.parent_path());
        const auto target = std::filesystem::read_symlink(source);
        std::error_code targetError;
        const auto targetPath = std::filesystem::canonical(source, targetError);
        const auto targetStat =
            targetError ? std::optional<FileStat>{} : statFile(targetPath);
        const bool directoryTarget =
            targetStat && targetStat->kind == FileKind::Directory;
        if (directoryTarget) {
            std::filesystem::create_directory_symlink(target, destination);
        } else {
            std::filesystem::create_symlink(target, destination);
        }
        return;
    }
    case SnapshotKind::Directory:
        createDirectories(destination);
        const auto listed = listDirectory(source);
        if (!listed.ok() || !listed.complete) {
            throw std::runtime_error("failed to list recovery source: " +
                                     listed.message);
        }
        for (const auto& entry : listed.entries) {
            const auto childKind = snapshotKind(entry.path());
            copyNode(entry.path(), destination / entry.path().filename(),
                      childKind);
        }
        return;
    }
}

void removeNode(const std::filesystem::path& path) {
    const auto removed = removeTree(path);
    if (!removed.ok() && removed.status != FileIoStatus::NotFound) {
        throw std::runtime_error("failed to remove filesystem node: " +
                                 removed.message);
    }
}

void restoreSnapshot(const std::filesystem::path& destination,
                      const std::filesystem::path& artifact,
                      SnapshotKind kind) {
    removeNode(destination);
    if (kind != SnapshotKind::Missing) {
        copyNode(artifact, destination, kind);
    }
}

std::uintmax_t storedTreeBytes(const std::filesystem::path& root) {
    std::uintmax_t total = 0;
    const auto addNode = [&](const std::filesystem::path& path) {
        const auto status = statFile(path);
        if (!status) return;
        if (status->kind == FileKind::Regular) {
            const auto size = status->size;
            if (size > std::numeric_limits<std::uintmax_t>::max() - total) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            total += size;
        } else if (status->kind == FileKind::Symlink) {
            const auto size = pathToUtf8(std::filesystem::read_symlink(path))
                                  .size();
            if (size > std::numeric_limits<std::uintmax_t>::max() - total) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            total += size;
        }
    };

    addNode(root);
    if (snapshotKind(root) == SnapshotKind::Directory) {
        const auto listed =
            listDirectory(root, DirectoryTraversal::Recursive);
        if (!listed.ok() || !listed.complete) {
            throw std::runtime_error("failed to list recovery tree: " +
                                     listed.message);
        }
        for (const auto& entry : listed.entries) {
            addNode(entry.path());
        }
    }
    return total;
}

void protectTree(const std::filesystem::path& root) {
    setOwnerOnlyPermissions(root);
    const auto listed =
        listDirectory(root, DirectoryTraversal::Recursive);
    if (!listed.ok() || !listed.complete) {
        throw std::runtime_error("failed to list recovery tree: " +
                                 listed.message);
    }
    for (const auto& entry : listed.entries) {
        const auto status = statFile(entry.path());
        if (status && status->kind != FileKind::Symlink) {
            setOwnerOnlyPermissions(entry.path());
        }
    }
}

bool pathComponentEqual(const std::filesystem::path& left,
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

bool pathContains(const std::filesystem::path& parent,
                   const std::filesystem::path& child) {
    const auto normalizedParent =
        std::filesystem::absolute(parent).lexically_normal();
    const auto normalizedChild =
        std::filesystem::absolute(child).lexically_normal();
    auto parentPart = normalizedParent.begin();
    auto childPart = normalizedChild.begin();
    for (; parentPart != normalizedParent.end();
         ++parentPart, ++childPart) {
        if (childPart == normalizedChild.end() ||
            !pathComponentEqual(*parentPart, *childPart)) {
            return false;
        }
    }
    return true;
}

std::string exceptionMessage(std::exception_ptr failure) {
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

bool documentKind(RecoveryRecordKind kind) {
    return kind == RecoveryRecordKind::DocumentClose;
}

bool validRecordKind(std::uint8_t kind) {
    switch (static_cast<RecoveryRecordKind>(kind)) {
    case RecoveryRecordKind::DocumentClose:
    case RecoveryRecordKind::PathRename:
    case RecoveryRecordKind::PathDelete: return true;
    }
    return false;
}

bool validSnapshotKind(std::uint8_t kind) {
    return kind <= static_cast<std::uint8_t>(SnapshotKind::Symlink);
}

struct StoredRecord {
    RecoveryRecord record;
    std::optional<JournalDocument> document;
    std::vector<SnapshotKind> snapshots;
    std::filesystem::path directory;
    bool restoredInMemory = false;
};

std::vector<std::byte> encodeManifest(const StoredRecord& stored) {
    ByteWriter writer;
    writer.raw(kManifestMagic);
    writer.u8(static_cast<std::uint8_t>(stored.record.kind));
    writer.u64(stored.record.storedBytes);

    if (stored.document) {
        writer.u8(1);
        const auto encoded =
            encodeJournalCheckpoint({std::vector<JournalDocument>{
                *stored.document}});
        if (encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("recovery document is too large");
        }
        writer.u32(static_cast<std::uint32_t>(encoded.size()));
        writer.raw(encoded);
    } else {
        writer.u8(0);
    }

    if (stored.record.affectedPaths.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many recovery paths");
    }
    writer.u32(
        static_cast<std::uint32_t>(stored.record.affectedPaths.size()));
    for (std::size_t index = 0;
         index != stored.record.affectedPaths.size(); ++index) {
        writer.string(pathToUtf8(stored.record.affectedPaths[index]));
        writer.u8(static_cast<std::uint8_t>(stored.snapshots[index]));
    }
    return writer.take();
}

StoredRecord decodeManifest(const RecoveryRecordId& id,
                             const std::filesystem::path& directory) {
    const auto encoded = readBytes(directory / "manifest.bin");
    ByteReader reader(encoded);
    std::span<const std::byte> magic;
    if (!reader.raw(kManifestMagic.size(), magic) ||
        !std::equal(magic.begin(), magic.end(), kManifestMagic.begin())) {
        throw std::runtime_error("invalid recovery manifest magic");
    }

    std::uint8_t encodedKind = 0;
    std::uint64_t storedBytes = 0;
    std::uint8_t hasDocument = 0;
    if (!reader.u8(encodedKind) || !validRecordKind(encodedKind) ||
        !reader.u64(storedBytes) || !reader.u8(hasDocument) ||
        hasDocument > 1) {
        throw std::runtime_error("invalid recovery manifest header");
    }

    std::optional<JournalDocument> document;
    if (hasDocument != 0) {
        std::uint32_t documentSize = 0;
        std::span<const std::byte> documentBytes;
        if (!reader.u32(documentSize) ||
            !reader.raw(documentSize, documentBytes)) {
            throw std::runtime_error("truncated recovery document");
        }
        const auto replayed = replayJournal(documentBytes);
        if (replayed.discardedTail ||
            replayed.validBytes != documentBytes.size() ||
            replayed.recovery.documents.size() != 1) {
            throw std::runtime_error("invalid recovery document");
        }
        document = replayed.recovery.documents.front();
    }

    std::uint32_t pathCount = 0;
    if (!reader.u32(pathCount)) {
        throw std::runtime_error("truncated recovery path list");
    }
    std::vector<std::filesystem::path> paths;
    std::vector<SnapshotKind> snapshots;
    paths.reserve(pathCount);
    snapshots.reserve(pathCount);
    for (std::uint32_t index = 0; index != pathCount; ++index) {
        std::string path;
        std::uint8_t kind = 0;
        if (!reader.string(path) || !reader.u8(kind) ||
            !validSnapshotKind(kind)) {
            throw std::runtime_error("invalid recovery path entry");
        }
        paths.push_back(pathFromUtf8(path));
        snapshots.push_back(static_cast<SnapshotKind>(kind));
    }

    if (!reader.empty()) {
        // Manifests written before workspace replacement was removed carry one
        // unused replacement-kind byte for every record.
        std::uint8_t legacyReplacement = 0;
        if (!reader.u8(legacyReplacement) ||
            !validSnapshotKind(legacyReplacement) || !reader.empty()) {
            throw std::runtime_error("invalid recovery manifest tail");
        }
    }

    const auto kind = static_cast<RecoveryRecordKind>(encodedKind);
    if (documentKind(kind) != document.has_value()) {
        throw std::runtime_error("recovery record payload does not match kind");
    }
    return {{id,
             kind,
             storedBytes,
             document ? std::optional<JournalDocumentKey>{document->key}
                      : std::nullopt,
             std::move(paths)},
            std::move(document),
            std::move(snapshots),
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

class RecoveryManager::Impl {
public:
    Impl(std::filesystem::path recoveryRoot,
         RecoveryConfig config,
         RecoveryFaultInjector* faultInjector)
        : recoveryRoot_(
              std::filesystem::absolute(std::move(recoveryRoot))
                  .lexically_normal()),
          config_(config),
          faultInjector_(faultInjector) {
        if (config_.maximumRecords == 0) {
            throw std::invalid_argument(
                "recovery maximum record count must be greater than zero");
        }
        if (config_.maximumBytes == 0) {
            throw std::invalid_argument(
                "recovery maximum byte count must be greater than zero");
        }
        createDirectories(recoveryRoot_);
        setOwnerOnlyPermissions(recoveryRoot_);
        loadRecords();
    }

    [[nodiscard]] std::vector<RecoveryRecord> records() const {
        std::vector<RecoveryRecord> result;
        result.reserve(records_.size());
        for (const auto& stored : records_) result.push_back(stored.record);
        return result;
    }

    RecoveryActionResult closeDocument(
        std::optional<JournalDocument>& document,
        ScratchStore& scratch,
        std::chrono::milliseconds durabilityTimeout) {
        if (!document) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          "cannot close an absent document")};
        }
        if (durabilityTimeout <= std::chrono::milliseconds::zero()) {
            return {{},
                    error(RecoveryErrorCode::DurabilityFailed,
                          "dirty close requires a finite positive durability "
                          "timeout")};
        }
        if (document->dirty) {
            try {
                scratch.updateDocument(*document);
                if (!scratch.waitUntilDurable(durabilityTimeout)) {
                    const auto state = scratch.durabilityState();
                    const auto detail =
                        state.failure.empty() ? "durability timed out"
                                              : state.failure;
                    return {{},
                            error(RecoveryErrorCode::DurabilityFailed,
                                  "dirty close rejected: " + detail)};
                }
            } catch (...) {
                return {{},
                        error(RecoveryErrorCode::DurabilityFailed,
                              "dirty close rejected: " +
                                  exceptionMessage(
                                      std::current_exception()))};
            }
        }

        const auto previous = *document;
        return prepareAndPerform(
            RecoveryRecordKind::DocumentClose, previous, {},
            [&] {
                before(RecoveryStep::MutateDocument);
                document.reset();
            },
            [&](const StoredRecord&) {
                before(RecoveryStep::RollbackDocument);
                document = previous;
            });
    }

    RecoveryActionResult renamePath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) {
        if (pathContains(source, destination) ||
            pathContains(destination, source)) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          "rename source and destination must not overlap")};
        }
        try {
            if (snapshotKind(source) == SnapshotKind::Missing) {
                return {{},
                        error(RecoveryErrorCode::PreparationFailed,
                              "rename source does not exist")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          exceptionMessage(std::current_exception()))};
        }

        return prepareAndPerform(
            RecoveryRecordKind::PathRename, std::nullopt,
            {source, destination},
            [&] {
                before(RecoveryStep::MutateFilesystem);
                removeNode(destination);
                before(RecoveryStep::MutateFilesystem);
                createDirectories(source.parent_path());
                createDirectories(destination.parent_path());
                renameDurably(source, destination);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::RollbackFilesystem);
                restoreRename(stored);
            });
    }

    RecoveryActionResult renamePathNoClobber(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) {
        if (pathContains(source, destination) ||
            pathContains(destination, source)) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          "rename source and destination must not overlap")};
        }
        try {
            if (snapshotKind(source) == SnapshotKind::Missing) {
                return {{},
                        error(RecoveryErrorCode::PreparationFailed,
                              "rename source does not exist")};
            }
            // An early, friendly rejection. It is NOT what makes the rename
            // safe -- renameFileNoClobber below is, because the kernel decides.
            // Snapshotting proceeds with the destination recorded as missing,
            // which is what lets a rollback remove it rather than restore
            // something that was never there.
            if (snapshotKind(destination) != SnapshotKind::Missing) {
                return {{},
                        error(RecoveryErrorCode::PreparationFailed,
                              "rename destination already exists")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          exceptionMessage(std::current_exception()))};
        }

        return prepareAndPerform(
            RecoveryRecordKind::PathRename, std::nullopt,
            {source, destination},
            [&] {
                before(RecoveryStep::MutateFilesystem);
                createDirectories(destination.parent_path());
                const auto renamed = renameFileNoClobber(source, destination);
                if (!renamed.ok()) {
                    throw std::runtime_error("rename failed: " +
                                             renamed.message);
                }
                syncPath(std::filesystem::absolute(source).parent_path(), true);
                const auto sourceParent =
                    std::filesystem::absolute(source).parent_path()
                        .lexically_normal();
                const auto destinationParent =
                    std::filesystem::absolute(destination).parent_path()
                        .lexically_normal();
                if (destinationParent != sourceParent) {
                    syncPath(destinationParent, true);
                }
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::RollbackFilesystem);
                restoreRename(stored);
            });
    }

    RecoveryActionResult deletePath(const std::filesystem::path& path) {
        try {
            if (snapshotKind(path) == SnapshotKind::Missing) {
                return {{},
                        error(RecoveryErrorCode::PreparationFailed,
                              "delete target does not exist")};
            }
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          exceptionMessage(std::current_exception()))};
        }
        return prepareAndPerform(
            RecoveryRecordKind::PathDelete, std::nullopt, {path},
            [&] {
                before(RecoveryStep::MutateFilesystem);
                removeNode(path);
            },
            [&](const StoredRecord& stored) {
                before(RecoveryStep::RollbackFilesystem);
                restoreSnapshot(path, artifactPath(stored, 0),
                                 stored.snapshots[0]);
            });
    }

    RecoveryRestoreResult restoreDocument(
        const RecoveryRecordId& id,
        std::optional<JournalDocument>& document) {
        const auto found = findRecord(id);
        if (found == records_.end()) {
            return {error(RecoveryErrorCode::RecordNotFound,
                          "recovery record was not found")};
        }
        if (!documentKind(found->record.kind)) {
            return {error(RecoveryErrorCode::RecordKindMismatch,
                          "recovery record does not restore a document")};
        }
        try {
            before(RecoveryStep::RestoreDocument);
            if (!restoredInThisInstance(*found)) {
                document = found->document;
                markRestored(*found);
            }
        } catch (...) {
            return {error(RecoveryErrorCode::RestorationFailed,
                          "document restoration failed: " +
                              exceptionMessage(std::current_exception()))};
        }
        return cleanupRestored(found);
    }

    RecoveryRestoreResult restoreFilesystem(const RecoveryRecordId& id) {
        const auto found = findRecord(id);
        if (found == records_.end()) {
            return {error(RecoveryErrorCode::RecordNotFound,
                          "recovery record was not found")};
        }
        if (documentKind(found->record.kind)) {
            return {error(RecoveryErrorCode::RecordKindMismatch,
                          "recovery record does not restore filesystem state")};
        }
        if (!found->restoredInMemory && !statFile(restoredMarker(*found))) {
            try {
                before(RecoveryStep::RestoreFilesystem);
                restoreFilesystemState(*found);
                syncFilesystemState(*found);
                markRestored(*found);
            } catch (...) {
                return {error(RecoveryErrorCode::RestorationFailed,
                              "filesystem restoration failed: " +
                                  exceptionMessage(
                                      std::current_exception()))};
            }
        }
        return cleanupRestored(found);
    }

private:
    using RecordIterator = std::vector<StoredRecord>::iterator;

    void before(RecoveryStep step) {
        if (faultInjector_ != nullptr) faultInjector_->beforeStep(step);
    }

    void loadRecords() {
        const auto listed = listDirectory(recoveryRoot_);
        if (!listed.ok() || !listed.complete) {
            throw std::runtime_error("failed to list recovery records: " +
                                     listed.message);
        }
        for (const auto& entry : listed.entries) {
            const auto status = statFile(entry.path());
            if (!status || status->kind != FileKind::Directory) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(".staging-", 0) == 0) {
                removeNode(entry.path());
                continue;
            }
            RecoveryRecordId id{name};
            std::optional<StoredRecord> decoded;
            std::optional<RecordState> state;
            try {
                decoded.emplace(decodeManifest(id, entry.path()));
                state = readRecordState(*decoded);
            } catch (...) {
                removeNode(entry.path());
                continue;
            }
            auto stored = std::move(*decoded);
            if (!state) {
                removeNode(entry.path());
                continue;
            }
            if (*state == RecordState::InProgress &&
                documentKind(stored.record.kind)) {
                removeNode(entry.path());
                continue;
            }
            if (*state == RecordState::InProgress &&
                !documentKind(stored.record.kind) &&
                !statFile(restoredMarker(stored))) {
                try {
                    restoreFilesystemState(stored);
                    syncFilesystemState(stored);
                    markRestored(stored);
                    removeNode(entry.path());
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
                nextId_ = std::max(nextId_, numeric);
            }
        }
        std::sort(records_.begin(), records_.end(),
                  [](const StoredRecord& left, const StoredRecord& right) {
                      return left.record.id.value() <
                             right.record.id.value();
                  });
        while (records_.size() > config_.maximumRecords ||
               totalStoredBytes() > config_.maximumBytes) {
            evictOldest();
        }
    }

    RecoveryRecordId makeId() {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        nextId_ = std::max(nextId_ + 1, now);
        std::ostringstream encoded;
        encoded << std::setfill('0') << std::setw(20) << nextId_;
        return RecoveryRecordId{encoded.str()};
    }

    std::uintmax_t totalStoredBytes() const {
        std::uintmax_t total = 0;
        for (const auto& record : records_) {
            if (record.record.storedBytes >
                std::numeric_limits<std::uintmax_t>::max() - total) {
                return std::numeric_limits<std::uintmax_t>::max();
            }
            total += record.record.storedBytes;
        }
        return total;
    }

    void validateRecoverySeparation(
        const std::vector<std::filesystem::path>& paths) const {
        for (const auto& path : paths) {
            if (pathContains(path, recoveryRoot_) ||
                pathContains(recoveryRoot_, path)) {
                throw std::invalid_argument(
                    "recovery root and action path must not overlap");
            }
        }
    }

    StoredRecord prepareRecord(
        RecoveryRecordKind kind,
        std::optional<JournalDocument> document,
        std::vector<std::filesystem::path> paths) {
        validateRecoverySeparation(paths);
        before(RecoveryStep::PrepareArtifact);

        const auto id = makeId();
        const auto staging =
            recoveryRoot_ / (".staging-" + std::string{id.value()});
        const auto installed = recoveryRoot_ / std::string{id.value()};
        removeNode(staging);
        createDirectories(staging / "artifacts");

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
            installed,
            false};

        try {
            for (std::size_t index = 0;
                 index != stored.record.affectedPaths.size(); ++index) {
                const auto kind =
                    snapshotKind(stored.record.affectedPaths[index]);
                stored.snapshots.push_back(kind);
                copyNode(stored.record.affectedPaths[index],
                          staging / "artifacts" / std::to_string(index), kind);
            }
            auto manifest = encodeManifest(stored);
            writeBytes(staging / "manifest.bin", manifest);
            const auto payloadBytes = storedTreeBytes(staging);
            if (payloadBytes >
                std::numeric_limits<std::uintmax_t>::max() -
                    kRecordLifecycleMetadataBytes) {
                throw std::overflow_error("recovery byte accounting overflow");
            }
            stored.record.storedBytes =
                payloadBytes + kRecordLifecycleMetadataBytes;
            manifest = encodeManifest(stored);
            writeBytes(staging / "manifest.bin", manifest);
            protectTree(staging);
            syncTree(staging);

            before(RecoveryStep::InstallRecord);
            installDirectoryDurably(staging, installed, recoveryRoot_);
        } catch (...) {
            (void)removeTree(staging);
            throw;
        }

        if (stored.record.storedBytes > config_.maximumBytes) {
            removeNode(installed);
            throw BudgetExceeded(
                "recovery record exceeds the configured byte budget");
        }

        return stored;
    }

    template <typename Mutation, typename Rollback>
    RecoveryActionResult prepareAndPerform(
        RecoveryRecordKind kind,
        std::optional<JournalDocument> document,
        std::vector<std::filesystem::path> paths,
        Mutation&& mutation,
        Rollback&& rollback) {
        std::optional<StoredRecord> prepared;
        try {
            prepared.emplace(prepareRecord(kind, std::move(document),
                                            std::move(paths)));
        } catch (const BudgetExceeded& failure) {
            return {{},
                    error(RecoveryErrorCode::BudgetExceeded, failure.what())};
        } catch (...) {
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          "recovery record preparation failed: " +
                              exceptionMessage(std::current_exception()))};
        }

        try {
            markRecordState(*prepared, RecordState::InProgress);
        } catch (...) {
            (void)removeTree(prepared->directory);
            return {{},
                    error(RecoveryErrorCode::PreparationFailed,
                          "recovery record activation failed: " +
                              exceptionMessage(std::current_exception()))};
        }


        std::exception_ptr actionFailure;
        try {
            mutation();
            if (!documentKind(prepared->record.kind)) {
                syncFilesystemState(*prepared);
            }
            before(RecoveryStep::PublishRecord);
            markRecordState(*prepared, RecordState::Published);
            before(RecoveryStep::PublishRecord);
        } catch (...) {
            actionFailure = std::current_exception();
        }


        if (!actionFailure) {
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            if (const auto cleanupFailure = enforceBudgets()) {
                return {id,
                        error(RecoveryErrorCode::CleanupFailed,
                              "recovery action succeeded, but old record "
                              "eviction failed: " +
                                  *cleanupFailure)};
            }
            return {id, {}};
        }

        try {
            markRecordState(*prepared, RecordState::InProgress);
            rollback(*prepared);
            if (!documentKind(prepared->record.kind)) {
                syncFilesystemState(*prepared);
            }
        } catch (...) {
            const auto rollbackFailure = std::current_exception();
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            auto rollbackMessage = exceptionMessage(rollbackFailure);
            if (const auto cleanupFailure = enforceBudgets()) {
                rollbackMessage +=
                    "; old record eviction failed: " + *cleanupFailure;
            }
            return {id,
                    error(RecoveryErrorCode::ActionAndRollbackFailed,
                          "recovery action failed: " +
                              exceptionMessage(actionFailure),
                          std::move(rollbackMessage))};
        }

        try {
            before(RecoveryStep::CleanupRecord);
            markRestored(*prepared);
            removeNode(prepared->directory);
            return {{},
                    error(RecoveryErrorCode::ActionFailed,
                          "recovery action failed: " +
                              exceptionMessage(actionFailure))};
        } catch (...) {
            const auto id = prepared->record.id;
            records_.push_back(std::move(*prepared));
            return {id,
                    error(RecoveryErrorCode::CleanupFailed,
                          "recovery action failed and was rolled back, but "
                          "record cleanup failed: " +
                              exceptionMessage(std::current_exception()))};
        }
    }

    void evictOldest() {
        if (records_.empty()) {
            throw BudgetExceeded(
                "recovery record cannot fit the configured budgets");
        }
        removeNode(records_.front().directory);
        records_.erase(records_.begin());
    }

    std::optional<std::string> enforceBudgets() {
        while (records_.size() > config_.maximumRecords ||
               totalStoredBytes() > config_.maximumBytes) {
            try {
                evictOldest();
            } catch (...) {
                return exceptionMessage(std::current_exception());
            }
        }
        return std::nullopt;
    }

    static std::filesystem::path artifactPath(const StoredRecord& stored,
                                               std::size_t index) {
        return stored.directory / "artifacts" / std::to_string(index);
    }

    static std::filesystem::path restoredMarker(
        const StoredRecord& stored) {
        return stored.directory / "restored";
    }

    static std::filesystem::path statePath(const StoredRecord& stored) {
        return stored.directory / "state";
    }

    static std::optional<RecordState> readRecordState(
        const StoredRecord& stored) {
        if (!statFile(statePath(stored))) {
            return std::nullopt;
        }
        const auto encoded = readBytes(statePath(stored));
        if (encoded.size() != 1) {
            throw std::runtime_error("invalid recovery record state");
        }
        const auto value =
            std::to_integer<std::uint8_t>(encoded.front());
        if (value != static_cast<std::uint8_t>(RecordState::InProgress) &&
            value != static_cast<std::uint8_t>(RecordState::Published)) {
            throw std::runtime_error("invalid recovery record state");
        }
        return static_cast<RecordState>(value);
    }

    static void markRecordState(const StoredRecord& stored,
                                  RecordState state) {
        const std::array encoded{
            static_cast<std::byte>(static_cast<std::uint8_t>(state))};
        replaceFileAtomically(statePath(stored), encoded);
        setOwnerOnlyPermissions(statePath(stored));
    }

    void markRestored(StoredRecord& stored) {
        replaceFileAtomically(restoredMarker(stored), instanceId_);
        setOwnerOnlyPermissions(restoredMarker(stored));
        stored.restoredInMemory = true;
    }

    bool restoredInThisInstance(const StoredRecord& stored) const {
        if (stored.restoredInMemory) return true;
        if (!statFile(restoredMarker(stored))) {
            return false;
        }
        return readBytes(restoredMarker(stored)) ==
               std::vector<std::byte>(instanceId_.begin(),
                                      instanceId_.end());
    }

    void restoreRename(const StoredRecord& stored) {
        const auto& source = stored.record.affectedPaths[0];
        const auto& destination = stored.record.affectedPaths[1];
        const auto sourceNow = snapshotKind(source);
        const auto destinationNow = snapshotKind(destination);
        if (sourceNow == SnapshotKind::Missing &&
            destinationNow != SnapshotKind::Missing) {
            removeNode(source);
            createDirectories(source.parent_path());
            renameDurably(destination, source);
        } else if (sourceNow == SnapshotKind::Missing) {
            restoreSnapshot(source, artifactPath(stored, 0),
                             stored.snapshots[0]);
        }
        before(RecoveryStep::RestoreFilesystem);
        restoreSnapshot(destination, artifactPath(stored, 1),
                         stored.snapshots[1]);
        before(RecoveryStep::RestoreFilesystem);
    }

    void restoreFilesystemState(const StoredRecord& stored) {
        switch (stored.record.kind) {
        case RecoveryRecordKind::PathDelete:
            restoreSnapshot(stored.record.affectedPaths[0],
                             artifactPath(stored, 0), stored.snapshots[0]);
            return;
        case RecoveryRecordKind::PathRename:
            restoreRename(stored);
            return;
        case RecoveryRecordKind::DocumentClose:
            throw std::logic_error(
                "document recovery record used for filesystem restoration");
        }
    }

    static void syncFilesystemState(const StoredRecord& stored) {
        for (const auto& path : stored.record.affectedPaths) {
            const auto kind = snapshotKind(path);
            if (kind == SnapshotKind::RegularFile) {
                syncPath(path, false);
            } else if (kind == SnapshotKind::Directory) {
                syncTree(path);
            }
            syncPath(std::filesystem::absolute(path).parent_path(), true);
        }
    }

    RecordIterator findRecord(const RecoveryRecordId& id) {
        return std::find_if(records_.begin(), records_.end(),
                            [&](const StoredRecord& stored) {
                                return stored.record.id == id;
                            });
    }

    RecoveryRestoreResult cleanupRestored(RecordIterator record) {
        try {
            before(RecoveryStep::CleanupRecord);
            removeNode(record->directory);
            records_.erase(record);
            return {};
        } catch (...) {
            return {error(RecoveryErrorCode::CleanupFailed,
                          "recovery cleanup failed: " +
                              exceptionMessage(std::current_exception()))};
        }
    }

    std::filesystem::path recoveryRoot_;
    RecoveryConfig config_;
    RecoveryFaultInjector* faultInjector_;
    std::vector<StoredRecord> records_;
    std::uint64_t nextId_ = 0;
    std::array<std::byte, 16> instanceId_ =
        UntitledDocumentId::generate().bytes();
};

RecoveryManager RecoveryManager::create(
    const std::filesystem::path& recoveryRoot,
    RecoveryConfig config) {
    return RecoveryManager{
        std::make_unique<Impl>(recoveryRoot, config, nullptr)};
}

RecoveryManager RecoveryManager::create(
    const std::filesystem::path& recoveryRoot,
    RecoveryConfig config,
    RecoveryFaultInjector& faultInjector) {
    return RecoveryManager{
        std::make_unique<Impl>(recoveryRoot, config, &faultInjector)};
}

RecoveryManager::RecoveryManager(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

RecoveryManager::~RecoveryManager() = default;
RecoveryManager::RecoveryManager(RecoveryManager&&) noexcept = default;
RecoveryManager& RecoveryManager::operator=(RecoveryManager&&) noexcept =
    default;

std::vector<RecoveryRecord> RecoveryManager::records() const {
    return impl_->records();
}

RecoveryActionResult RecoveryManager::closeDocument(
    std::optional<JournalDocument>& document,
    ScratchStore& scratch,
    std::chrono::milliseconds durabilityTimeout) {
    return impl_->closeDocument(document, scratch, durabilityTimeout);
}

RecoveryActionResult RecoveryManager::renamePath(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    return impl_->renamePath(source, destination);
}

RecoveryActionResult RecoveryManager::renamePathNoClobber(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    return impl_->renamePathNoClobber(source, destination);
}

RecoveryActionResult RecoveryManager::deletePath(
    const std::filesystem::path& path) {
    return impl_->deletePath(path);
}

RecoveryRestoreResult RecoveryManager::restoreDocument(
    const RecoveryRecordId& record,
    std::optional<JournalDocument>& document) {
    return impl_->restoreDocument(record, document);
}

RecoveryRestoreResult RecoveryManager::restoreFilesystem(
    const RecoveryRecordId& record) {
    return impl_->restoreFilesystem(record);
}

} // namespace ssg
