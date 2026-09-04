#include <ssg/whole_screen_schema.h>

#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

UiSchema validateOrThrow(UiSchema schema) {
    const UiSchemaValidation result = validateUiSchema(schema);
    if (!result.ok()) {
        throw std::logic_error("WholeScreenSchema: assembled tree failed validation: " +
                               *result.error);
    }
    return schema;
}

}  // namespace

WholeScreenSchema::WholeScreenSchema(UiComposition initial)
    : schema_{validateOrThrow(UiSchema{std::move(initial.root)})} {}

bool WholeScreenSchema::update(UiComposition assembled) {
    // Structure comparison is the whole tree: a stable structure produces an identical
    // root, so equality means "no structural change" and the schema stays fixed.
    if (assembled.root == schema_.root) return false;
    schema_ = validateOrThrow(UiSchema{std::move(assembled.root)});
    return true;
}

}  // namespace ssg
