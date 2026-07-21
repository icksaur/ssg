#include <ssg/SyntaxModel.h>

#include "test_helpers.h"

#include <array>
#include <string_view>

namespace {

using namespace ssg;

struct MappingCase {
    std::string_view path;
    std::string_view language;
};

constexpr std::array<MappingCase, 20> kMappings{{
    {"main.c", "c"},
    {"header.h", "c"},
    {"engine.cc", "cpp"},
    {"engine.cpp", "cpp"},
    {"engine.cxx", "cpp"},
    {"engine.hpp", "cpp"},
    {"engine.hh", "cpp"},
    {"engine.hxx", "cpp"},
    {"app.js", "javascript"},
    {"app.mjs", "javascript"},
    {"app.cjs", "javascript"},
    {"app.ts", "typescript"},
    {"app.tsx", "typescript"},
    {"tool.cs", "csharp"},
    {"script.lua", "lua"},
    {"APP.TS", "typescript"},
    {"APP.HPP", "cpp"},
    {"no_extension", "plain_text"},
    {"readme.md", "plain_text"},
    {"archive.tar.gz", "plain_text"},
}};

} // namespace

TEST(languageDetectionFromPath) {
    for (const auto& mapping : kMappings) {
        ASSERT_EQ(LanguageId::fromPath(mapping.path).value(), mapping.language);
    }
}

int main() {
    RUN(languageDetectionFromPath);
    if (failed != 0) {
        std::cerr << "FAILED (" << failed << " failures, " << passed
                  << " assertions passed)\n";
        return 1;
    }
    std::cout << "OK (" << passed << " assertions)\n";
    return 0;
}
