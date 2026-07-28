#pragma once

// Kind: seam.
//
// Runtime command cases, derived from the compiled command catalog.

#include <ssg/Commands.h>

#include <string>
#include <vector>

namespace ssg::test {

struct RuntimeCommandCase {
    std::string id;
    std::string owner;
};

[[nodiscard]] inline std::vector<RuntimeCommandCase> runtimeCommandCases() {
    std::vector<RuntimeCommandCase> cases;
    auto const catalog = ssg::commandCatalog();
    cases.reserve(catalog.size());
    for (auto const& command : catalog) {
        cases.push_back({std::string{command.id}, std::string{command.owner}});
    }
    return cases;
}

}  // namespace ssg::test
