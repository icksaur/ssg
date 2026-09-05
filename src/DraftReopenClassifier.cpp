#include <ssg/DraftReopenClassifier.h>

namespace ssg {

DraftReopenClass classifyDraftReopen(
    const std::optional<DraftBaseline>& baseline,
    std::string_view draftContent,
    const std::optional<DraftDiskState>& disk) {
    if (!disk) return DraftReopenClass::Missing;
    if (draftContent == disk->decodedText) return DraftReopenClass::Converged;
    if (baseline &&
        fastContentHash(disk->rawBytes) == baseline->contentHash) {
        return DraftReopenClass::Unchanged;
    }
    return DraftReopenClass::Conflict;
}

} // namespace ssg
