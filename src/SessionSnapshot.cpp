#include <ssg/SessionSnapshot.h>

#include <ssg/Document.h>
#include <ssg/DocumentKey.h>
#include <ssg/platform_files.h>

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'S', 'S', 'G', 'S', 'N', 'A', 'P', '\0'};
constexpr std::uint32_t kVersion = 1;
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

std::filesystem::path sessionSnapshotPath(
    const std::filesystem::path& processStartingDirectory) {
    if (processStartingDirectory.empty()) return {};
    return processStartingDirectory / kSessionDirectoryName /
           kSessionSnapshotFilename;
}

SessionSnapshotEncodeResult encodeSessionSnapshot(
    const SessionSnapshot& snapshot) {
    if (const auto error = validate(snapshot)) return {{}, *error};
    try {
        std::vector<std::uint8_t> output;
        output.insert(output.end(), kMagic.begin(), kMagic.end());
        appendU32(output, kVersion);
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
        if (*version != kVersion) return {std::nullopt, "uses an unsupported version"};
        const auto tabCount = reader.u64();
        if (!tabCount) return {std::nullopt, "is truncated"};
        if (*tabCount > reader.remaining() / kTabFixedBytes) {
            return {std::nullopt, "contains an invalid tab count"};
        }

        SessionSnapshot snapshot;
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
    const std::filesystem::path& path) {
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

} // namespace ssg
