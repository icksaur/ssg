#pragma once

// The private snapshot-publication step: fills UiNode::resolved on a copy of
// the interaction schema from typed runtime values, addressed by the tree's
// own fixed, well-known UiNodeId identities (StatusFields.h and UiTree.h).
// This is tree addressing, not a provider registry: there is no runtime
// collection of arbitrary keys or callbacks. An expected fixed node that is
// missing or the wrong widget kind is a broken invariant, thrown rather than
// silently skipped.

#include "prompt_resolution.h"

#include <ssg/StatusFields.h>
#include <ssg/StatusQueue.h>
#include <ssg/UiTree.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg::detail {

struct UiTreeValues {
    StatusFieldProjection status;
    std::string helpHintLabel;
    std::vector<StatusActionNode> statusActions;
    std::optional<ResolvedPromptControls> prompt;
};

// Populate `schema`'s fixed leaves -- the header/footer status fields, the
// help hint, the status actions, and any active footer-region prompt controls
// -- from `values`.
void populateUiTree(UiSchema& schema, const UiTreeValues& values);

}  // namespace ssg::detail
