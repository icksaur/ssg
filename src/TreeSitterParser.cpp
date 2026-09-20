#include <ssg/TreeSitterParser.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ssg {

// Defined by the generated translation unit (cmake/embed_text.cmake).  Returns
// the compiled-in query text for a key from the table below, or an empty view
// for an unknown key -- which can only mean the table and the CMake embed list
// have drifted apart.
std::string_view embeddedHighlightQuery(std::string_view key);

namespace {

extern "C" {
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_c_sharp();
const TSLanguage* tree_sitter_markdown();
const TSLanguage* tree_sitter_markdown_inline();
}

class TreeSitterParse final : public OpaqueSyntaxParse {};

// Adapts a tree-sitter entry point to the public opaque handle.  Captureless
// lambdas convert to plain function pointers, which keeps this well-defined --
// casting between function pointer types would not be.
TreeSitterGrammar vendoredGrammar(std::vector<std::string> ids,
                                  SyntaxLanguageFactory language,
                                  std::string_view queryKey,
                                  std::string_view inheritsQueryKey) {
    TreeSitterGrammar grammar;
    grammar.languageIds = std::move(ids);
    grammar.language = language;
    grammar.highlightQuery = std::string{embeddedHighlightQuery(queryKey)};
    if (!inheritsQueryKey.empty()) {
        grammar.inheritedHighlightQuery =
            std::string{embeddedHighlightQuery(inheritsQueryKey)};
    }
    return grammar;
}

}  // namespace

std::vector<TreeSitterGrammar> TreeSitterParserFactory::vendoredGrammars() {
    std::vector<TreeSitterGrammar> grammars;
    grammars.push_back(vendoredGrammar(
        {"c"}, []() -> SyntaxLanguageHandle { return tree_sitter_c(); }, "c", ""));
    grammars.push_back(vendoredGrammar(
        {"cpp", "c++", "cc"},
        []() -> SyntaxLanguageHandle { return tree_sitter_cpp(); }, "cpp", "c"));
    grammars.push_back(vendoredGrammar(
        {"javascript", "js"},
        []() -> SyntaxLanguageHandle { return tree_sitter_javascript(); },
        "javascript", ""));
    grammars.push_back(vendoredGrammar(
        {"typescript", "ts"},
        []() -> SyntaxLanguageHandle { return tree_sitter_typescript(); },
        "typescript", "javascript"));
    grammars.push_back(vendoredGrammar(
        {"csharp", "c#", "cs"},
        []() -> SyntaxLanguageHandle { return tree_sitter_c_sharp(); }, "csharp",
        ""));
    grammars.push_back(vendoredGrammar(
        {"markdown", "md"},
        []() -> SyntaxLanguageHandle { return tree_sitter_markdown(); },
        "markdown", ""));
    // Markdown's inline half runs inside the block grammar's `inline` nodes.
    grammars.back().injection = TreeSitterGrammar::Injection{
        "inline",
        []() -> SyntaxLanguageHandle { return tree_sitter_markdown_inline(); },
        std::string{embeddedHighlightQuery("markdown_inline")}};
    return grammars;
}

std::shared_ptr<SyntaxParser> TreeSitterParserFactory::create(
    std::vector<TreeSitterGrammar> grammars) {
    return std::make_shared<TreeSitterParser>(std::move(grammars));
}

std::shared_ptr<SyntaxParser> TreeSitterParserFactory::createDefault() {
    return std::make_shared<TreeSitterParser>();
}

// Compiled queries for one parser, keyed by the grammar's INDEX in that
// parser's registration list.  Not keyed by language id: the public contract
// does not require ids to be unique or non-empty across registrations, so two
// valid-looking grammars could collide on a string key and silently share a
// compiled query.  An index is unique by construction.
struct TreeSitterParser::QueryCache {
    std::mutex mutex;
    std::unordered_map<std::size_t,
                       std::unique_ptr<TSQuery, decltype(&ts_query_delete)>>
        compiled;
    std::unordered_set<std::size_t> failed;
};

TreeSitterParser::TreeSitterParser()
    : TreeSitterParser(TreeSitterParserFactory::vendoredGrammars()) {}

