#pragma once

#include <ssg/Selection.h>
#include <ssg/Workspace.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ssg {

enum class DocumentPointerTargetState {
    Current,
    DocumentChanged,
    RevisionChanged,
};

class DocumentPointerGesture {
public:
    [[nodiscard]] bool has_value() const noexcept {
        return state_.has_value();
    }
    void clear() noexcept { state_.reset(); }
    void begin(FileDocumentId documentId, std::uint64_t documentRevision,
               DocumentPosition position, bool additive,
               std::vector<Selection> baseline);
    [[nodiscard]] DocumentPointerTargetState validateTarget(
        std::optional<FileDocumentId> documentId,
        std::uint64_t documentRevision);
    [[nodiscard]] bool additive() const;
    [[nodiscard]] SelectionCommandArguments selectionThrough(
        DocumentPosition position) const;
    void moveTo(DocumentPosition position);

private:
    struct State {
        FileDocumentId documentId;
        std::uint64_t documentRevision;
        DocumentPosition anchor;
        DocumentPosition active;
        bool additive = false;
        std::vector<Selection> baseline;
    };

    std::optional<State> state_;
};

}  // namespace ssg
