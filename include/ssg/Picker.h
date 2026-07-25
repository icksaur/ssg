#pragma once

#include <ssg/Search.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ssg {

// Which picker a `PromptKind::Palette` prompt belongs to.
//
// A dedicated enum rather than reusing `SearchMode` as the discriminator:
// `SearchMode` is the WIRE vocabulary and three of its five values (`Line`,
// `Symbol`, `Text`) are not pickers and never will be, which would dilute the
// exhaustiveness check in tests/test_picker.cpp into a mostly-inapplicable
// loop.  The wire mode a picker publishes is carried by its descriptor.
enum class PickerKind : std::uint8_t { Command };

inline constexpr std::array<PickerKind, 1> kAllPickerKinds{
    PickerKind::Command,
};

// Every point a picker must wire, in one place, so adding a picker is filling a
// row rather than finding the four sites that hardcoded the command palette.
struct PickerDescriptor {
    PickerKind kind = PickerKind::Command;
    std::string_view openCommandId;
    std::string_view promptTitle;
    SearchMode wireMode = SearchMode::Command;
};

class PickerCatalog {
public:
    // Null for a kind with no entry, rather than a fabricated fallback: the
    // exhaustiveness oracle can only fail if absence is observable.
    [[nodiscard]] PickerDescriptor const* find(PickerKind kind) const noexcept;

    [[nodiscard]] std::array<PickerDescriptor, kAllPickerKinds.size()> const&
    descriptors() const noexcept {
        return descriptors_;
    }

private:
    static constexpr std::array<PickerDescriptor, kAllPickerKinds.size()>
        descriptors_{{
            {PickerKind::Command, "palette.open", "Command Palette",
             SearchMode::Command},
        }};
};

[[nodiscard]] PickerCatalog const& pickerCatalog() noexcept;

}  // namespace ssg
