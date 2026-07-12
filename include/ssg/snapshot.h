#pragma once

#include <ssg/types.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

struct DocumentViewState {
    Revision revision;
    std::string text;
    ByteOffset caret;

    bool operator==(DocumentViewState const&) const = default;
};

struct DocumentDelta {
    Revision base_revision;
    Revision revision;
    ByteOffset start;
    std::uint64_t erased_bytes;
    std::string inserted_text;

    bool operator==(DocumentDelta const&) const = default;
};

[[nodiscard]] std::optional<DocumentDelta> derive_document_delta(
    DocumentViewState const& before, DocumentViewState const& after);
[[nodiscard]] std::optional<DocumentViewState> replay_document_delta(
    DocumentViewState const& before, DocumentDelta const& delta,
    ByteOffset target_caret);

}  // namespace ssg
