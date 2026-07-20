#include <ssg/snapshot.h>

#include <algorithm>
#include <cstddef>

namespace ssg {

std::optional<DocumentDelta> deriveDocumentDelta(
    DocumentViewState const& before, DocumentViewState const& after) {
    if (before.revision == after.revision) {
        return std::nullopt;
    }

    std::size_t prefix = 0;
    std::size_t const common = std::min(before.text.size(), after.text.size());
    while (prefix < common && before.text[prefix] == after.text[prefix]) {
        ++prefix;
    }

    std::size_t suffix = 0;
    while (suffix < common - prefix &&
           before.text[before.text.size() - 1 - suffix] ==
               after.text[after.text.size() - 1 - suffix]) {
        ++suffix;
    }

    return DocumentDelta{
        before.revision,
        after.revision,
        ByteOffset{prefix},
        static_cast<std::uint64_t>(before.text.size() - prefix - suffix),
        after.text.substr(prefix, after.text.size() - prefix - suffix)};
}

std::optional<DocumentViewState> replayDocumentDelta(
    DocumentViewState const& before, DocumentDelta const& delta,
    ByteOffset targetCaret) {
    if (before.revision != delta.baseRevision ||
        delta.revision == delta.baseRevision ||
        delta.start.value() > before.text.size() ||
        delta.erasedBytes > before.text.size() - delta.start.value()) {
        return std::nullopt;
    }
    std::string text = before.text;
    text.replace(delta.start.value(), delta.erasedBytes, delta.insertedText);
    if (targetCaret.value() > text.size()) {
        return std::nullopt;
    }
    return DocumentViewState{delta.revision, std::move(text), targetCaret};
}

}  // namespace ssg
