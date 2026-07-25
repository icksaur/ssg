#include "TreeSitterParser.h"

#ifdef SSG_TREESITTER

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
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
const TSLanguage* tree_sitter_lua();
}

class TreeSitterParse final : public OpaqueSyntaxParse {};

struct GrammarSpec {
    std::array<std::string_view, 4> ids;
    const TSLanguage* (*language)();
    // Key into the generated embedded-query table, NOT a path: the queries are
    // compiled into the binary so a moved or absent source tree cannot silently
    // cost us highlighting.
    std::string_view queryKey;
    // Highlight query for the base grammar this one inherits (tree-sitter's
    // "; inherits:" directive, which ts_query_new does not process). Its rules
    // are prepended so this grammar's specific rules override them. Empty = none.
    std::string_view inheritsQueryKey;
};

const std::array kGrammars{
    GrammarSpec{{"c", "", "", ""}, tree_sitter_c, "c", ""},
    GrammarSpec{{"cpp", "c++", "cc", ""}, tree_sitter_cpp, "cpp", "c"},
    GrammarSpec{{"javascript", "js", "", ""}, tree_sitter_javascript,
                "javascript", ""},
    GrammarSpec{{"typescript", "ts", "", ""}, tree_sitter_typescript,
                "typescript", "javascript"},
    GrammarSpec{{"csharp", "c#", "cs", ""}, tree_sitter_c_sharp, "csharp", ""},
    GrammarSpec{{"lua", "", "", ""}, tree_sitter_lua, "lua", ""},
};

const GrammarSpec* grammarFor(const LanguageId& language) {
    const auto& id = language.value();
    for (const auto& grammar : kGrammars) {
        for (const auto alias : grammar.ids) {
            if (!alias.empty() && alias == id) {
                return &grammar;
            }
        }
    }
    return nullptr;
}

const TSQuery* queryFor(const GrammarSpec& grammar) {
    static std::mutex mutex;
    static std::unordered_map<
        std::string, std::unique_ptr<TSQuery, decltype(&ts_query_delete)>>
        queries;
    static std::unordered_set<std::string> failedQueries;

    std::lock_guard<std::mutex> lock{mutex};
    const std::string queryKey{grammar.queryKey};
    if (const auto found = queries.find(queryKey); found != queries.end()) {
        return found->second.get();
    }
    if (failedQueries.contains(queryKey)) {
        return nullptr;
    }

    const auto querySource = [&] {
        std::string source;
        if (!grammar.inheritsQueryKey.empty()) {
            source += embeddedHighlightQuery(grammar.inheritsQueryKey);
            source += '\n';
        }
        source += embeddedHighlightQuery(grammar.queryKey);
        return source;
    }();
    if (querySource.empty()) {
        failedQueries.insert(queryKey);
        return nullptr;
    }

    std::uint32_t errorOffset = 0;
    TSQueryError errorType = TSQueryErrorNone;
    auto query = std::unique_ptr<TSQuery, decltype(&ts_query_delete)>(
        ts_query_new(grammar.language(), querySource.data(), querySource.size(),
                     &errorOffset, &errorType),
        &ts_query_delete);
    (void)errorOffset;
    if (!query || errorType != TSQueryErrorNone) {
        failedQueries.insert(queryKey);
        return nullptr;
    }

    const auto inserted =
        queries.emplace(queryKey, std::move(query));
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
    return SyntaxScope::PlainText;
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
    return grammarFor(language) != nullptr;
}

SyntaxParseOutput TreeSitterParser::parse(const SyntaxParseRequest& request) {
    SyntaxParseOutput output;
    output.revision = request.revision();
    if (request.cancelled()) {
        output.status = SyntaxParseStatus::Cancelled;
        return output;
    }

    const auto* grammar = grammarFor(request.language());
    if (grammar == nullptr) {
        output.status = SyntaxParseStatus::GrammarUnavailable;
        return output;
    }

    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(
        ts_parser_new(), &ts_parser_delete);
    if (!parser || !ts_parser_set_language(parser.get(), grammar->language())) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
        ts_parser_parse_string(parser.get(), nullptr, request.text().data(),
                               request.text().size()),
        &ts_tree_delete);
    if (!tree) {
        output.status = SyntaxParseStatus::Failed;
        return output;
    }

    const TSQuery* query = queryFor(*grammar);
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

    output.status = SyntaxParseStatus::Parsed;
    output.parse = std::make_shared<TreeSitterParse>();
    output.spans = spansFromBytes(perByte);
    return output;
}

} // namespace ssg

#endif
