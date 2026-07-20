#include "ssg/syntax.h"
#include "test_helpers.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

constexpr ByteOffset byte(std::uint64_t value) {
    return ByteOffset{value};
}

constexpr LineIndex line(std::uint64_t value) {
    return LineIndex{value};
}

class FakeParse final : public OpaqueSyntaxParse {
public:
    explicit FakeParse(std::string text) : text_(std::move(text)) {}
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::string text_;
};

class DeterministicParser final : public SyntaxParser {
public:
    bool grammar_available = true;
    bool fail_parse = false;
    std::size_t parse_calls = 0;
    SyntaxParseHandle last_prior;
    std::vector<SyntaxEdit> last_edits;

    bool hasGrammar(const LanguageId&) const override {
        return grammar_available;
    }

    SyntaxParseOutput parse(const SyntaxParseRequest& request) override {
        ++parse_calls;
        last_prior = request.priorParse();
        last_edits = request.edits();

        SyntaxParseOutput output;
        output.revision = request.revision();
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::Cancelled;
            return output;
        }
        if (fail_parse) {
            output.status = SyntaxParseStatus::Failed;
            return output;
        }

        output.status = SyntaxParseStatus::Parsed;
        const auto parsed_text = applyIncrementalInput(request);
        if (!parsed_text || *parsed_text != request.text()) {
            output.status = SyntaxParseStatus::Failed;
            return output;
        }
        output.parse = std::make_shared<FakeParse>(*parsed_text);
        scan(*parsed_text, output);
        return output;
    }

