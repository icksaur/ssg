#include <ssg/SessionSnapshot.h>

#include <ssg/Document.h>
#include <ssg/DocumentKey.h>
#include <ssg/platform_files.h>

#include <array>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'S', 'S', 'G', 'S', 'N', 'A', 'P', '\0'};
constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kTabFixedBytes = 4 + 4 * sizeof(std::uint64_t);

std::string invalidSnapshot(const std::filesystem::path& path,
                            std::string reason) {
    return "session snapshot '" + path.string() + "' " + std::move(reason) +
           "; move or delete it to start without recovery";
}

bool validUtf8(std::string_view value) {
    try {
        (void)Document{value};
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

std::optional<std::string> validate(const SessionSnapshot& snapshot) {
    if (!snapshot.identity.empty() && !validUtf8(snapshot.identity)) {
        return "contains an invalid session identity";
    }
    bool hasActive = false;
    std::unordered_set<std::string> namedPaths;
    for (const auto& tab : snapshot.tabs) {
        if (tab.mode != DocumentMode::Edit) {
            return "contains a non-editable document";
        }
        if (tab.label.empty() || !validUtf8(tab.label)) {
            return "contains an invalid label";
        }
        if (!validUtf8(tab.draft)) {
            return "contains invalid UTF-8 draft text";
        }
        if (tab.active && std::exchange(hasActive, true)) {
            return "contains multiple active tabs";
        }
        switch (tab.backing) {
        case SessionBackingKind::Untitled:
            if (!tab.path.empty() || !tab.baseline.empty() ||
                tab.draft.empty()) {
                return "contains an invalid untitled tab";
            }
            break;
        case SessionBackingKind::NeverCreatedPath:
            if (tab.path.empty() || !tab.baseline.empty() ||
                !validUtf8(tab.path)) {
                return "contains an invalid never-created path tab";
            }
            try {
                (void)DocumentKey::saved(tab.path);
            } catch (const std::invalid_argument&) {
                return "contains an invalid never-created path tab";
            }
            if (!namedPaths.insert(tab.path).second) {
                return "contains duplicate document paths";
            }
            break;
        case SessionBackingKind::PersistedPath:
            if (tab.path.empty() || !validUtf8(tab.path)) {
                return "contains an invalid persisted path tab";
            }
            try {
                (void)DocumentKey::saved(tab.path);
            } catch (const std::invalid_argument&) {
                return "contains an invalid persisted path tab";
            }
            if (!namedPaths.insert(tab.path).second) {
                return "contains duplicate document paths";
            }
            break;
        default:
            return "contains an unknown backing kind";
        }
    }
    return std::nullopt;
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendBytes(std::vector<std::uint8_t>& output,
                 std::span<const std::uint8_t> bytes) {
    appendU64(output, static_cast<std::uint64_t>(bytes.size()));
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void appendString(std::vector<std::uint8_t>& output, std::string_view value) {
    appendBytes(output, {reinterpret_cast<const std::uint8_t*>(value.data()),
                         value.size()});
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] std::optional<std::uint8_t> u8() {
        if (remaining() < 1) return std::nullopt;
        return bytes_[offset_++];
    }

    [[nodiscard]] std::optional<std::uint32_t> u32() {
        if (remaining() < sizeof(std::uint32_t)) return std::nullopt;
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        }
        return value;
    }

    [[nodiscard]] std::optional<std::uint64_t> u64() {
        if (remaining() < sizeof(std::uint64_t)) return std::nullopt;
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        }
        return value;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> bytes() {
        const auto length = u64();
        if (!length ||
            *length > static_cast<std::uint64_t>(
                          (std::numeric_limits<std::size_t>::max)()) ||
            *length > remaining()) {
            return std::nullopt;
        }
        const auto size = static_cast<std::size_t>(*length);
        std::vector<std::uint8_t> value{
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size)};
        offset_ += size;
        return value;
    }

    [[nodiscard]] std::optional<std::string> string() {
        auto value = bytes();
        if (!value) return std::nullopt;
        return std::string{value->begin(), value->end()};
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

} // namespace

std::string sessionSnapshotIdentity(
    const std::filesystem::path& processStartingDirectory) {
    if (processStartingDirectory.empty()) return {};
    return weaklyCanonicalPath(processStartingDirectory).generic_string();
}

std::string sessionSnapshotKey(std::string_view identity) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : identity) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << hash;
    return key.str();
}

