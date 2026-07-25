// The decisive check that highlight queries are compiled into the binary rather
// than read from the source tree.
//
// This is a SEPARATE executable on purpose.  TreeSitterParser's compiled-query
// cache is a function-local static, so it is process-wide, not per-parser: any
// earlier test that highlights a language leaves that language's query cached
// and makes this check vacuous.  Constructing a fresh parser is NOT enough --
// only a fresh process is.  (Verified by perturbation: run in-process alongside
// the golden tests, a reverted-to-file-reading implementation passes.)
#include "TreeSitterParser.h"
#include "test_helpers.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

SyntaxScope scopeAtOffset(const std::vector<SyntaxSpan>& spans, std::size_t offset) {
    for (const auto& span : spans) {
        if (span.begin.value() <= offset && offset < span.end.value()) {
            return span.scope;
        }
    }
    return SyntaxScope::PlainText;
}

TEST(highlightingWorksWithTheVendorQueryFilesRemoved) {
    const fs::path vendorRoot = fs::path{SSG_TREESITTER_VENDOR_DIR};
    const std::vector<std::string> grammarDirs{
        "tree-sitter-c",          "tree-sitter-cpp",
        "tree-sitter-javascript", "tree-sitter-typescript",
        "tree-sitter-c-sharp",    "tree-sitter-lua"};

    // Establish the premise: these are the files the old implementation read.
    std::vector<fs::path> queryFiles;
    for (const auto& dir : grammarDirs) {
        auto path = vendorRoot / dir / "queries" / "highlights.scm";
        if (fs::exists(path)) queryFiles.push_back(path);
    }
    ASSERT_FALSE(queryFiles.empty());

    // Rename rather than delete, and restore unconditionally, so a crash here
    // leaves a recoverable checkout.
    std::vector<std::pair<fs::path, fs::path>> hidden;
    for (const auto& path : queryFiles) {
        auto stashed = path;
        stashed += ".hidden-by-test";
        std::error_code error;
        fs::rename(path, stashed, error);
        if (!error) hidden.emplace_back(path, stashed);
    }
    struct Restore {
        const std::vector<std::pair<fs::path, fs::path>>& entries;
        ~Restore() {
            for (const auto& [original, stashed] : entries) {
                std::error_code error;
                fs::rename(stashed, original, error);
            }
        }
    } restore{hidden};
    ASSERT_EQ(hidden.size(), queryFiles.size());

    // Every grammar, not just one: a partially-embedded table (the generator's
    // most likely failure) would otherwise pass on whichever grammar came first.
    const std::vector<std::pair<std::string, std::string>> cases{
        {"c", "int main() { return 0; }\n"},
        {"cpp", "class Widget { int value; };\n"},
        {"javascript", "function main() { return 0; }\n"},
        {"typescript", "function main(): number { return 0; }\n"},
        {"csharp", "class Widget { int Value; }\n"},
        {"lua", "local function main() return 0 end\n"},
    };

    auto parser = std::make_shared<TreeSitterParser>();
    SyntaxModel model{parser};
    std::uint64_t revision = 1;
    for (const auto& [language, source] : cases) {
        const auto request =
            model.request(Revision{revision++}, LanguageId{language}, source);
        ASSERT_TRUE(request.accepted());
        if (!request.accepted()) continue;
        const auto output = model.run(*request.request);
        ASSERT_FALSE(output.spans.empty());

        // With no query loaded the parse still succeeds but every span is
        // PlainText, so a non-plain scope proves the query was available.
        bool highlighted = false;
        for (const auto& span : output.spans) {
            if (span.scope != SyntaxScope::PlainText) highlighted = true;
        }
        ASSERT_TRUE(highlighted);
    }
}

}  // namespace

int main() {
    RUN(highlightingWorksWithTheVendorQueryFilesRemoved);
    return failed == 0 ? 0 : 1;
}