private:
    static std::optional<std::string> applyIncrementalInput(
        const SyntaxParseRequest& request) {
        if (!request.priorParse()) {
            return request.text();
        }
        const auto prior =
            std::dynamic_pointer_cast<const FakeParse>(request.priorParse());
        if (!prior) {
            return std::nullopt;
        }

        auto result = prior->text();
        std::vector<std::string> inserted;
        inserted.reserve(request.edits().size());
        std::int64_t preceding_delta = 0;
        for (const auto& edit : request.edits()) {
            const auto final_start = static_cast<std::int64_t>(
                                         edit.start_byte.value()) +
                                     preceding_delta;
            const auto inserted_size =
                edit.new_end_byte.value() - edit.start_byte.value();
            if (final_start < 0 ||
                static_cast<std::uint64_t>(final_start) >
                    request.text().size() ||
                inserted_size >
                    request.text().size() -
                        static_cast<std::uint64_t>(final_start)) {
                return std::nullopt;
            }
            inserted.push_back(request.text().substr(
                static_cast<std::size_t>(final_start), inserted_size));
            preceding_delta += static_cast<std::int64_t>(inserted_size) -
                               static_cast<std::int64_t>(
                                   edit.old_end_byte.value() -
                                   edit.start_byte.value());
        }
        for (std::size_t index = request.edits().size(); index > 0; --index) {
            const auto& edit = request.edits()[index - 1];
            result.replace(
                edit.start_byte.value(),
                edit.old_end_byte.value() - edit.start_byte.value(),
                inserted[index - 1]);
        }
        return result;
    }

    static bool startsWith(std::string_view text, std::size_t at,
                            std::string_view token) {
        return at <= text.size() && text.substr(at, token.size()) == token;
    }

    static bool wordAt(std::string_view text, std::size_t at,
                        std::string_view word) {
        if (!startsWith(text, at, word)) {
            return false;
        }
        const auto word_character = [](char value) {
            return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
                   value == '_';
        };
        return (at == 0 || !word_character(text[at - 1])) &&
               (at + word.size() == text.size() ||
                !word_character(text[at + word.size()]));
    }

    static void scan(std::string_view text, SyntaxParseOutput& output) {
        std::size_t index = 0;
        while (index < text.size()) {
            if (startsWith(text, index, "//")) {
                const auto end = text.find('\n', index);
                const auto range_end =
                    end == std::string_view::npos ? text.size() : end;
                output.spans.push_back(
                    {byte(index), byte(range_end), SyntaxScope::Comment});
                output.comment_tokens.push_back(
                    {{byte(index), byte(index + 2)}, CommentTokenRole::Line});
                output.comment_ranges.push_back(
                    {{byte(index), byte(range_end)}, CommentKind::Line});
                index = range_end;
                continue;
            }
            if (startsWith(text, index, "/*")) {
                const auto close = text.find("*/", index + 2);
                const auto range_end =
                    close == std::string_view::npos ? text.size() : close + 2;
                output.spans.push_back(
                    {byte(index), byte(range_end), SyntaxScope::Comment});
                output.comment_tokens.push_back(
                    {{byte(index), byte(index + 2)},
                     CommentTokenRole::BlockOpen});
                if (close != std::string_view::npos) {
                    output.comment_tokens.push_back(
                        {{byte(close), byte(close + 2)},
                         CommentTokenRole::BlockClose});
                }
                output.comment_ranges.push_back(
                    {{byte(index), byte(range_end)}, CommentKind::Block});
                index = range_end;
                continue;
            }
            if (text[index] == '"') {
                auto end = index + 1;
                while (end < text.size() && text[end] != '"') {
                    ++end;
                }
                end += end < text.size() ? 1 : 0;
                output.spans.push_back(
                    {byte(index), byte(end), SyntaxScope::String});
                index = end;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
                auto end = index + 1;
                while (end < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
                    ++end;
                }
                output.spans.push_back(
                    {byte(index), byte(end), SyntaxScope::Number});
                index = end;
                continue;
            }
            if (wordAt(text, index, "fn")) {
                output.spans.push_back(
                    {byte(index), byte(index + 2), SyntaxScope::Keyword});
                index += 2;
                continue;
            }
            if (wordAt(text, index, "let")) {
                output.spans.push_back(
                    {byte(index), byte(index + 3), SyntaxScope::Keyword});
                index += 3;
                continue;
            }

            const auto bracket = [](char value)
                -> std::optional<std::pair<BracketKind, BracketRole>> {
                switch (value) {
                case '(':
                    return {{BracketKind::Round, BracketRole::Open}};
                case ')':
                    return {{BracketKind::Round, BracketRole::Close}};
                case '[':
                    return {{BracketKind::Square, BracketRole::Open}};
                case ']':
                    return {{BracketKind::Square, BracketRole::Close}};
                case '{':
                    return {{BracketKind::Curly, BracketRole::Open}};
                case '}':
                    return {{BracketKind::Curly, BracketRole::Close}};
                default:
                    return std::nullopt;
                }
            }(text[index]);
            if (bracket) {
                output.brackets.push_back(
                    {byte(index), bracket->first, bracket->second});
            }
            ++index;
        }
    }
};

SyntaxParseRequestResult requestFor(SyntaxModel& model, Revision revision,
                                     std::string text,
                                     std::vector<SyntaxEdit> edits = {}) {
    return model.request(revision, LanguageId{"toy"}, std::move(text),
                         std::move(edits));
}

SyntaxAcceptResult parseAndAccept(SyntaxModel& model,
                                    const SyntaxParseRequestResult& prepared) {
    ASSERT_TRUE(prepared.accepted());
    const auto output = model.run(*prepared.request);
    return model.accept(prepared.request, output);
}

