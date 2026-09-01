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
    std::optional<std::string> diffFileIdentity;

    bool operator==(DocumentViewState const&) const = default;
};

struct DocumentDelta {
    Revision baseRevision;
    Revision revision;
    ByteOffset start;
    std::uint64_t erasedBytes;
    std::string insertedText;
    std::optional<std::string> diffFileIdentity;

    bool operator==(DocumentDelta const&) const = default;
};

class DocumentSnapshotCodec {
public:
    [[nodiscard]] std::optional<DocumentDelta> deriveDelta(
        DocumentViewState const& before, DocumentViewState const& after) const;
    [[nodiscard]] std::optional<DocumentViewState> replay(
        DocumentViewState const& before, DocumentDelta const& delta,
        ByteOffset targetCaret) const;
};

}  // namespace ssg
