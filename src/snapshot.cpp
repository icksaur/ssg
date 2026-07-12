#include <ssg/snapshot.h>

#include <algorithm>
#include <cstddef>

namespace ssg {

std::optional<DocumentDelta> derive_document_delta(
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

}  // namespace ssg
