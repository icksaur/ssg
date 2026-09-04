#pragma once

#include <ssg/platform_files.h>
#include <ssg/ScratchJournal.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

class ScratchSessionId {
public:
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    friend bool operator==(const ScratchSessionId&,
                           const ScratchSessionId&) = default;

private:
    explicit ScratchSessionId(std::string value) : value_(std::move(value)) {}
    friend class ScratchSession;
    friend class ScratchRemnantClaim;

    std::string value_;
};

[[nodiscard]] std::string scratchWorkspaceKey(
    const std::filesystem::path& canonicalWorkspace);

class ScratchRemnantClaim {
public:
    ScratchRemnantClaim(ScratchRemnantClaim&&) noexcept = default;
    ScratchRemnantClaim& operator=(ScratchRemnantClaim&&) noexcept = default;
    ScratchRemnantClaim(const ScratchRemnantClaim&) = delete;
    ScratchRemnantClaim& operator=(const ScratchRemnantClaim&) = delete;

    [[nodiscard]] const ScratchSessionId& id() const noexcept { return id_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::filesystem::path journalPath() const {
        return path_ / "journal.bin";
    }
    [[nodiscard]] JournalReplayResult replay() const;
    void markRestored();

private:
    ScratchRemnantClaim(ScratchSessionId id,
                        std::filesystem::path path,
                        ExclusiveFileLock lock)
        : id_(std::move(id)), path_(std::move(path)), lock_(std::move(lock)) {}
    friend class ScratchSession;

    ScratchSessionId id_;
    std::filesystem::path path_;
    ExclusiveFileLock lock_;
};

class ScratchSession {
public:
    [[nodiscard]] static ScratchSession create(
        const std::filesystem::path& scratchRoot,
        const std::filesystem::path& canonicalWorkspace);

    ScratchSession(ScratchSession&&) noexcept = default;
    ScratchSession& operator=(ScratchSession&&) noexcept = default;
    ScratchSession(const ScratchSession&) = delete;
    ScratchSession& operator=(const ScratchSession&) = delete;

    [[nodiscard]] const ScratchSessionId& id() const noexcept { return id_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::filesystem::path journalPath() const {
        return path_ / "journal.bin";
    }
    [[nodiscard]] std::optional<ScratchRemnantClaim>
    claimNewestRestorable() const;

private:
    ScratchSession(ScratchSessionId id,
                   std::filesystem::path path,
                   std::filesystem::path sessionsPath,
                   ExclusiveFileLock lock)
        : id_(std::move(id)),
          path_(std::move(path)),
          sessionsPath_(std::move(sessionsPath)),
          lock_(std::move(lock)) {}

    ScratchSessionId id_;
    std::filesystem::path path_;
    std::filesystem::path sessionsPath_;
    ExclusiveFileLock lock_;
};

} // namespace ssg
