#pragma once

#include <ssg/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

inline constexpr std::string_view kSessionDirectoryName = ".ssg";
inline constexpr std::string_view kSessionSnapshotFilename = "session.snapshot";

enum class SessionBackingKind : std::uint8_t {
    Untitled = 0,
    NeverCreatedPath = 1,
    PersistedPath = 2,
};

struct SessionSnapshotTab {
    SessionBackingKind backing = SessionBackingKind::Untitled;
    std::string path;
    std::string label;
    DocumentMode mode = DocumentMode::Edit;
    std::string draft;
    std::vector<std::uint8_t> baseline;
    bool active = false;

    friend bool operator==(const SessionSnapshotTab&,
                           const SessionSnapshotTab&) = default;
};

struct SessionSnapshot {
    std::vector<SessionSnapshotTab> tabs;

    friend bool operator==(const SessionSnapshot&,
                           const SessionSnapshot&) = default;
};

struct SessionSnapshotEncodeResult {
    std::vector<std::uint8_t> bytes;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return message.empty(); }
};

struct SessionSnapshotDecodeResult {
    std::optional<SessionSnapshot> snapshot;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return snapshot.has_value();
    }
};

struct SessionSnapshotReadResult {
    std::optional<SessionSnapshot> snapshot;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return message.empty(); }
};

struct SessionSnapshotWriteResult {
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return message.empty(); }
};

[[nodiscard]] std::filesystem::path sessionSnapshotPath(
    const std::filesystem::path& processStartingDirectory);
[[nodiscard]] SessionSnapshotEncodeResult encodeSessionSnapshot(
    const SessionSnapshot& snapshot);
[[nodiscard]] SessionSnapshotDecodeResult decodeSessionSnapshot(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] SessionSnapshotReadResult readSessionSnapshot(
    const std::filesystem::path& path);
[[nodiscard]] SessionSnapshotWriteResult writeSessionSnapshot(
    const std::filesystem::path& path, const SessionSnapshot& snapshot);

} // namespace ssg