std::filesystem::path sessionSnapshotPath(
    const std::filesystem::path& stateRoot,
    const std::filesystem::path& processStartingDirectory) {
    if (stateRoot.empty() || processStartingDirectory.empty()) return {};
    const auto identity = sessionSnapshotIdentity(processStartingDirectory);
    return stateRoot / kSessionDirectoryName / sessionSnapshotKey(identity) /
           kSessionSnapshotFilename;
}

std::filesystem::path legacySessionSnapshotPath(
    const std::filesystem::path& processStartingDirectory) {
    if (processStartingDirectory.empty()) return {};
    return processStartingDirectory / kLegacySessionDirectoryName /
           kSessionSnapshotFilename;
}

SessionSnapshotEncodeResult encodeSessionSnapshot(
    const SessionSnapshot& snapshot) {
    if (const auto error = validate(snapshot)) return {{}, *error};
    try {
        std::vector<std::uint8_t> output;
        output.insert(output.end(), kMagic.begin(), kMagic.end());
        appendU32(output, kVersion);
        appendString(output, snapshot.identity);
        appendU64(output, static_cast<std::uint64_t>(snapshot.tabs.size()));
        for (const auto& tab : snapshot.tabs) {
            output.push_back(static_cast<std::uint8_t>(tab.backing));
            output.push_back(static_cast<std::uint8_t>(tab.mode));
            output.push_back(tab.active ? 1U : 0U);
            output.push_back(0);
            appendString(output, tab.path);
            appendString(output, tab.label);
            appendString(output, tab.draft);
            appendBytes(output, tab.baseline);
        }
        return {std::move(output), {}};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

SessionSnapshotDecodeResult decodeSessionSnapshot(
    std::span<const std::uint8_t> bytes) {
    try {
        Reader reader{bytes};
        for (const auto expected : kMagic) {
            const auto actual = reader.u8();
            if (!actual || *actual != expected) return {std::nullopt, "has invalid magic"};
        }
        const auto version = reader.u32();
        if (!version) return {std::nullopt, "is truncated"};
        if (*version != kLegacyVersion && *version != kVersion) {
            return {std::nullopt, "uses an unsupported version"};
        }
        std::string identity;
        if (*version == kVersion) {
            auto decodedIdentity = reader.string();
            if (!decodedIdentity) return {std::nullopt, "is truncated"};
            identity = std::move(*decodedIdentity);
        }
        const auto tabCount = reader.u64();
        if (!tabCount) return {std::nullopt, "is truncated"};
        if (*tabCount > reader.remaining() / kTabFixedBytes) {
            return {std::nullopt, "contains an invalid tab count"};
        }

        SessionSnapshot snapshot;
        snapshot.identity = std::move(identity);
        snapshot.tabs.reserve(static_cast<std::size_t>(*tabCount));
        for (std::uint64_t index = 0; index < *tabCount; ++index) {
            const auto backing = reader.u8();
            const auto mode = reader.u8();
            const auto active = reader.u8();
            const auto reserved = reader.u8();
            auto path = reader.string();
            auto label = reader.string();
            auto draft = reader.string();
            auto baseline = reader.bytes();
            if (!backing || !mode || !active || !reserved || !path || !label ||
                !draft || !baseline) {
                return {std::nullopt, "is truncated or contains an invalid length"};
            }
            if (*active > 1 || *reserved != 0) {
                return {std::nullopt, "contains invalid flags"};
            }
            snapshot.tabs.push_back(
                {static_cast<SessionBackingKind>(*backing), std::move(*path),
                 std::move(*label), static_cast<DocumentMode>(*mode),
                 std::move(*draft), std::move(*baseline), *active == 1});
        }
        if (reader.remaining() != 0) {
            return {std::nullopt, "contains trailing bytes"};
        }
        if (const auto error = validate(snapshot)) {
            return {std::nullopt, *error};
        }
        return {std::move(snapshot), {}};
    } catch (const std::exception& error) {
        return {std::nullopt, error.what()};
    }
}

SessionSnapshotReadResult readSessionSnapshot(
    const std::filesystem::path& path,
    std::string_view expectedIdentity) {
    if (path.empty()) return {};
    try {
        auto contents = readFile(path);
        if (contents.status == FileIoStatus::NotFound) return {};
        if (!contents.ok()) {
            return {std::nullopt,
                    invalidSnapshot(path, "could not be read: " + contents.message)};
        }
        auto decoded = decodeSessionSnapshot(contents.bytes);
        if (!decoded.accepted()) {
            return {std::nullopt, invalidSnapshot(path, decoded.message)};
        }
        if (!expectedIdentity.empty() &&
            decoded.snapshot->identity != expectedIdentity) {
            return {std::nullopt,
                    invalidSnapshot(path, "belongs to another launch directory")};
        }
        return {std::move(decoded.snapshot), {}};
    } catch (const std::exception& error) {
        return {std::nullopt,
                invalidSnapshot(path, "could not be read: " +
                                          std::string{error.what()})};
    }
}

SessionSnapshotWriteResult writeSessionSnapshot(
    const std::filesystem::path& path, const SessionSnapshot& snapshot) {
    if (path.empty()) return {};
    const auto encoded = encodeSessionSnapshot(snapshot);
    if (!encoded.accepted()) {
        return {"could not write session snapshot '" + path.string() +
                "': " + encoded.message};
    }
    try {
        const auto created = ensureDirectory(path.parent_path());
        if (!created.ok()) {
            return {"could not write session snapshot '" + path.string() +
                    "': " + created.message};
        }
        replaceFileAtomically(
            path, {reinterpret_cast<const std::byte*>(encoded.bytes.data()),
                   encoded.bytes.size()});
        return {};
    } catch (const std::exception& error) {
        return {"could not write session snapshot '" + path.string() +
                "': " + error.what()};
    }
}

SessionSnapshotMigrationResult migrateLegacySessionSnapshot(
    const std::filesystem::path& centralPath,
    std::string_view identity,
    const std::filesystem::path& legacyPath) {
    try {
        const auto central = readSessionSnapshot(centralPath, identity);
        if (!central.accepted()) return {central.message};

        const auto legacyDirectory = legacyPath.parent_path();
        const auto directoryStatus = statFile(legacyDirectory);
        if (!directoryStatus) return {};
        if (directoryStatus->kind == FileKind::Symlink) {
            return {"legacy session directory '" + legacyDirectory.string() +
                    "' is a symbolic link; move or delete it before starting"};
        }

        auto legacy = readSessionSnapshot(legacyPath);
        if (!legacy.accepted()) {
            return central.snapshot ? SessionSnapshotMigrationResult{}
                                    : SessionSnapshotMigrationResult{
                                          legacy.message};
        }
        if (!legacy.snapshot) return {};

        auto retireLegacy = [&]() -> SessionSnapshotMigrationResult {
            const auto currentDirectoryStatus = statFile(legacyDirectory);
            if (!currentDirectoryStatus ||
                currentDirectoryStatus->kind == FileKind::Symlink) {
                return {"legacy session directory '" +
                        legacyDirectory.string() +
                        "' changed during migration"};
            }
            const auto status = statFile(legacyPath);
            if (!status) return {};
            if (status->kind != FileKind::Regular) {
                return {"legacy session snapshot '" + legacyPath.string() +
                        "' is not a regular file"};
            }
            const auto removed = removeTree(legacyPath);
            if (!removed.ok() && removed.status != FileIoStatus::NotFound) {
                return {"could not remove legacy session snapshot '" +
                        legacyPath.string() + "': " + removed.message};
            }
            return {};
        };

        if (central.snapshot) return retireLegacy();

        legacy.snapshot->identity = std::string{identity};
        const auto written = writeSessionSnapshot(centralPath, *legacy.snapshot);
        if (!written.accepted()) return {written.message};
        return retireLegacy();
    } catch (const std::exception& error) {
        return {"could not migrate legacy session snapshot '" +
                legacyPath.string() + "': " + error.what()};
    }
}

} // namespace ssg
