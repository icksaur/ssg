#pragma once

#include "ssg/types.h"

#include <array>
#include <cstddef>
#include <filesystem>
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
    Saved,
    Untitled,
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

struct JournalDocument {
    JournalDocumentKey key;
    DocumentMode mode = DocumentMode::Edit;
    bool dirty = false;
    std::string utf8Content;

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

class JournalCodec {
public:
    [[nodiscard]] std::vector<std::byte> encodeCheckpoint(
        const JournalRecoverySet& recovery) const;
    [[nodiscard]] std::vector<std::byte> encodeDocument(
        const JournalDocument& document) const;
    [[nodiscard]] std::vector<std::byte> encodeRemove(
        const JournalDocumentKey& key) const;
    [[nodiscard]] JournalReplayResult replay(std::span<const std::byte> bytes)
        const;
};

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
