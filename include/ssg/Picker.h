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
enum class PickerKind : std::uint8_t { Command, File };

inline constexpr std::array<PickerKind, 2> kAllPickerKinds{
    PickerKind::Command,
    PickerKind::File,
};

// Every point a picker must wire, in one place, so adding a picker is filling a
// row rather than finding the four sites that hardcoded the command palette.
struct PickerDescriptor {
    PickerKind kind = PickerKind::Command;
    std::string_view openCommandId;
    std::string_view promptTitle;
    SearchMode wireMode = SearchMode::Command;
};

// The descriptor table is sized independently of `kAllPickerKinds` on purpose:
// sizing it by `kAllPickerKinds.size()` would silently grow it with a
// default-constructed row (kind `Command`, empty ids) when a kind is added,
// deferring the missing wiring to a runtime test.  With an independent size the
// static_assert below turns that into a compile error, and the runtime oracle in
// tests/test_picker.cpp is left to catch what a count cannot: a row that exists
// but names the wrong kind or a nonexistent command.
inline constexpr std::array<PickerDescriptor, 2> kPickerDescriptors{{
    {PickerKind::Command, "palette.open", "Command Palette", SearchMode::Command},
    {PickerKind::File, "file_finder.open", "Go to File", SearchMode::File},
}};

static_assert(kPickerDescriptors.size() == kAllPickerKinds.size(),
              "every PickerKind needs exactly one descriptor row");

class PickerCatalog {
public:
    // Null for a kind with no entry, rather than a fabricated fallback: the
    // exhaustiveness oracle can only fail if absence is observable.
    [[nodiscard]] PickerDescriptor const* find(PickerKind kind) const noexcept;

    [[nodiscard]] std::array<PickerDescriptor, kPickerDescriptors.size()> const&
    descriptors() const noexcept {
        return kPickerDescriptors;
    }
};

[[nodiscard]] PickerCatalog const& pickerCatalog() noexcept;

}  // namespace ssg