TEST(handComputedMetadataGoldenCoversAllExportedSections) {
    const std::string text = "fn(a[1]) {\n\t// c\n  x] /* y */\n}\n";
    SyntaxParseOutput raw{
        .revision = Revision{7},
        .status = SyntaxParseStatus::Parsed,
        .parse = std::make_shared<FakeParse>(text),
        .spans =
            {
                {byte(22), byte(29), SyntaxScope::Comment},
                {byte(5), byte(6), SyntaxScope::Number},
                {byte(0), byte(2), SyntaxScope::Keyword},
                {byte(12), byte(16), SyntaxScope::Comment},
            },
        .brackets =
            {
                {byte(30), BracketKind::Curly, BracketRole::Close},
                {byte(2), BracketKind::Round, BracketRole::Open},
                {byte(20), BracketKind::Square, BracketRole::Close},
                {byte(4), BracketKind::Square, BracketRole::Open},
                {byte(6), BracketKind::Square, BracketRole::Close},
                {byte(7), BracketKind::Round, BracketRole::Close},
                {byte(9), BracketKind::Curly, BracketRole::Open},
            },
        .comment_tokens =
            {
                {{byte(27), byte(29)}, CommentTokenRole::BlockClose},
                {{byte(12), byte(14)}, CommentTokenRole::Line},
                {{byte(22), byte(24)}, CommentTokenRole::BlockOpen},
            },
        .comment_ranges =
            {
                {{byte(22), byte(29)}, CommentKind::Block},
                {{byte(12), byte(16)}, CommentKind::Line},
            },
    };

    const auto state = buildSyntaxViewState(
        Revision{7}, LanguageId{"toy"}, text, raw, SyntaxConfig{.tab_width = 4});

    ASSERT_EQ(state.revision(), Revision{7});
    ASSERT_EQ(state.language(), LanguageId{"toy"});
    ASSERT_EQ(state.textBytes(), std::uint64_t{32});
    ASSERT_EQ(
        state.spans(),
        (std::vector<SyntaxSpan>{
            {byte(0), byte(2), SyntaxScope::Keyword},
            {byte(2), byte(5), SyntaxScope::PlainText},
            {byte(5), byte(6), SyntaxScope::Number},
            {byte(6), byte(12), SyntaxScope::PlainText},
            {byte(12), byte(16), SyntaxScope::Comment},
            {byte(16), byte(22), SyntaxScope::PlainText},
            {byte(22), byte(29), SyntaxScope::Comment},
            {byte(29), byte(32), SyntaxScope::PlainText},
        }));
    ASSERT_EQ(
        state.bracketPairs(),
        (std::vector<SyntaxBracketPair>{
            {byte(2), byte(7), BracketKind::Round, 0},
            {byte(4), byte(6), BracketKind::Square, 1},
            {byte(9), byte(30), BracketKind::Curly, 0},
        }));
    ASSERT_EQ(
        state.unmatchedBrackets(),
        (std::vector<UnmatchedBracket>{
            {byte(20), BracketKind::Square, BracketRole::Close},
        }));
    ASSERT_EQ(
        state.commentTokens(),
        (std::vector<CommentToken>{
            {{byte(12), byte(14)}, CommentTokenRole::Line},
            {{byte(22), byte(24)}, CommentTokenRole::BlockOpen},
            {{byte(27), byte(29)}, CommentTokenRole::BlockClose},
        }));
    ASSERT_EQ(
        state.commentRanges(),
        (std::vector<CommentRange>{
            {{byte(12), byte(16)}, CommentKind::Line},
            {{byte(22), byte(29)}, CommentKind::Block},
        }));
    ASSERT_EQ(
        state.indentation(),
        (std::vector<LineIndentation>{
            {line(0), byte(0), byte(0), 0, 0, 0, false},
            {line(1), byte(11), byte(12), 0, 1, 4, false},
            {line(2), byte(17), byte(19), 2, 0, 2, false},
            {line(3), byte(30), byte(30), 0, 0, 0, false},
            {line(4), byte(32), byte(32), 0, 0, 0, true},
        }));
    ASSERT_EQ(matchingBracket(state, ByteOffset{4}),
              std::optional<ByteOffset>{ByteOffset{6}});
    ASSERT_EQ(matchingBracket(state, ByteOffset{20}),
              std::optional<ByteOffset>{});
    ASSERT_EQ(scopeAt(state, ByteOffset{5}), SyntaxScope::Number);
    ASSERT_EQ(scopeAt(state, ByteOffset{19}), SyntaxScope::PlainText);

    static_assert(
        std::is_const_v<std::remove_reference_t<decltype(state.spans())>>);
}

