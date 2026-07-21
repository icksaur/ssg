#include <ssg/snapshot.h>

#include <algorithm>
#include <cstddef>

namespace ssg {

std::optional<DocumentDelta> DocumentSnapshotCodec::deriveDelta(
    DocumentViewState const& before, DocumentViewState const& after) const {
    if (before.revision == after.revision &&
        before.diffFileIdentity == after.diffFileIdentity) {
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
        after.text.substr(prefix, after.text.size() - prefix - suffix),
        after.diffFileIdentity};
}

std::optional<DocumentViewState> DocumentSnapshotCodec::replay(
    DocumentViewState const& before, DocumentDelta const& delta,
    ByteOffset targetCaret) const {
    if (before.revision != delta.baseRevision ||
        delta.start.value() > before.text.size() ||
        delta.erasedBytes > before.text.size() - delta.start.value()) {
        return std::nullopt;
    }
    bool const textChanged = delta.erasedBytes != 0 || !delta.insertedText.empty();
    if (textChanged && delta.revision == delta.baseRevision) {
        return std::nullopt;
    }
    std::string text = before.text;
    text.replace(delta.start.value(), delta.erasedBytes, delta.insertedText);
    if (targetCaret.value() > text.size()) {
        return std::nullopt;
    }
    return DocumentViewState{delta.revision, std::move(text), targetCaret,
                             delta.diffFileIdentity};
}

}  // namespace ssg
