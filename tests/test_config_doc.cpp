#include <ssg/InitScriptCatalog.h>

#include "test_helpers.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SSG_CONFIG_DOC_PATH
#error "SSG_CONFIG_DOC_PATH must name doc/config.md"
#endif

namespace {

std::string readFile(const char* path) {
    std::ifstream input{path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

// Every id in ssg::kInitScriptCommands (the actual init.lua Lua surface --
// see InitScriptCatalog.h and apps/ssg_main.cpp's initScriptCommandCatalog)
// must appear backticked in doc/config.md, so a newly Lua-exposed command
// can't ship without a user-facing mention. One-directional (unlike
// test_required_commands.cpp's bidirectional feature-doc check): doc/
// config.md's keymap.bind section also backtick-mentions ordinary P0
// command ids (file.save, edit.undo, ...) purely as bind-target examples,
// which are not themselves init-script commands.
TEST(configDocMentionsEveryInitScriptCommand) {
    const auto doc = readFile(SSG_CONFIG_DOC_PATH);
    ASSERT_FALSE(doc.empty());
    for (const auto& command : ssg::kInitScriptCommands) {
        const std::string needle = "`" + std::string{command.id} + "`";
        ASSERT_TRUE(doc.find(needle) != std::string::npos);
    }
}

} // namespace

int main() {
    RUN(configDocMentionsEveryInitScriptCommand);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