TreeSitterParser::TreeSitterParser(std::vector<TreeSitterGrammar> grammars)
    : grammars_(std::move(grammars)),
      queries_(std::make_unique<QueryCache>()) {}

TreeSitterParser::~TreeSitterParser() = default;

std::optional<std::size_t> TreeSitterParser::grammarIndexFor(
    const LanguageId& language) const {
    const auto& id = language.value();
    for (std::size_t index = 0; index < grammars_.size(); ++index) {
        const auto& grammar = grammars_[index];
        if (grammar.language == nullptr) continue;
        for (const auto& alias : grammar.languageIds) {
            if (!alias.empty() && alias == id) return index;
        }
    }
    return std::nullopt;
}

namespace {

const TSQuery* queryFor(TreeSitterParser::QueryCache& cache,
                        const TreeSitterGrammar& grammar, std::size_t key) {
    std::lock_guard<std::mutex> lock{cache.mutex};
    if (const auto found = cache.compiled.find(key); found != cache.compiled.end()) {
        return found->second.get();
    }
    if (cache.failed.contains(key)) {
        return nullptr;
    }

    std::string source;
    if (!grammar.inheritedHighlightQuery.empty()) {
        source += grammar.inheritedHighlightQuery;
        source += '\n';
    }
    source += grammar.highlightQuery;
    if (source.empty()) {
        cache.failed.insert(key);
        return nullptr;
    }
    if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
        cache.failed.insert(key);
        return nullptr;
    }

    std::uint32_t errorOffset = 0;
    TSQueryError errorType = TSQueryErrorNone;
    auto query = std::unique_ptr<TSQuery, decltype(&ts_query_delete)>(
        ts_query_new(static_cast<const TSLanguage*>(grammar.language()),
                     source.data(), static_cast<std::uint32_t>(source.size()),
                     &errorOffset, &errorType),
        &ts_query_delete);
    (void)errorOffset;
    if (!query || errorType != TSQueryErrorNone) {
        cache.failed.insert(key);
        return nullptr;
    }

    const auto inserted = cache.compiled.emplace(key, std::move(query));
    return inserted.first->second.get();
}

bool hasTag(std::string_view capture, std::string_view prefix) {
    return capture == prefix ||
           (capture.size() > prefix.size() &&
            capture.starts_with(prefix) && capture[prefix.size()] == '.');
}

SyntaxScope scopeForCapture(std::string_view captureName) {
    if (hasTag(captureName, "comment")) {
        return SyntaxScope::Comment;
    }
    if (hasTag(captureName, "keyword")) {
        return SyntaxScope::Keyword;
    }
    if (captureName == "char" || hasTag(captureName, "string")) {
        return SyntaxScope::String;
    }
    if (captureName == "float" || hasTag(captureName, "number")) {
        return SyntaxScope::Number;
    }
    if (captureName == "constructor" || hasTag(captureName, "type")) {
        return SyntaxScope::Type;
    }
    if (captureName == "method" || hasTag(captureName, "function")) {
        return SyntaxScope::Function;
    }
    if (captureName == "parameter" || hasTag(captureName, "parameter") ||
        captureName == "property" || hasTag(captureName, "property") ||
        captureName == "field" || hasTag(captureName, "field") ||
        hasTag(captureName, "variable")) {
        return SyntaxScope::Variable;
    }
    if (captureName == "operator" || hasTag(captureName, "operator")) {
        return SyntaxScope::OperatorToken;
    }
    if (captureName == "delimiter" || hasTag(captureName, "delimiter") ||
        captureName == "bracket" || hasTag(captureName, "bracket") ||
        hasTag(captureName, "punctuation")) {
        return SyntaxScope::Punctuation;
    }
    // The `text.*` family, used by prose grammars (markdown) and by any grammar
    // following the nvim-treesitter convention.  Without these a markdown
    // document highlights only its punctuation: headings, code blocks and links
    // all carry `text.*` captures and would otherwise fall through to plain.
    if (captureName == "text.title" || hasTag(captureName, "markup.heading")) {
        return SyntaxScope::Keyword;  // Headings: the most prominent scope.
    }
    if (captureName == "text.literal" || hasTag(captureName, "markup.raw")) {
        return SyntaxScope::String;  // Code blocks and spans read as literals.
    }
    if (captureName == "text.uri" || hasTag(captureName, "markup.link")) {
        return SyntaxScope::Function;  // Link destinations, like a call target.
    }
    if (captureName == "text.reference") {
        return SyntaxScope::Variable;  // A label naming something defined later.
    }
    return SyntaxScope::PlainText;
}

