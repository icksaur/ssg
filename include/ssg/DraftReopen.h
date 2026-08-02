#pragma once

#include "ssg/ScratchJournal.h"

#include <optional>
#include <string>
#include <string_view>

namespace ssg {

// The outcome of reconciling a dirty saved-file draft against the file's current
// disk content when the file is reopened (single-file draft recovery, M15).
enum class DraftReopenClass {
    // The draft's content already equals the current disk content (the edits
    // converged, or were undone): drop the draft, open clean, no dirty state.
    Converged,
    // The disk is unchanged since the draft branched from it: offer the draft as
    // a dirty buffer, no conflict.
    Unchanged,
    // The disk changed externally since the draft branched: offer the draft as a
    // dirty buffer AND surface a conflict, so nothing is silently lost.
    Conflict,
    // The backing file is gone or unreadable: the draft is an orphaned dirty
    // buffer.
    Missing,
};

// The file's current on-disk state at reopen, in the two representations the
// classifier compares against. `rawBytes` are the exact bytes on disk — hashed
// and compared to the baseline (which was captured from raw bytes), so an
// external encoding/line-ending change is detected. `decodedText` is the decoded
// UTF-8 text — compared to the draft (which is stored decoded), so a draft whose
// edits converged with the current text is recognized regardless of encoding.
struct DraftDiskState {
    std::string rawBytes;
    std::string decodedText;
};

// Decides how a reopened file's dirty draft reconciles with disk. Stateless: the
// decision is a pure function of the draft's baseline + content and the current
// disk state. Content is the authority — mtime/size are not consulted here (v1
// always hashes the disk bytes, which the caller has already read to open the
// file); the baseline's `contentHash` is what a fresh disk-bytes hash is compared
// to.
class DraftReopenClassifier {
public:
    // `disk == nullopt` means the file is missing/unreadable. Decided in order:
    // Missing, then Converged (draft == decoded disk — checked before any
    // dirty-load so an undone draft is never briefly loaded dirty), then
    // Unchanged (raw-disk hash == baseline hash), else Conflict. An ABSENT
    // baseline is "unknown" and classifies as Conflict, never silently Unchanged.
    [[nodiscard]] DraftReopenClass classify(
        const std::optional<DraftBaseline>& baseline,
        std::string_view draftContent,
        const std::optional<DraftDiskState>& disk) const;
};

} // namespace ssg
