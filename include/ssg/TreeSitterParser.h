#pragma once

#include <ssg/SyntaxModel.h>
#include <ssg/TreeSitterGrammars.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ssg {

class TreeSitterParser final : public SyntaxParser {
public:
    // Compiled queries for THIS parser. Public only so the compile helper in
    // the .cpp can name it; this is an internal header, not public API.
    struct QueryCache;

    // Defaults to the vendored grammars, so existing callers are unaffected.
    TreeSitterParser();
    explicit TreeSitterParser(std::vector<TreeSitterGrammar> grammars);
    ~TreeSitterParser() override;

    [[nodiscard]] bool hasGrammar(const LanguageId& language) const override;
    [[nodiscard]] SyntaxParseOutput parse(
        const SyntaxParseRequest& request) override;

private:

    // Index rather than pointer: the compiled-query cache is keyed by it, since
    // language ids are host-supplied and not guaranteed unique.
    [[nodiscard]] std::optional<std::size_t> grammarIndexFor(
        const LanguageId& language) const;

    std::vector<TreeSitterGrammar> grammars_;
    // Per-parser, not process-wide: two parsers may now register different
    // query text for the same language id, and a cache shared across parsers
    // and keyed by id would serve one parser's query to the other.
    std::unique_ptr<QueryCache> queries_;
};

} // namespace ssg