// Write every capture of `query` over `root` into `perByte`, shifted by
// `byteOffset`.  Shared by the main pass and the injected pass so an injected
// grammar colours its content by exactly the same rules -- a capture cannot mean
// one thing at the top level and another inside an injection.
void applyCaptures(TSQuery const& query, TSNode root, std::uint32_t byteOffset,
                   std::vector<SyntaxScope>& perByte) {
    std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)> cursor(
        ts_query_cursor_new(), &ts_query_cursor_delete);
    if (!cursor) return;
    ts_query_cursor_exec(cursor.get(), &query, root);
    TSQueryMatch match{};
    std::uint32_t captureIndex = 0;
    while (ts_query_cursor_next_capture(cursor.get(), &match, &captureIndex)) {
        if (captureIndex >= match.capture_count) continue;
        auto const capture = match.captures[captureIndex];
        std::uint32_t nameLength = 0;
        char const* const captureName =
            ts_query_capture_name_for_id(&query, capture.index, &nameLength);
        if (captureName == nullptr || nameLength == 0) continue;
        auto const scope =
            scopeForCapture({captureName, static_cast<std::size_t>(nameLength)});
        if (scope == SyntaxScope::PlainText) continue;
        auto const start = ts_node_start_byte(capture.node) + byteOffset;
        auto const end = ts_node_end_byte(capture.node) + byteOffset;
        if (start >= end || start >= perByte.size()) continue;
        auto const boundedEnd =
            static_cast<std::uint32_t>(std::min<std::size_t>(end, perByte.size()));
        for (std::uint32_t offset = start; offset < boundedEnd; ++offset) {
            perByte[offset] = scope;
        }
    }
}

std::vector<SyntaxSpan> spansFromBytes(const std::vector<SyntaxScope>& perByte) {
    if (perByte.empty()) {
        return {};
    }

    std::vector<SyntaxSpan> spans;
    std::uint64_t begin = 0;
    auto scope = perByte.front();
    for (std::uint64_t index = 1; index < perByte.size(); ++index) {
        if (perByte[index] != scope) {
            spans.push_back(
                {ByteOffset{begin}, ByteOffset{index}, scope});
            begin = index;
            scope = perByte[index];
        }
    }
    spans.push_back(
        {ByteOffset{begin}, ByteOffset{perByte.size()}, scope});
    return spans;
}

} // namespace

bool TreeSitterParser::hasGrammar(const LanguageId& language) const {
    return grammarIndexFor(language).has_value();
}

