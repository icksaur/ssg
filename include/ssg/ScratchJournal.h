#pragma once

#include <ssg/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {

class UntitledDocumentId {
public:
    explicit constexpr UntitledDocumentId(
        std::array<std::byte, 16> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] static UntitledDocumentId generate();
    [[nodiscard]] constexpr const std::array<std::byte, 16>& bytes()
        const noexcept {
        return bytes_;
    }

    friend bool operator==(const UntitledDocumentId&,
                           const UntitledDocumentId&) = default;

private:
    std::array<std::byte, 16> bytes_;
};

enum class JournalDocumentKeyKind {
    Saved = 0,
    Untitled = 1,
};

class JournalDocumentKey {
public:
    [[nodiscard]] static JournalDocumentKey saved(
        std::string_view workspaceRelativePath);
    [[nodiscard]] static JournalDocumentKey untitled(
        UntitledDocumentId id);

    [[nodiscard]] JournalDocumentKeyKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& savedPath() const;
    [[nodiscard]] UntitledDocumentId untitledId() const;

    friend bool operator==(const JournalDocumentKey&,
                           const JournalDocumentKey&) = default;

private:
    JournalDocumentKey(JournalDocumentKeyKind kind,
                       std::string path,
                       UntitledDocumentId id)
        : kind_(kind), path_(std::move(path)), id_(id) {}

    JournalDocumentKeyKind kind_;
    std::string path_;
    UntitledDocumentId id_;
};

// The disk state a saved-file draft branched from, captured when the file is
// opened and refreshed on each save. On reopen, comparing this to the current
// disk file tells draft recovery whether the file changed externally since the
// edits were made. `contentHash` is the authority (a fast non-cryptographic hash
// of the disk bytes); `mtimeNanos` and `size` are only a cheap pre-check. Absent
// (nullopt) for untitled buffers, and for legacy records written before the
// baseline existed — an absent baseline means "unknown", treated as a conflict.
struct DraftBaseline {
    std::uint64_t mtimeNanos = 0;
    std::uint64_t size = 0;
    std::uint64_t contentHash = 0;

    friend bool operator==(const DraftBaseline&, const DraftBaseline&) = default;
};

// A fast, deterministic, non-cryptographic hash (FNV-1a, 64-bit) of raw bytes,
// for change-detection only — no integrity or security guarantee. Stable across
// processes and platforms so a stored baseline hash compares to a fresh one.
[[nodiscard]] std::uint64_t fastContentHash(std::string_view bytes) noexcept;

struct JournalDocument {
    JournalDocumentKey key;
    DocumentMode mode = DocumentMode::Edit;
    bool dirty = false;
    std::string utf8Content;
    std::optional<DraftBaseline> baseline;

    friend bool operator==(const JournalDocument&,
                           const JournalDocument&) = default;
};

struct JournalRecoverySet {
    std::vector<JournalDocument> documents;

    friend bool operator==(const JournalRecoverySet&,
                           const JournalRecoverySet&) = default;
};

struct JournalReplayResult {
    JournalRecoverySet recovery;
    std::size_t validBytes = 0;
    bool discardedTail = false;
};

[[nodiscard]] std::vector<std::byte> encodeJournalCheckpoint(
    const JournalRecoverySet& recovery);
[[nodiscard]] std::vector<std::byte> encodeJournalDocument(
    const JournalDocument& document);
[[nodiscard]] std::vector<std::byte> encodeJournalRemove(
    const JournalDocumentKey& key);
[[nodiscard]] JournalReplayResult replayJournal(std::span<const std::byte> bytes);

class ScratchJournal {
public:
    explicit ScratchJournal(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void appendCheckpoint(const JournalRecoverySet& recovery) const;
    void appendDocument(const JournalDocument& document) const;
    void appendRemove(const JournalDocumentKey& key) const;
    [[nodiscard]] JournalReplayResult replay() const;

private:
    void append(std::span<const std::byte> record) const;

    std::filesystem::path path_;
};

} // namespace ssg
