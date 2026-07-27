#pragma once

// Kind: seam.
//
// THE single loader for `data/required-commands.json`, the one authored source
// of the command catalog.  Tests that need the catalog parse it here rather
// than restating it: hand-copied inventories assert only that a human retyped a
// list correctly, and every copy is another edit site when a command is added.
//
// The product invariant -- that the catalog and the assembled runtime registry
// are exactly equal, capabilities included -- is asserted once, by
// `requiredCatalogEqualsAssembledRegistryExactly` in
// tests/test_editor_session_assembly.cpp.
//
// Requires SSG_REQUIRED_COMMANDS_PATH as a compile definition.

#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace ssg::test {

struct CatalogCommand {
    std::string id;
    std::string owner;
    std::vector<std::string> requiredCapabilities;
    bool lua{};
    bool keymap{};
    bool palette{};
};

namespace catalog_detail {

inline std::optional<std::string> field(std::string const& object,
                                         std::string const& name) {
    std::regex const expression{"\"" + name + R"regex("\s*:\s*"([^"]*)")regex"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) return std::nullopt;
    return match[1].str();
}

inline std::optional<bool> booleanField(std::string const& object,
                                         std::string const& name) {
    std::regex const expression{"\"" + name + R"("\s*:\s*(true|false))"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) return std::nullopt;
    return match[1].str() == "true";
}

inline std::optional<std::vector<std::string>> capabilitiesField(
    std::string const& object) {
    std::regex const expression{R"("required_capabilities"\s*:\s*\[([^\]]*)\])"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) return std::nullopt;
    std::vector<std::string> capabilities;
    std::string const contents = match[1].str();
    std::regex const valueExpression{R"regex("([^"]+)")regex"};
    for (auto it = std::sregex_iterator(contents.begin(), contents.end(),
                                        valueExpression);
         it != std::sregex_iterator(); ++it) {
        capabilities.push_back((*it)[1].str());
    }
    return capabilities;
}

}  // namespace catalog_detail

// Parses the catalog.  Returns nullopt when the file is unreadable or any entry
// is malformed, so a silently-truncated parse cannot masquerade as a small
// catalog.
[[nodiscard]] inline std::optional<std::vector<CatalogCommand>> loadCommandCatalog() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto const json = buffer.str();

    std::vector<CatalogCommand> commands;
    std::regex const objectExpression{R"(\{([^{}]*)\})"};
    for (auto it = std::sregex_iterator(json.begin(), json.end(),
                                        objectExpression);
         it != std::sregex_iterator(); ++it) {
        auto const object = (*it)[1].str();
        auto const id = catalog_detail::field(object, "id");
        if (!id) continue;
        auto const owner = catalog_detail::field(object, "owner");
        auto const capabilities = catalog_detail::capabilitiesField(object);
        auto const lua = catalog_detail::booleanField(object, "lua");
        auto const keymap = catalog_detail::booleanField(object, "keymap");
        auto const palette = catalog_detail::booleanField(object, "palette");
        if (!owner || !capabilities || !lua || !keymap || !palette) {
            return std::nullopt;
        }
        commands.push_back({*id, *owner, *capabilities, *lua, *keymap, *palette});
    }

    // Every `"id":` token must have produced an entry, so a regex that silently
    // skipped a malformed object cannot pass as a complete catalog.
    std::regex const idToken{R"("id"\s*:)"};
    auto const idCount = static_cast<std::size_t>(
        std::distance(std::sregex_iterator(json.begin(), json.end(), idToken),
                      std::sregex_iterator()));
    if (idCount != commands.size()) return std::nullopt;
    return commands;
}

}  // namespace ssg::test
