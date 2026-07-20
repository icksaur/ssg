#include <ssg/document.h>

#include "piece_tree.h"

#include <ssg/open_metrics.h>
#include <ssg/text_codec.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
namespace {

bool isContinuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

bool validUtf8WithoutNul(std::string_view text) {
    noteUtf8Validation();
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first == 0) {
            return false;
        }
        if (first <= 0x7FU) {
            ++offset;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        if (offset + length > text.size()) {
            return false;
        }

        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (!isContinuation(second)) {
            return false;
        }
        if ((first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second > 0x9FU) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second > 0x8FU)) {
            return false;
        }
        for (std::size_t index = 2; index < length; ++index) {
            if (!isContinuation(
                    static_cast<unsigned char>(text[offset + index]))) {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

bool isUtf8Boundary(std::string_view text, std::size_t offset) {
    return offset == text.size() ||
           !isContinuation(static_cast<unsigned char>(text[offset]));
}

TransactionResult failure(DocumentError error, Revision revision,
                          std::string message) {
    return TransactionResult{error, revision, std::move(message)};
}

}  // namespace

struct Document::Impl {
    explicit Impl(std::string_view text, DocumentMode documentMode)
        : tree(text), mode(documentMode) {}
    explicit Impl(SharedBytes text, DocumentMode documentMode)
        : tree(std::move(text)), mode(documentMode) {}

    detail::PieceTree tree;
    Revision revision{1};
    DocumentMode mode;
    bool dirty{false};
};

Document::Document(std::string_view initialText, DocumentMode mode) {
    if (!validUtf8WithoutNul(initialText)) {
        throw std::invalid_argument(
            "document text must be well-formed UTF-8 without NUL bytes");
    }
    impl_ = std::make_unique<Impl>(initialText, mode);
}

Document::Document(ValidatedUtf8 validated, DocumentMode mode) {
    impl_ = std::make_unique<Impl>(std::move(validated).take(), mode);
}

Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

Revision Document::revision() const noexcept {
    return impl_->revision;
}

DocumentMode Document::mode() const noexcept {
    return impl_->mode;
}

bool Document::dirty() const noexcept {
    return impl_->dirty;
}

DocumentSnapshot Document::snapshot() const {
    return DocumentSnapshot{
        impl_->tree.text(), impl_->revision, impl_->mode, impl_->dirty};
}

TransactionResult Document::apply(EditTransaction const& transaction) {
    const auto currentRevision = impl_->revision;
    if (transaction.baseRevision != currentRevision) {
        return failure(DocumentError::StaleRevision, currentRevision,
                       "transaction base revision is stale");
    }
    if (impl_->mode == DocumentMode::ReadOnly) {
        return failure(DocumentError::ReadOnly, currentRevision,
                       "read-only documents cannot be edited");
    }
    if (impl_->mode == DocumentMode::Diff) {
        return failure(DocumentError::Diff, currentRevision,
                       "diff documents cannot be edited");
    }
    if (transaction.edits.empty()) {
        return failure(DocumentError::EmptyTransaction, currentRevision,
                       "transaction must contain at least one edit");
    }
    if (currentRevision.value() ==
        std::numeric_limits<std::uint64_t>::max()) {
        return failure(DocumentError::RevisionExhausted, currentRevision,
                       "document revision is exhausted");
    }

    const auto original = impl_->tree.text();
    std::vector<TextEdit const*> ordered;
    ordered.reserve(transaction.edits.size());
    for (auto const& edit : transaction.edits) {
        if (edit.erasedBytes == 0 && edit.insertedText.empty()) {
            return failure(DocumentError::EmptyTransaction, currentRevision,
                           "transaction contains an empty edit");
        }
        if (!validUtf8WithoutNul(edit.insertedText)) {
            return failure(DocumentError::InvalidUtf8, currentRevision,
                           "inserted text must be well-formed UTF-8 without NUL bytes");
        }
        if (edit.offset.value() > original.size() ||
            edit.erasedBytes >
                original.size() -
                    static_cast<std::size_t>(edit.offset.value())) {
            return failure(DocumentError::InvalidRange, currentRevision,
                           "edit range is outside the document");
        }

        const auto begin = static_cast<std::size_t>(edit.offset.value());
        const auto end =
            begin + static_cast<std::size_t>(edit.erasedBytes);
        if (!isUtf8Boundary(original, begin) ||
            !isUtf8Boundary(original, end)) {
            return failure(DocumentError::InvalidUtf8Boundary,
                           currentRevision,
                           "edit range splits a UTF-8 code point");
        }
        ordered.push_back(&edit);
    }

    std::sort(ordered.begin(), ordered.end(),
              [](TextEdit const* left, TextEdit const* right) {
                  return left->offset.value() < right->offset.value();
              });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        const auto& previous = *ordered[index - 1];
        const auto& current = *ordered[index];
        const auto previousEnd =
            previous.offset.value() + previous.erasedBytes;
        if (current.offset == previous.offset ||
            current.offset.value() < previousEnd) {
            return failure(DocumentError::OverlappingEdits,
                           currentRevision,
                           "edit ranges must be distinct and non-overlapping");
        }
    }

    for (auto iterator = ordered.rbegin(); iterator != ordered.rend();
         ++iterator) {
        const auto& edit = **iterator;
        const auto offset = static_cast<std::size_t>(edit.offset.value());
        const auto erased = static_cast<std::size_t>(edit.erasedBytes);
        if (erased != 0) {
            impl_->tree.erase(offset, erased);
        }
        if (!edit.insertedText.empty()) {
            impl_->tree.insert(offset, edit.insertedText);
        }
    }

    impl_->revision = Revision{currentRevision.value() + 1};
    impl_->dirty = true;
    return TransactionResult{DocumentError::None, impl_->revision, {}};
}

}  // namespace ssg
