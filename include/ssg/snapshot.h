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
    Revision baseRevision;
    Revision revision;
    ByteOffset start;
    std::uint64_t erasedBytes;
    std::string insertedText;

    bool operator==(DocumentDelta const&) const = default;
};

[[nodiscard]] std::optional<DocumentDelta> deriveDocumentDelta(
    DocumentViewState const& before, DocumentViewState const& after);
[[nodiscard]] std::optional<DocumentViewState> replayDocumentDelta(
    DocumentViewState const& before, DocumentDelta const& delta,
    ByteOffset targetCaret);

}  // namespace ssg
