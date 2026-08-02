#include "ssg/DraftReopen.h"

namespace ssg {

DraftReopenClass DraftReopenClassifier::classify(
    const std::optional<DraftBaseline>& baseline,
    std::string_view draftContent,
    const std::optional<DraftDiskState>& disk) const {
    if (!disk) return DraftReopenClass::Missing;
    if (draftContent == disk->decodedText) return DraftReopenClass::Converged;
    if (baseline &&
        fastContentHash(disk->rawBytes) == baseline->contentHash) {
        return DraftReopenClass::Unchanged;
    }
    return DraftReopenClass::Conflict;
}

} // namespace ssg
