#pragma once

#include <ssg/types.h>

#include <optional>
#include <string>

namespace ssg {

struct DocumentViewState {
    Revision revision;
    std::string text;
    ByteOffset caret;
    std::optional<std::string> diffFileIdentity;

    bool operator==(DocumentViewState const&) const = default;
};

}  // namespace ssg
