#include "test_helpers.h"

#include "command_catalog.h"

#include <regex>
#include <set>
#include <string>

// Kind: seam.
//
// Asserts the SHAPE and POLICY of the authored command catalog: ids are
// well-formed, every field is owned, and each command's capability/keymap/
// palette exposure matches its class.
//
// It deliberately does NOT restate the catalog's contents.  This file used to
// carry a verbatim copy of all 182 id/owner pairs, a per-category count table,
// and a `static_assert` on the total.  None of those asserted anything about
// the product -- they asserted that a human had retyped a list correctly, and
// they made every command addition a five-site edit.  The real invariant, that
// the catalog exactly equals the assembled runtime registry (capabilities
// included), is asserted by `requiredCatalogEqualsAssembledRegistryExactly` in
// tests/test_editor_session_assembly.cpp.

namespace {

using ssg::test::CatalogCommand;
using ssg::test::loadCommandCatalog;

TEST(catalogParsesAndIdsAreUniqueAndWellFormed) {
    const auto catalog = loadCommandCatalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) return;
    ASSERT_FALSE(catalog->empty());

    const std::regex validId{R"(^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$)"};
    std::set<std::string> seen;
    for (const auto& command : *catalog) {
        ASSERT_TRUE(std::regex_match(command.id, validId));
        ASSERT_FALSE(command.owner.empty());
        ASSERT_TRUE(seen.insert(command.id).second);
    }
    ASSERT_EQ(seen.size(), catalog->size());
}

TEST(capabilityAndSurfaceExclusionsAreExact) {
    const auto catalog = loadCommandCatalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    for (const auto& command : *catalog) {
        if (command.id == "file.open_dropped_content") {
            ASSERT_EQ(command.requiredCapabilities,
                      std::vector<std::string>{"local_file_drop"});
            ASSERT_FALSE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else if (command.id == "find.update_query" ||
                   command.id == "replace.update_replacement" ||
                   command.id == "prompt.update_value") {
            // Client-fulfilment commands: the client edits the query/replacement
            // and reports the full next string, so each carries a payload and is
            // neither keymap- nor palette-reachable, but remains Lua-scriptable.
            ASSERT_TRUE(command.requiredCapabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else if (command.id == "tree.select") {
            // A pointer-fulfilment command: a click selects a specific tree node
            // by id, so it carries a node-id payload and is neither keymap- nor
            // palette-reachable (the keyboard selects via next/previous), but
            // remains Lua-scriptable.
            ASSERT_TRUE(command.requiredCapabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else if (command.id == "tree.scroll" ||
                   command.id == "tree.scroll_to_fraction") {
            // A pointer-fulfilment command: the wheel scrolls the tree viewport,
            // so it carries a scroll-lines payload and is neither keymap- nor
            // palette-reachable (the keyboard scrolls via next/previous, which
            // move the selection), but remains Lua-scriptable.
            ASSERT_TRUE(command.requiredCapabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else if (command.id == "keymap.bind" ||
                   command.id == "keymap.unbind" ||
                   command.id == "style.define") {
            // Config-time commands: each requires a typed payload that neither a
            // bare keystroke nor a parameterless palette invocation can supply,
            // so both are excluded -- same shape as the client/pointer-fulfilment
            // exclusions above. Lua-only (init.lua is the only caller).
            ASSERT_TRUE(command.requiredCapabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else {
            ASSERT_TRUE(command.requiredCapabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_TRUE(command.keymap);
            ASSERT_TRUE(command.palette);
        }
    }
}

} // namespace

int main() {
    RUN(catalogParsesAndIdsAreUniqueAndWellFormed);
    RUN(capabilityAndSurfaceExclusionsAreExact);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
