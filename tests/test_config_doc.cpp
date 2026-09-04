#include "all_command_ids.h"

#include <ssg/LuaCommandHost.h>

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

// Every command granted to init.lua (surfaces.initScript in the command
// catalog, which src/application.cpp's initScriptCommandCatalog builds from)
// must appear backticked in doc/config.md, so a newly Lua-exposed command
// can't ship without a user-facing mention. One-directional (unlike
// test_required_commands.cpp's bidirectional feature-doc check): doc/
// config.md's keymap.bind section also backtick-mentions ordinary P0
// command ids (file.save, edit.undo, ...) purely as bind-target examples,
// which are not themselves init-script commands.
TEST(configDocMentionsEveryInitScriptCommand) {
    const auto doc = readFile(SSG_CONFIG_DOC_PATH);
    ASSERT_FALSE(doc.empty());
    for (const auto& command : ssg::testing::allCommandFacts()) {
        if (!command.initScript) continue;
        const std::string needle = "`" + command.id + "`";
        ASSERT_TRUE(doc.find(needle) != std::string::npos);
    }
}

// Every function the Lua API exposes must be described in doc/config.md.
// A function a user can call but cannot read about is undiscoverable, and the
// only way to learn it exists is to read the source.
TEST(configDocDescribesEveryLuaApiFunction) {
    const auto doc = readFile(SSG_CONFIG_DOC_PATH);
    ASSERT_FALSE(doc.empty());
    for (const auto& function : ssg::LuaCommandHost::kApiFunctions) {
        const std::string needle = "ssg." + std::string{function};
        ASSERT_TRUE(doc.find(needle) != std::string::npos);
    }
}

} // namespace

SSG_TEST_SUITE(test_config_doc) {
    RUN(configDocMentionsEveryInitScriptCommand);
    RUN(configDocDescribesEveryLuaApiFunction);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
