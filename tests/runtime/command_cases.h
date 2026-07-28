#pragma once

// Kind: seam.
//
// Every command the editor offers, for tests that must cover all of them.
// Asked of a real runtime, because a command is declared by the component that
// implements it and no single file lists them all
// (doc/spec-command-registry.md).

#include "../all_command_ids.h"

#include <string>
#include <vector>

namespace ssg::test {

struct RuntimeCommandCase {
    std::string id;
    std::string owner;
};

[[nodiscard]] inline std::vector<RuntimeCommandCase> runtimeCommandCases() {
    std::vector<RuntimeCommandCase> cases;
    for (auto const& facts : ssg::testing::allCommandFacts()) {
        cases.push_back({facts.id, facts.owner});
    }
    return cases;
}

}  // namespace ssg::test