TEST(injectedParserReceivesPriorParseAndEditsAndMatchesFullParse) {
    auto incremental_parser = std::make_shared<DeterministicParser>();
    SyntaxModel incremental{incremental_parser};
    const std::string before = "fn main() {\n  let x = [1];\n}\n";
    const auto initial = requestFor(incremental, Revision{1}, before);
    ASSERT_TRUE(parseAndAccept(incremental, initial).accepted());
    const auto accepted_parse = incremental_parser->last_prior;
    ASSERT_FALSE(accepted_parse != nullptr);

    const auto number = before.find('1');
    std::string after = before;
    after.replace(number, 1, "42");
    const SyntaxEdit edit{
        .start_byte = ByteOffset{number},
        .old_end_byte = ByteOffset{number + 1},
        .new_end_byte = ByteOffset{number + 2},
        .start_position = {line(1), number - before.find('\n') - 1},
        .old_end_position = {line(1), number - before.find('\n')},
        .new_end_position = {line(1), number - before.find('\n') + 1},
    };

    const auto updated =
        requestFor(incremental, Revision{2}, after, {edit});
    ASSERT_TRUE(updated.accepted());
    ASSERT_TRUE(updated.request->priorParse() != nullptr);
    ASSERT_EQ(updated.request->edits(), (std::vector<SyntaxEdit>{edit}));
    ASSERT_TRUE(parseAndAccept(incremental, updated).accepted());
    ASSERT_TRUE(incremental_parser->last_prior != nullptr);
    ASSERT_EQ(incremental_parser->last_edits,
              (std::vector<SyntaxEdit>{edit}));

    auto full_parser = std::make_shared<DeterministicParser>();
    SyntaxModel full{full_parser};
    const auto full_request = requestFor(full, Revision{2}, after);
    ASSERT_TRUE(parseAndAccept(full, full_request).accepted());
    ASSERT_EQ(incremental.viewState(), full.viewState());
}

