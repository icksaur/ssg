#pragma once

// Private runtime owner of the whole-screen schema. The schema is STRUCTURALLY
// STABLE (WholeScreenAssembly builds it from the stable catalog + provider-backed
// content), so update() replaces the schema only when the structure actually
// changes -- a dimensions change or a catalog change that alters the
// header/footer subtree. A caller that rebuilds the interaction aggregate on a
// change therefore rebuilds only on a real structural change, never on
// per-frame value churn.

#include <ssg/UiTree.h>  // UiComposition, UiSchema
#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/Style.h>

#include <string_view>
#include <vector>

namespace ssg {

class WholeScreenSchema {
public:
    // Seed from the initial assembled composition. The composition must validate
    // (WholeScreenAssembly always produces a valid tree); a validation failure is
    // a broken invariant, thrown, never a silently invalid schema.
    explicit WholeScreenSchema(UiComposition initial);

    // Adopt `assembled` as the current schema iff its root differs structurally
    // from the current root; returns true iff the schema was replaced. An
    // identical structure keeps the current schema (no churn).
    bool update(UiComposition assembled);

    [[nodiscard]] const UiSchema& schema() const noexcept {
        return schema_;
    }

private:
    UiSchema schema_;
};

[[nodiscard]] UiComposition assembleWholeScreen(
    std::string_view hintCommandId, const StyleDimensions& dimensions,
    std::string_view promptSigil);

[[nodiscard]] UiComposition withFooterPrompt(UiComposition base,
                                             const PromptSurface& prompt);
[[nodiscard]] UiComposition withStatusActions(
    UiComposition base, const std::vector<StatusActionNode>& actions);
[[nodiscard]] UiNode assembleFooterPrompt(const PromptSurface& prompt);

}  // namespace ssg
