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

// Every table-based configuration operation must appear in doc/config.md so a
// newly exposed operation cannot ship without a user-facing mention.
TEST(configDocMentionsEveryInitScriptCommand) {
    const auto doc = readFile(SSG_CONFIG_DOC_PATH);
    ASSERT_FALSE(doc.empty());
    for (const std::string_view id :
         {"theme.set", "style.define", "keymap.bind", "keymap.unbind"}) {
        const std::string needle = "`" + std::string{id} + "`";
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
        const std::string needle = "ssg." + std::string{function.name};
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
