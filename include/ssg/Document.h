#pragma once

#include <ssg/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class ValidatedUtf8;

struct TextEdit {
    ByteOffset offset;
    std::uint64_t erasedBytes;
    std::string insertedText;

    bool operator==(TextEdit const&) const = default;
};

struct EditTransaction {
    std::uint64_t baseRevision;
    std::vector<TextEdit> edits;

    bool operator==(EditTransaction const&) const = default;
};

enum class DocumentError : std::uint8_t {
    None,
    ReadOnly,
    Diff,
    StaleRevision,
    EmptyTransaction,
    InvalidRange,
    OverlappingEdits,
    InvalidUtf8,
    InvalidUtf8Boundary,
    RevisionExhausted,
};

struct TransactionResult {
    DocumentError error;
    std::uint64_t revision;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == DocumentError::None;
    }

    bool operator==(TransactionResult const&) const = default;
};

struct DocumentSnapshot {
    std::string text;
    std::uint64_t revision;
    DocumentMode mode;
    bool dirty;

    bool operator==(DocumentSnapshot const&) const = default;
};

class Document {
public:
    explicit Document(std::string_view initialText = {},
                      DocumentMode mode = DocumentMode::Edit);
    // Construct from decoder-validated UTF-8 WITHOUT re-validating (open path).
    // The bytes are moved into the piece tree, not copied.
    explicit Document(ValidatedUtf8 validated,
                      DocumentMode mode = DocumentMode::Edit);
    ~Document();

    Document(Document const&) = delete;
    Document& operator=(Document const&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] DocumentMode mode() const noexcept;
    [[nodiscard]] DocumentSnapshot snapshot() const;

    [[nodiscard]] TransactionResult apply(EditTransaction const& transaction);
    [[nodiscard]] TransactionResult replace(std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
