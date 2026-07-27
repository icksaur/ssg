#pragma once

// Kind: seam.
//
// Runtime command cases, derived from `data/required-commands.json` rather than
// restated.  This file used to carry a hand-maintained copy of all 182 command
// ids and owners plus a `static_assert` on the total, which meant adding one
// command required editing two inventories and two counts that asserted nothing
// about the product.

#include "command_catalog.h"

#include <string>
#include <vector>

namespace ssg::test {

struct RuntimeCommandCase {
    std::string id;
    std::string owner;
};

// Empty when the catalog cannot be parsed; callers assert non-empty so a broken
// load fails loudly instead of vacuously passing a zero-iteration loop.
[[nodiscard]] inline std::vector<RuntimeCommandCase> runtimeCommandCases() {
    std::vector<RuntimeCommandCase> cases;
    auto const catalog = loadCommandCatalog();
    if (!catalog) return cases;
    cases.reserve(catalog->size());
    for (auto const& command : *catalog) {
        cases.push_back({command.id, command.owner});
    }
    return cases;
}

}  // namespace ssg::test
