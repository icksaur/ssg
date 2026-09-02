#include "runtime/whole_screen_schema.h"

#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

ValidatedSchema validateOrThrow(UiSchema schema) {
    auto result = ValidatedSchema::validate(std::move(schema));
    if (!result.ok()) {
        throw std::logic_error("WholeScreenSchema: assembled tree failed validation: " +
                               result.error());
    }
    return result.takeSchema();
}

}  // namespace

WholeScreenSchema::WholeScreenSchema(UiComposition initial)
    : schema_{validateOrThrow(UiSchema{Generation{0}, std::move(initial.root)})} {}

bool WholeScreenSchema::update(UiComposition assembled) {
    // Structure comparison is the whole tree: a stable structure produces an identical
    // root, so equality means "no structural change" and the generation stays fixed.
    if (assembled.root == schema_.schema().root) return false;
    const std::uint64_t next = schema_.generation().value() + 1;
    schema_ = validateOrThrow(UiSchema{Generation{next}, std::move(assembled.root)});
    return true;
}

}  // namespace ssg
