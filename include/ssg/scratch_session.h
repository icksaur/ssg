#pragma once

#include "ssg/platform_files.h"
#include "ssg/scratch_journal.h"

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

[[nodiscard]] std::string scratch_workspace_key(
    const std::filesystem::path& canonical_workspace);

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
    [[nodiscard]] std::filesystem::path journal_path() const {
        return path_ / "journal.bin";
    }
    [[nodiscard]] JournalReplayResult replay() const;
    void mark_restored();

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
        const std::filesystem::path& scratch_root,
        const std::filesystem::path& canonical_workspace);

    ScratchSession(ScratchSession&&) noexcept = default;
    ScratchSession& operator=(ScratchSession&&) noexcept = default;
    ScratchSession(const ScratchSession&) = delete;
    ScratchSession& operator=(const ScratchSession&) = delete;

    [[nodiscard]] const ScratchSessionId& id() const noexcept { return id_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::filesystem::path journal_path() const {
        return path_ / "journal.bin";
    }
    [[nodiscard]] std::optional<ScratchRemnantClaim>
    claim_newest_restorable() const;

private:
    ScratchSession(ScratchSessionId id,
                   std::filesystem::path path,
                   std::filesystem::path sessions_path,
                   ExclusiveFileLock lock)
        : id_(std::move(id)),
          path_(std::move(path)),
          sessions_path_(std::move(sessions_path)),
          lock_(std::move(lock)) {}

    ScratchSessionId id_;
    std::filesystem::path path_;
    std::filesystem::path sessions_path_;
    ExclusiveFileLock lock_;
};

} // namespace ssg
