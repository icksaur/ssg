#pragma once

#include <ssg/Selection.h>

#include <array>
#include <string_view>

namespace ssg {

// CONTRACT: This is the single selection-command inventory consumed by
// registration and command lookup.
struct SelectionCommandDescriptor {
    std::string_view id;
    SelectionCommand command;

};

extern const std::array<SelectionCommandDescriptor, 38> kSelectionCommands;

}  // namespace ssg
