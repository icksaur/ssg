#pragma once

// Private runtime owner of the whole-screen schema and its generation. The schema is
// STRUCTURALLY STABLE (WholeScreenAssembly builds it from the stable catalog + provider-
// backed content), so its generation must advance ONLY when the structure actually
// changes -- a dimensions change or a catalog change that alters the
// header/footer subtree. This owner enforces exactly that: update() with a
// freshly assembled composition advances the generation iff the root differs
// structurally from the current one, and otherwise keeps the generation fixed. A caller
// that rebuilds the interaction aggregate on a generation change therefore rebuilds only
// on a real structural change, never on per-frame value churn.

#include <ssg/UiTree.h>  // UiComposition, ValidatedSchema, Generation

namespace ssg {

class WholeScreenSchema {
public:
    // Seed at generation 0 from the initial assembled composition. The composition must
    // validate (WholeScreenAssembly always produces a valid tree); a validation failure
    // is a broken invariant, thrown, never a silently invalid schema.
    explicit WholeScreenSchema(UiComposition initial);

    // Adopt `assembled` as the current schema. Advances the generation iff its root
    // differs structurally from the current root; returns true iff the generation
    // advanced. An identical structure keeps the same generation (no churn).
    bool update(UiComposition assembled);

    [[nodiscard]] const ValidatedSchema& validated() const noexcept {
        return schema_;
    }
    [[nodiscard]] Generation generation() const noexcept {
        return schema_.generation();
    }

private:
    ValidatedSchema schema_;
};

}  // namespace ssg
