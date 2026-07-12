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
    saved,
    untitled,
};

class JournalDocumentKey {
public:
    [[nodiscard]] static JournalDocumentKey saved(
        std::string_view workspace_relative_path);
    [[nodiscard]] static JournalDocumentKey untitled(
        UntitledDocumentId id);

    [[nodiscard]] JournalDocumentKeyKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& saved_path() const;
    [[nodiscard]] UntitledDocumentId untitled_id() const;

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
    DocumentMode mode = DocumentMode::edit;
    bool dirty = false;
    std::string utf8_content;

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
    std::size_t valid_bytes = 0;
    bool discarded_tail = false;
};

[[nodiscard]] std::vector<std::byte> encode_checkpoint_record(
    const JournalRecoverySet& recovery);
[[nodiscard]] std::vector<std::byte> encode_document_record(
    const JournalDocument& document);
[[nodiscard]] std::vector<std::byte> encode_remove_record(
    const JournalDocumentKey& key);
[[nodiscard]] JournalReplayResult replay_journal(
    std::span<const std::byte> bytes);

class ScratchJournal {
public:
    explicit ScratchJournal(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void append_checkpoint(const JournalRecoverySet& recovery) const;
    void append_document(const JournalDocument& document) const;
    void append_remove(const JournalDocumentKey& key) const;
    [[nodiscard]] JournalReplayResult replay() const;

private:
    void append(std::span<const std::byte> record) const;

    std::filesystem::path path_;
};

} // namespace ssg