SyntaxParseOutput TreeSitterParser::parse(const SyntaxParseRequest& request) {
    SyntaxParseOutput output;
    output.revision = request.revision();
    if (request.cancelled()) {
        output.status = SyntaxParseStatus::Cancelled;
        return output;
    }

    const auto grammarIndex = grammarIndexFor(request.language());
    if (!grammarIndex) {
        output.status = SyntaxParseStatus::GrammarUnavailable;
        return output;
    }
    const TreeSitterGrammar* grammar = &grammars_[*grammarIndex];

    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(
        ts_parser_new(), &ts_parser_delete);
    if (!parser ||
        !ts_parser_set_language(
            parser.get(),
            static_cast<const TSLanguage*>(grammar->language()))) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }
    if (request.text().size() >
        std::numeric_limits<std::uint32_t>::max()) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
        ts_parser_parse_string(parser.get(), nullptr, request.text().data(),
                               static_cast<std::uint32_t>(
                                   request.text().size())),
        &ts_tree_delete);
    if (!tree) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    const TSQuery* query = queryFor(*queries_, *grammar, *grammarIndex);
    if (query == nullptr) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    std::vector<SyntaxScope> perByte(request.text().size(),
                                     SyntaxScope::PlainText);
    std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)> cursor(
        ts_query_cursor_new(), &ts_query_cursor_delete);
    if (!cursor) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    ts_query_cursor_exec(cursor.get(), query, ts_tree_root_node(tree.get()));
    TSQueryMatch match{};
    std::uint32_t captureIndex = 0;
    // Later captures overwrite earlier captures so more-specific highlight rules
    // can refine broad matches from the same query run.
    while (ts_query_cursor_next_capture(cursor.get(), &match, &captureIndex)) {
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::Cancelled;
            return output;
        }
        if (captureIndex >= match.capture_count) {
            continue;
        }
        const auto capture = match.captures[captureIndex];
        std::uint32_t nameLength = 0;
        const char* captureName =
            ts_query_capture_name_for_id(query, capture.index, &nameLength);
        if (captureName == nullptr || nameLength == 0) {
            continue;
        }
        const auto scope =
            scopeForCapture({captureName, static_cast<std::size_t>(nameLength)});
        if (scope == SyntaxScope::PlainText) {
            continue;
        }

        const auto start = ts_node_start_byte(capture.node);
        const auto end = ts_node_end_byte(capture.node);
        if (start >= end || start >= perByte.size()) {
            continue;
        }
        const auto boundedEnd =
            static_cast<std::uint32_t>(std::min<std::size_t>(end, perByte.size()));
        for (std::uint32_t offset = start; offset < boundedEnd; ++offset) {
            perByte[offset] = scope;
        }
    }

    // Run the injected grammar inside each node that carries it, merging its
    // captures into the same per-byte map.  Markdown's block grammar leaves
    // inline content opaque, so without this pass emphasis, code spans and
    // inline links are unhighlighted in every markdown document.
    if (grammar->injection && grammar->injection->language != nullptr) {
        auto const& injection = *grammar->injection;
        std::unique_ptr<TSParser, decltype(&ts_parser_delete)> inner(
            ts_parser_new(), &ts_parser_delete);
        std::uint32_t queryError = 0;
        TSQueryError queryErrorType = TSQueryErrorNone;
        std::unique_ptr<TSQuery, decltype(&ts_query_delete)> innerQuery(
            ts_query_new(static_cast<const TSLanguage*>(injection.language()),
                         injection.highlightQuery.data(),
                         static_cast<std::uint32_t>(injection.highlightQuery.size()),
                         &queryError, &queryErrorType),
            &ts_query_delete);
        if (inner && innerQuery && queryErrorType == TSQueryErrorNone &&
            ts_parser_set_language(
                inner.get(),
                static_cast<const TSLanguage*>(injection.language()))) {
            // Walk the outer tree for the nodes to inject into.
            std::vector<TSNode> pending{ts_tree_root_node(tree.get())};
            while (!pending.empty()) {
                if (request.cancelled()) {
                    output.status = SyntaxParseStatus::Cancelled;
                    return output;
                }
                TSNode const node = pending.back();
                pending.pop_back();
                char const* const type = ts_node_type(node);
                if (type != nullptr && injection.nodeType == type) {
                    auto const start = ts_node_start_byte(node);
                    auto const end = ts_node_end_byte(node);
                    if (start < end && start < perByte.size()) {
                        auto const length = std::min<std::size_t>(
                            end - start, perByte.size() - start);
                        std::unique_ptr<TSTree, decltype(&ts_tree_delete)>
                            innerTree(
                                ts_parser_parse_string(
                                    inner.get(), nullptr,
                                    request.text().data() + start,
                                    static_cast<std::uint32_t>(length)),
                                &ts_tree_delete);
                        if (innerTree) {
                            applyCaptures(*innerQuery,
                                          ts_tree_root_node(innerTree.get()),
                                          start, perByte);
                        }
                    }
                    // Injected content is not searched again: the inner grammar
                    // owns everything inside it.
                    continue;
                }
                auto const children = ts_node_child_count(node);
                for (std::uint32_t index = 0; index < children; ++index) {
                    pending.push_back(ts_node_child(node, index));
                }
            }
        }
    }

    output.status = SyntaxParseStatus::Parsed;
    output.parse = std::make_shared<TreeSitterParse>();
    output.spans = spansFromBytes(perByte);
    return output;
}

} // namespace ssg
