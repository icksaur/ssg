#include <ssg/DocumentPointerGesture.h>

#include <stdexcept>
#include <utility>

namespace ssg {

void DocumentPointerGesture::begin(
    FileDocumentId documentId, std::uint64_t documentRevision,
    DocumentPosition position, bool additive,
    std::vector<Selection> baseline) {
    state_ = State{documentId, documentRevision, position, position, additive,
                   std::move(baseline)};
}

DocumentPointerTargetState DocumentPointerGesture::validateTarget(
    std::optional<FileDocumentId> documentId,
    std::uint64_t documentRevision) {
    if (!state_) {
        throw std::logic_error{
            "cannot validate an inactive document pointer gesture"};
    }
    if (documentId !=
        std::optional<FileDocumentId>{state_->documentId}) {
        clear();
        return DocumentPointerTargetState::DocumentChanged;
    }
    if (documentRevision != state_->documentRevision) {
        clear();
        return DocumentPointerTargetState::RevisionChanged;
    }
    return DocumentPointerTargetState::Current;
}

bool DocumentPointerGesture::additive() const {
    if (!state_) {
        throw std::logic_error{
            "cannot inspect an inactive document pointer gesture"};
    }
    return state_->additive;
}

SelectionCommandArguments DocumentPointerGesture::selectionThrough(
    DocumentPosition position) const {
    if (!state_) {
        throw std::logic_error{
            "cannot extend an inactive document pointer gesture"};
    }
    if (!state_->additive) {
        return {std::nullopt, Selection{state_->anchor, position}};
    }
    auto ranges = state_->baseline;
    ranges.push_back(Selection{state_->anchor, position});
    return {std::nullopt, std::nullopt, std::move(ranges)};
}

void DocumentPointerGesture::moveTo(DocumentPosition position) {
    if (!state_) {
        throw std::logic_error{
            "cannot move an inactive document pointer gesture"};
    }
    state_->active = position;
}

}  // namespace ssg
