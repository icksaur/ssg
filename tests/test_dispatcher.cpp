#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "all_command_ids.h"
#include "suite_declarations.inc"

namespace {

int unexpectedSuiteArguments(std::string_view suite) {
    std::cerr << "test suite does not accept arguments: " << suite << '\n';
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view{argv[1]} == "--dump-command-surface") {
        for (auto const& command : ssg::testing::allCommandFacts()) {
            std::cout << command.id << '\t' << command.label << '\t'
                      << ssg::testing::classifyCommand(command) << '\n';
        }
        return 0;
    }
    if (argc < 3 || std::string_view{argv[1]} != "--suite") {
        std::cerr << "usage: ssg_tests --suite <name> [suite arguments]\n"
                     "       ssg_tests --dump-command-surface\n";
        return 2;
    }

    const std::string_view suite{argv[2]};
    std::vector<char*> suiteArguments;
    suiteArguments.reserve(static_cast<std::size_t>(argc));
    suiteArguments.push_back(argv[0]);
    for (int index = 3; index < argc; ++index) {
        suiteArguments.push_back(argv[index]);
    }
    suiteArguments.push_back(nullptr);

#include "suite_cases.inc"

    std::cerr << "unknown test suite: " << suite << '\n';
    return 2;
}
