#include <ssg/Picker.h>

#include "all_command_ids.h"

#include <ssg/EditorSessionBuilder.h>
#include <ssg/Search.h>

#include "test_helpers.h"

#include <algorithm>
#include <string>

namespace {

// Against the runtime's catalog: a picker's open command may be declared by the
// component that implements it rather than by the static table.
bool isP0Command(std::string_view id) {
    for (auto const& facts : ssg::testing::allCommandFacts()) {
        if (facts.id == id) return true;
    }
    return false;
}

}  // namespace

// Exhaustiveness oracle.  A picker kind is only usable if every wiring point in
// its descriptor is filled, and none of those points is checkable by the
// compiler: the catalog is a table, so a kind added to the enum but missing
// from the table (or pointing at a command id that does not exist) builds
// cleanly and fails only at runtime, as an empty prompt or a no-op open.
TEST(everyPickerKindHasACompletelyWiredDescriptor) {
    for (auto kind : ssg::kAllPickerKinds) {
        auto const* descriptor = ssg::pickerCatalog().find(kind);
        ASSERT_TRUE(descriptor != nullptr);
        if (descriptor == nullptr) continue;
        ASSERT_TRUE(descriptor->kind == kind);
        ASSERT_FALSE(descriptor->promptTitle.empty());
        ASSERT_FALSE(descriptor->openCommandId.empty());
        ASSERT_TRUE(isP0Command(descriptor->openCommandId));
    }
}

// Existence alone is a weak oracle: a descriptor naming the wrong (but real)
// command id would satisfy it.  Pin the Command picker's exact semantics so the
// P1 refactor is provably behavior-preserving -- these are the values the
// pre-refactor code hardcoded in palettePromptRequest() and paletteView().
TEST(commandPickerDescriptorMatchesThePreRefactorPaletteBehavior) {
    auto const* descriptor = ssg::pickerCatalog().find(ssg::PickerKind::Command);
    ASSERT_TRUE(descriptor != nullptr);
    if (descriptor == nullptr) return;
    ASSERT_EQ(std::string{descriptor->openCommandId}, std::string{"palette.open"});
    ASSERT_EQ(std::string{descriptor->promptTitle}, std::string{"Command Palette"});
    ASSERT_TRUE(descriptor->wireMode == ssg::SearchMode::Command);
}

TEST(catalogReportsAbsenceRatherThanFabricatingADescriptor) {
    // The lookup must be total over the enum and honest outside it; a
    // fabricated fallback descriptor would make the exhaustiveness oracle
    // above unfalsifiable.
    auto const* unknown =
        ssg::pickerCatalog().find(static_cast<ssg::PickerKind>(200));
    ASSERT_TRUE(unknown == nullptr);
}

// The static_assert in Picker.h pins the row COUNT; it cannot see that two rows
// name the same kind, which is what a copy-pasted row looks like -- the count
// still matches, and the new kind silently resolves to nullptr.
TEST(noTwoDescriptorsClaimTheSameKind) {
    auto const& descriptors = ssg::pickerCatalog().descriptors();
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        for (std::size_t j = i + 1; j < descriptors.size(); ++j) {
            ASSERT_TRUE(descriptors[i].kind != descriptors[j].kind);
        }
    }
}

int main() {
    RUN(everyPickerKindHasACompletelyWiredDescriptor);
    RUN(commandPickerDescriptorMatchesThePreRefactorPaletteBehavior);
    RUN(catalogReportsAbsenceRatherThanFabricatingADescriptor);
    RUN(noTwoDescriptorsClaimTheSameKind);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