TEST(immutableDeltaDerivesOnlyChangedSectionsAndReplaysExactly) {
    const auto before = plainTextSyntaxViewState(
        Revision{4}, LanguageId{"toy"}, "alpha\n", 4);
    SyntaxParseOutput raw{
        .revision = Revision{5},
        .status = SyntaxParseStatus::Parsed,
        .parse = std::make_shared<FakeParse>("let 2\n"),
        .spans =
            {
                {byte(0), byte(3), SyntaxScope::Keyword},
                {byte(4), byte(5), SyntaxScope::Number},
            },
    };
    const auto after = buildSyntaxViewState(
        Revision{5}, LanguageId{"toy"}, "let 2\n", raw, {});

    const auto delta = deriveSyntaxDelta(before, after);
    ASSERT_EQ(delta.baseRevision(), Revision{4});
    ASSERT_EQ(delta.revision(), Revision{5});
    ASSERT_TRUE(delta.spans().has_value());
    ASSERT_FALSE(delta.bracketPairs().has_value());
    const auto replayed = replaySyntaxDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, after);

    const auto identical = deriveSyntaxDelta(after, after);
    ASSERT_TRUE(identical.empty());
    ASSERT_EQ(replaySyntaxDelta(after, identical).state,
              std::optional<SyntaxViewState>{after});

    const auto revision_only_target = plainTextSyntaxViewState(
        Revision{5}, LanguageId{"toy"}, "alpha\n", 4);
    const auto revision_only =
        deriveSyntaxDelta(before, revision_only_target);
    ASSERT_TRUE(revision_only.empty());
    const auto revision_only_replay =
        replaySyntaxDelta(before, revision_only);
    ASSERT_TRUE(revision_only_replay.accepted());
    ASSERT_EQ(*revision_only_replay.state, revision_only_target);

    const auto stale_base = plainTextSyntaxViewState(
        Revision{3}, LanguageId{"toy"}, "alpha\n", 4);
    ASSERT_EQ(replaySyntaxDelta(stale_base, delta).error,
              SyntaxReplayError::StaleRevision);

    const SyntaxDelta malformed{
        Revision{4},
        Revision{5},
        std::nullopt,
        std::uint64_t{6},
        std::vector<SyntaxSpan>{
            {byte(0), byte(4), SyntaxScope::PlainText},
            {byte(3), byte(6), SyntaxScope::Keyword},
        },
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    ASSERT_EQ(replaySyntaxDelta(before, malformed).error,
              SyntaxReplayError::MalformedDelta);

    const SyntaxDelta same_revision_change{
        Revision{4},
        Revision{4},
        std::nullopt,
        std::nullopt,
        std::vector<SyntaxSpan>{
            {byte(0), byte(6), SyntaxScope::Keyword},
        },
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    ASSERT_EQ(replaySyntaxDelta(before, same_revision_change).error,
              SyntaxReplayError::MalformedDelta);
}

TEST(supersededAndCancelledResultsNeverReplaceNewerState) {
    auto parser = std::make_shared<DeterministicParser>();
    SyntaxModel model{parser};
    const auto first = requestFor(model, Revision{1}, "let a = 1;\n");
    ASSERT_TRUE(parseAndAccept(model, first).accepted());
    const auto accepted = model.viewState();

    const auto second = requestFor(model, Revision{2}, "let a = 2;\n");
    ASSERT_TRUE(second.accepted());
    const auto completed_second = model.run(*second.request);
    const auto third = requestFor(model, Revision{3}, "let a = 3;\n");
    ASSERT_TRUE(third.accepted());
    ASSERT_TRUE(second.request->cancelled());

    ASSERT_EQ(model.accept(second.request, completed_second).error,
              SyntaxAcceptError::Cancelled);
    ASSERT_EQ(model.viewState(), accepted);

    model.cancelPending();
    ASSERT_TRUE(third.request->cancelled());
    ASSERT_EQ(model.accept(third.request, model.run(*third.request)).error,
              SyntaxAcceptError::Cancelled);
    ASSERT_EQ(model.viewState(), accepted);

    const auto newest = requestFor(model, Revision{4}, "let a = 4;\n");
    ASSERT_TRUE(parseAndAccept(model, newest).accepted());
    ASSERT_EQ(model.viewState().revision(), Revision{4});
    ASSERT_EQ(model.accept(first.request, model.run(*first.request)).error,
              SyntaxAcceptError::StaleRevision);
}

TEST(noParserUnavailableGrammarAndFailedParseShareFallbackSnapshot) {
    const std::string text = "\tplain\n";

    SyntaxModel no_parser;
    const auto absent = requestFor(no_parser, Revision{1}, text);
    const auto absent_result = parseAndAccept(no_parser, absent);
    ASSERT_TRUE(absent_result.accepted());
    ASSERT_TRUE(absent_result.used_fallback);

    auto unavailable_parser = std::make_shared<DeterministicParser>();
    unavailable_parser->grammar_available = false;
    SyntaxModel unavailable{unavailable_parser};
    const auto unavailable_request =
        requestFor(unavailable, Revision{1}, text);
    const auto unavailable_result =
        parseAndAccept(unavailable, unavailable_request);
    ASSERT_TRUE(unavailable_result.accepted());
    ASSERT_TRUE(unavailable_result.used_fallback);
    ASSERT_EQ(unavailable_parser->parse_calls, std::size_t{0});

    auto failed_parser = std::make_shared<DeterministicParser>();
    failed_parser->fail_parse = true;
    SyntaxModel failed_model{failed_parser};
    const auto failed_request = requestFor(failed_model, Revision{1}, text);
    const auto failed_result =
        parseAndAccept(failed_model, failed_request);
    ASSERT_TRUE(failed_result.accepted());
    ASSERT_TRUE(failed_result.used_fallback);
    ASSERT_EQ(failed_parser->parse_calls, std::size_t{1});

    auto plain_parser = std::make_shared<DeterministicParser>();
    SyntaxModel explicit_plain{plain_parser};
    const auto plain_request = explicit_plain.request(
        Revision{1}, LanguageId::plainText(), text);
    ASSERT_TRUE(parseAndAccept(explicit_plain, plain_request).accepted());
    ASSERT_EQ(plain_parser->parse_calls, std::size_t{0});

    ASSERT_EQ(no_parser.viewState(), unavailable.viewState());
    ASSERT_EQ(unavailable.viewState(), failed_model.viewState());
    ASSERT_EQ(no_parser.viewState().spans(),
              (std::vector<SyntaxSpan>{
                  {byte(0), byte(text.size()), SyntaxScope::PlainText},
              }));
    ASSERT_TRUE(no_parser.viewState().bracketPairs().empty());
    ASSERT_TRUE(no_parser.viewState().commentRanges().empty());
    ASSERT_EQ(no_parser.viewState().indentation().at(0).columns,
              std::uint32_t{4});
}

TEST(requestAndResultValidationIsFailureAtomic) {
    auto parser = std::make_shared<DeterministicParser>();
    SyntaxModel model{parser, SyntaxConfig{.maximum_document_bytes = 8}};

    const auto oversized =
        requestFor(model, Revision{1}, "123456789");
    ASSERT_EQ(oversized.error, SyntaxRequestError::DocumentTooLarge);
    ASSERT_EQ(model.viewState().revision(), Revision{0});

    const SyntaxEdit malformed{
        .start_byte = ByteOffset{4},
        .old_end_byte = ByteOffset{3},
        .new_end_byte = ByteOffset{4},
        .start_position = {line(0), 4},
        .old_end_position = {line(0), 3},
        .new_end_position = {line(0), 4},
    };
    const auto malformed_request =
        requestFor(model, Revision{1}, "abc", {malformed});
    ASSERT_EQ(malformed_request.error, SyntaxRequestError::MalformedEdits);
    ASSERT_EQ(model.viewState().revision(), Revision{0});

    const auto valid = requestFor(model, Revision{1}, "let 1\n");
    ASSERT_TRUE(valid.accepted());
    auto mismatched = model.run(*valid.request);
    mismatched.revision = Revision{2};
    ASSERT_EQ(model.accept(valid.request, mismatched).error,
              SyntaxAcceptError::MalformedOutput);
    ASSERT_EQ(model.viewState().revision(), Revision{0});

    ASSERT_TRUE(parseAndAccept(model, valid).accepted());
    const auto stale = requestFor(model, Revision{1}, "let 2\n");
    ASSERT_EQ(stale.error, SyntaxRequestError::StaleRevision);
    ASSERT_EQ(model.viewState().revision(), Revision{1});

    const auto switched_language = model.request(
        Revision{2}, LanguageId{"other"}, "let 1\n");
    ASSERT_TRUE(switched_language.accepted());
    ASSERT_TRUE(switched_language.request->priorParse() == nullptr);
    const auto completed_switch = model.run(*switched_language.request);

    const auto rejected_newer =
        requestFor(model, Revision{3}, "123456789");
    ASSERT_EQ(rejected_newer.error, SyntaxRequestError::DocumentTooLarge);
    ASSERT_TRUE(switched_language.request->cancelled());
    ASSERT_EQ(model.accept(switched_language.request, completed_switch).error,
              SyntaxAcceptError::Cancelled);
    ASSERT_EQ(model.viewState().revision(), Revision{1});
}

} // namespace

int main() {
    RUN(handComputedMetadataGoldenCoversAllExportedSections);
    RUN(injectedParserReceivesPriorParseAndEditsAndMatchesFullParse);
    RUN(immutableDeltaDerivesOnlyChangedSectionsAndReplaysExactly);
    RUN(supersededAndCancelledResultsNeverReplaceNewerState);
    RUN(noParserUnavailableGrammarAndFailedParseShareFallbackSnapshot);
    RUN(requestAndResultValidationIsFailureAtomic);
    return failed == 0 ? 0 : 1;
}
