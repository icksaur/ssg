#include "TreeSitterParser.h"
#include "test_helpers.h"

#include <ssg/Theme.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

struct FixtureCase {
    std::string language;
    std::string sourceFile;
    std::string goldenFile;
    std::string overlapToken;
    SyntaxScope overlapScope = SyntaxScope::PlainText;
    bool overlapUseLast = false;
};

std::string readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool isUtf8Boundary(const std::string& text, std::size_t offset) {
    if (offset == 0 || offset == text.size()) {
        return true;
    }
    const auto byte = static_cast<unsigned char>(text[offset]);
    return (byte & 0xC0u) != 0x80u;
}

SyntaxScope scopeAtOffset(const std::vector<SyntaxSpan>& spans, std::size_t offset) {
    for (const auto& span : spans) {
        if (span.begin.value() <= offset && offset < span.end.value()) {
            return span.scope;
        }
    }
    return SyntaxScope::PlainText;
}

std::string serializeSpans(const std::vector<SyntaxSpan>& spans) {
    std::ostringstream output;
    for (const auto& span : spans) {
        output << span.begin.value() << ' ' << span.end.value() << ' '
               << syntaxScopeName(span.scope) << '\n';
    }
    return output.str();
}

bool matchesGolden(const std::string& actual, const fs::path& golden) {
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{golden, std::ios::binary} << actual;
        return true;
    }
    return actual == readFile(golden);
}

TEST(treeSitterSyntaxGoldenByLanguage) {
    const fs::path fixtureRoot = SSG_TREESITTER_FIXTURE_DIR;
    auto parser = std::make_shared<TreeSitterParser>();
    SyntaxModel model{parser};

    const std::vector<FixtureCase> cases{
        {"c", "c.c", "c.golden", "main(", SyntaxScope::Function},
        {"cpp", "cpp.cpp", "cpp.golden", "", SyntaxScope::PlainText},
        {"javascript", "javascript.js", "javascript.golden", "method(value)", SyntaxScope::Function},
        {"typescript", "typescript.ts", "typescript.golden", "",
         SyntaxScope::PlainText, false},
        {"csharp", "csharp.cs", "csharp.golden", "", SyntaxScope::PlainText},
        {"lua", "lua.lua", "lua.golden", "add(", SyntaxScope::Function},
    };

    std::uint64_t revision = 1;
    for (const auto& fixture : cases) {
        const auto text = readFile(fixtureRoot / fixture.sourceFile);
        const auto request = model.request(Revision{revision++}, LanguageId{fixture.language}, text);
        ASSERT_TRUE(request.accepted());
        if (!request.accepted()) {
            continue;
        }
        const auto output = model.run(*request.request);
        ASSERT_EQ(output.status, SyntaxParseStatus::Parsed);
        ASSERT_TRUE(output.parse != nullptr);
        for (const auto& span : output.spans) {
            ASSERT_TRUE(isUtf8Boundary(text, span.begin.value()));
            ASSERT_TRUE(isUtf8Boundary(text, span.end.value()));
        }

        if (!fixture.overlapToken.empty()) {
            const auto overlapOffset = fixture.overlapUseLast
                                           ? text.rfind(fixture.overlapToken)
                                           : text.find(fixture.overlapToken);
            ASSERT_TRUE(overlapOffset != std::string::npos);
            if (overlapOffset != std::string::npos) {
                ASSERT_EQ(scopeAtOffset(output.spans, overlapOffset),
                          fixture.overlapScope);
            }
        }

        const auto serialized = serializeSpans(output.spans);
        ASSERT_TRUE(matchesGolden(serialized, fixtureRoot / fixture.goldenFile));
    }
}

} // namespace

int main() {
    RUN(treeSitterSyntaxGoldenByLanguage);
    return failed == 0 ? 0 : 1;
}
