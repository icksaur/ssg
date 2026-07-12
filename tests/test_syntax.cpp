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

    bool has_grammar(const LanguageId&) const override {
        return grammar_available;
    }

    SyntaxParseOutput parse(const SyntaxParseRequest& request) override {
        ++parse_calls;
        last_prior = request.prior_parse();
        last_edits = request.edits();

        SyntaxParseOutput output;
        output.revision = request.revision();
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::cancelled;
            return output;
        }
        if (fail_parse) {
            output.status = SyntaxParseStatus::failed;
            return output;
        }

        output.status = SyntaxParseStatus::parsed;
        const auto parsed_text = apply_incremental_input(request);
        if (!parsed_text || *parsed_text != request.text()) {
            output.status = SyntaxParseStatus::failed;
            return output;
        }
        output.parse = std::make_shared<FakeParse>(*parsed_text);
        scan(*parsed_text, output);
        return output;
    }

private:
    static std::optional<std::string> apply_incremental_input(
        const SyntaxParseRequest& request) {
        if (!request.prior_parse()) {
            return request.text();
        }
        const auto prior =
            std::dynamic_pointer_cast<const FakeParse>(request.prior_parse());
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

    static bool starts_with(std::string_view text, std::size_t at,
                            std::string_view token) {
        return at <= text.size() && text.substr(at, token.size()) == token;
    }

    static bool word_at(std::string_view text, std::size_t at,
                        std::string_view word) {
        if (!starts_with(text, at, word)) {
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
            if (starts_with(text, index, "//")) {
                const auto end = text.find('\n', index);
                const auto range_end =
                    end == std::string_view::npos ? text.size() : end;
                output.spans.push_back(
                    {byte(index), byte(range_end), SyntaxScope::comment});
                output.comment_tokens.push_back(
                    {{byte(index), byte(index + 2)}, CommentTokenRole::line});
                output.comment_ranges.push_back(
                    {{byte(index), byte(range_end)}, CommentKind::line});
                index = range_end;
                continue;
            }
            if (starts_with(text, index, "/*")) {
                const auto close = text.find("*/", index + 2);
                const auto range_end =
                    close == std::string_view::npos ? text.size() : close + 2;
                output.spans.push_back(
                    {byte(index), byte(range_end), SyntaxScope::comment});
                output.comment_tokens.push_back(
                    {{byte(index), byte(index + 2)},
                     CommentTokenRole::block_open});
                if (close != std::string_view::npos) {
                    output.comment_tokens.push_back(
                        {{byte(close), byte(close + 2)},
                         CommentTokenRole::block_close});
                }
                output.comment_ranges.push_back(
                    {{byte(index), byte(range_end)}, CommentKind::block});
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
                    {byte(index), byte(end), SyntaxScope::string});
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
                    {byte(index), byte(end), SyntaxScope::number});
                index = end;
                continue;
            }
            if (word_at(text, index, "fn")) {
                output.spans.push_back(
                    {byte(index), byte(index + 2), SyntaxScope::keyword});
                index += 2;
                continue;
            }
            if (word_at(text, index, "let")) {
                output.spans.push_back(
                    {byte(index), byte(index + 3), SyntaxScope::keyword});
                index += 3;
                continue;
            }

            const auto bracket = [](char value)
                -> std::optional<std::pair<BracketKind, BracketRole>> {
                switch (value) {
                case '(':
                    return {{BracketKind::round, BracketRole::open}};
                case ')':
                    return {{BracketKind::round, BracketRole::close}};
                case '[':
                    return {{BracketKind::square, BracketRole::open}};
                case ']':
                    return {{BracketKind::square, BracketRole::close}};
                case '{':
                    return {{BracketKind::curly, BracketRole::open}};
                case '}':
                    return {{BracketKind::curly, BracketRole::close}};
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

SyntaxParseRequestResult request_for(SyntaxModel& model, Revision revision,
                                     std::string text,
                                     std::vector<SyntaxEdit> edits = {}) {
    return model.request(revision, LanguageId{"toy"}, std::move(text),
                         std::move(edits));
}

SyntaxAcceptResult parse_and_accept(SyntaxModel& model,
                                    const SyntaxParseRequestResult& prepared) {
    ASSERT_TRUE(prepared.accepted());
    const auto output = model.run(*prepared.request);
    return model.accept(prepared.request, output);
}

TEST(hand_computed_metadata_golden_covers_all_exported_sections) {
    const std::string text = "fn(a[1]) {\n\t// c\n  x] /* y */\n}\n";
    SyntaxParseOutput raw{
        .revision = Revision{7},
        .status = SyntaxParseStatus::parsed,
        .parse = std::make_shared<FakeParse>(text),
        .spans =
            {
                {byte(22), byte(29), SyntaxScope::comment},
                {byte(5), byte(6), SyntaxScope::number},
                {byte(0), byte(2), SyntaxScope::keyword},
                {byte(12), byte(16), SyntaxScope::comment},
            },
        .brackets =
            {
                {byte(30), BracketKind::curly, BracketRole::close},
                {byte(2), BracketKind::round, BracketRole::open},
                {byte(20), BracketKind::square, BracketRole::close},
                {byte(4), BracketKind::square, BracketRole::open},
                {byte(6), BracketKind::square, BracketRole::close},
                {byte(7), BracketKind::round, BracketRole::close},
                {byte(9), BracketKind::curly, BracketRole::open},
            },
        .comment_tokens =
            {
                {{byte(27), byte(29)}, CommentTokenRole::block_close},
                {{byte(12), byte(14)}, CommentTokenRole::line},
                {{byte(22), byte(24)}, CommentTokenRole::block_open},
            },
        .comment_ranges =
            {
                {{byte(22), byte(29)}, CommentKind::block},
                {{byte(12), byte(16)}, CommentKind::line},
            },
    };

    const auto state = build_syntax_view_state(
        Revision{7}, LanguageId{"toy"}, text, raw, SyntaxConfig{.tab_width = 4});

    ASSERT_EQ(state.revision(), Revision{7});
    ASSERT_EQ(state.language(), LanguageId{"toy"});
    ASSERT_EQ(state.text_bytes(), std::uint64_t{32});
    ASSERT_EQ(
        state.spans(),
        (std::vector<SyntaxSpan>{
            {byte(0), byte(2), SyntaxScope::keyword},
            {byte(2), byte(5), SyntaxScope::plain_text},
            {byte(5), byte(6), SyntaxScope::number},
            {byte(6), byte(12), SyntaxScope::plain_text},
            {byte(12), byte(16), SyntaxScope::comment},
            {byte(16), byte(22), SyntaxScope::plain_text},
            {byte(22), byte(29), SyntaxScope::comment},
            {byte(29), byte(32), SyntaxScope::plain_text},
        }));
    ASSERT_EQ(
        state.bracket_pairs(),
        (std::vector<BracketPair>{
            {byte(2), byte(7), BracketKind::round, 0},
            {byte(4), byte(6), BracketKind::square, 1},
            {byte(9), byte(30), BracketKind::curly, 0},
        }));
    ASSERT_EQ(
        state.unmatched_brackets(),
        (std::vector<UnmatchedBracket>{
            {byte(20), BracketKind::square, BracketRole::close},
        }));
    ASSERT_EQ(
        state.comment_tokens(),
        (std::vector<CommentToken>{
            {{byte(12), byte(14)}, CommentTokenRole::line},
            {{byte(22), byte(24)}, CommentTokenRole::block_open},
            {{byte(27), byte(29)}, CommentTokenRole::block_close},
        }));
    ASSERT_EQ(
        state.comment_ranges(),
        (std::vector<CommentRange>{
            {{byte(12), byte(16)}, CommentKind::line},
            {{byte(22), byte(29)}, CommentKind::block},
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
    ASSERT_EQ(matching_bracket(state, ByteOffset{4}),
              std::optional<ByteOffset>{ByteOffset{6}});
    ASSERT_EQ(matching_bracket(state, ByteOffset{20}),
              std::optional<ByteOffset>{});
    ASSERT_EQ(scope_at(state, ByteOffset{5}), SyntaxScope::number);
    ASSERT_EQ(scope_at(state, ByteOffset{19}), SyntaxScope::plain_text);

    static_assert(
        std::is_const_v<std::remove_reference_t<decltype(state.spans())>>);
}

TEST(injected_parser_receives_prior_parse_and_edits_and_matches_full_parse) {
    auto incremental_parser = std::make_shared<DeterministicParser>();
    SyntaxModel incremental{incremental_parser};
    const std::string before = "fn main() {\n  let x = [1];\n}\n";
    const auto initial = request_for(incremental, Revision{1}, before);
    ASSERT_TRUE(parse_and_accept(incremental, initial).accepted());
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
        request_for(incremental, Revision{2}, after, {edit});
    ASSERT_TRUE(updated.accepted());
    ASSERT_TRUE(updated.request->prior_parse() != nullptr);
    ASSERT_EQ(updated.request->edits(), (std::vector<SyntaxEdit>{edit}));
    ASSERT_TRUE(parse_and_accept(incremental, updated).accepted());
    ASSERT_TRUE(incremental_parser->last_prior != nullptr);
    ASSERT_EQ(incremental_parser->last_edits,
              (std::vector<SyntaxEdit>{edit}));

    auto full_parser = std::make_shared<DeterministicParser>();
    SyntaxModel full{full_parser};
    const auto full_request = request_for(full, Revision{2}, after);
    ASSERT_TRUE(parse_and_accept(full, full_request).accepted());
    ASSERT_EQ(incremental.view_state(), full.view_state());
}

TEST(immutable_delta_derives_only_changed_sections_and_replays_exactly) {
    const auto before = plain_text_syntax_view_state(
        Revision{4}, LanguageId{"toy"}, "alpha\n", 4);
    SyntaxParseOutput raw{
        .revision = Revision{5},
        .status = SyntaxParseStatus::parsed,
        .parse = std::make_shared<FakeParse>("let 2\n"),
        .spans =
            {
                {byte(0), byte(3), SyntaxScope::keyword},
                {byte(4), byte(5), SyntaxScope::number},
            },
    };
    const auto after = build_syntax_view_state(
        Revision{5}, LanguageId{"toy"}, "let 2\n", raw, {});

    const auto delta = derive_syntax_delta(before, after);
    ASSERT_EQ(delta.base_revision(), Revision{4});
    ASSERT_EQ(delta.revision(), Revision{5});
    ASSERT_TRUE(delta.spans().has_value());
    ASSERT_FALSE(delta.bracket_pairs().has_value());
    const auto replayed = replay_syntax_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, after);

    const auto identical = derive_syntax_delta(after, after);
    ASSERT_TRUE(identical.empty());
    ASSERT_EQ(replay_syntax_delta(after, identical).state,
              std::optional<SyntaxViewState>{after});

    const auto revision_only_target = plain_text_syntax_view_state(
        Revision{5}, LanguageId{"toy"}, "alpha\n", 4);
    const auto revision_only =
        derive_syntax_delta(before, revision_only_target);
    ASSERT_TRUE(revision_only.empty());
    const auto revision_only_replay =
        replay_syntax_delta(before, revision_only);
    ASSERT_TRUE(revision_only_replay.accepted());
    ASSERT_EQ(*revision_only_replay.state, revision_only_target);

    const auto stale_base = plain_text_syntax_view_state(
        Revision{3}, LanguageId{"toy"}, "alpha\n", 4);
    ASSERT_EQ(replay_syntax_delta(stale_base, delta).error,
              SyntaxReplayError::stale_revision);

    const SyntaxDelta malformed{
        Revision{4},
        Revision{5},
        std::nullopt,
        std::uint64_t{6},
        std::vector<SyntaxSpan>{
            {byte(0), byte(4), SyntaxScope::plain_text},
            {byte(3), byte(6), SyntaxScope::keyword},
        },
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    ASSERT_EQ(replay_syntax_delta(before, malformed).error,
              SyntaxReplayError::malformed_delta);

    const SyntaxDelta same_revision_change{
        Revision{4},
        Revision{4},
        std::nullopt,
        std::nullopt,
        std::vector<SyntaxSpan>{
            {byte(0), byte(6), SyntaxScope::keyword},
        },
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    ASSERT_EQ(replay_syntax_delta(before, same_revision_change).error,
              SyntaxReplayError::malformed_delta);
}

TEST(superseded_and_cancelled_results_never_replace_newer_state) {
    auto parser = std::make_shared<DeterministicParser>();
    SyntaxModel model{parser};
    const auto first = request_for(model, Revision{1}, "let a = 1;\n");
    ASSERT_TRUE(parse_and_accept(model, first).accepted());
    const auto accepted = model.view_state();

    const auto second = request_for(model, Revision{2}, "let a = 2;\n");
    ASSERT_TRUE(second.accepted());
    const auto completed_second = model.run(*second.request);
    const auto third = request_for(model, Revision{3}, "let a = 3;\n");
    ASSERT_TRUE(third.accepted());
    ASSERT_TRUE(second.request->cancelled());

    ASSERT_EQ(model.accept(second.request, completed_second).error,
              SyntaxAcceptError::cancelled);
    ASSERT_EQ(model.view_state(), accepted);

    model.cancel_pending();
    ASSERT_TRUE(third.request->cancelled());
    ASSERT_EQ(model.accept(third.request, model.run(*third.request)).error,
              SyntaxAcceptError::cancelled);
    ASSERT_EQ(model.view_state(), accepted);

    const auto newest = request_for(model, Revision{4}, "let a = 4;\n");
    ASSERT_TRUE(parse_and_accept(model, newest).accepted());
    ASSERT_EQ(model.view_state().revision(), Revision{4});
    ASSERT_EQ(model.accept(first.request, model.run(*first.request)).error,
              SyntaxAcceptError::stale_revision);
}

TEST(no_parser_unavailable_grammar_and_failed_parse_share_fallback_snapshot) {
    const std::string text = "\tplain\n";

    SyntaxModel no_parser;
    const auto absent = request_for(no_parser, Revision{1}, text);
    const auto absent_result = parse_and_accept(no_parser, absent);
    ASSERT_TRUE(absent_result.accepted());
    ASSERT_TRUE(absent_result.used_fallback);

    auto unavailable_parser = std::make_shared<DeterministicParser>();
    unavailable_parser->grammar_available = false;
    SyntaxModel unavailable{unavailable_parser};
    const auto unavailable_request =
        request_for(unavailable, Revision{1}, text);
    const auto unavailable_result =
        parse_and_accept(unavailable, unavailable_request);
    ASSERT_TRUE(unavailable_result.accepted());
    ASSERT_TRUE(unavailable_result.used_fallback);
    ASSERT_EQ(unavailable_parser->parse_calls, std::size_t{0});

    auto failed_parser = std::make_shared<DeterministicParser>();
    failed_parser->fail_parse = true;
    SyntaxModel failed_model{failed_parser};
    const auto failed_request = request_for(failed_model, Revision{1}, text);
    const auto failed_result =
        parse_and_accept(failed_model, failed_request);
    ASSERT_TRUE(failed_result.accepted());
    ASSERT_TRUE(failed_result.used_fallback);
    ASSERT_EQ(failed_parser->parse_calls, std::size_t{1});

    auto plain_parser = std::make_shared<DeterministicParser>();
    SyntaxModel explicit_plain{plain_parser};
    const auto plain_request = explicit_plain.request(
        Revision{1}, LanguageId::plain_text(), text);
    ASSERT_TRUE(parse_and_accept(explicit_plain, plain_request).accepted());
    ASSERT_EQ(plain_parser->parse_calls, std::size_t{0});

    ASSERT_EQ(no_parser.view_state(), unavailable.view_state());
    ASSERT_EQ(unavailable.view_state(), failed_model.view_state());
    ASSERT_EQ(no_parser.view_state().spans(),
              (std::vector<SyntaxSpan>{
                  {byte(0), byte(text.size()), SyntaxScope::plain_text},
              }));
    ASSERT_TRUE(no_parser.view_state().bracket_pairs().empty());
    ASSERT_TRUE(no_parser.view_state().comment_ranges().empty());
    ASSERT_EQ(no_parser.view_state().indentation().at(0).columns,
              std::uint32_t{4});
}

TEST(request_and_result_validation_is_failure_atomic) {
    auto parser = std::make_shared<DeterministicParser>();
    SyntaxModel model{parser, SyntaxConfig{.maximum_document_bytes = 8}};

    const auto oversized =
        request_for(model, Revision{1}, "123456789");
    ASSERT_EQ(oversized.error, SyntaxRequestError::document_too_large);
    ASSERT_EQ(model.view_state().revision(), Revision{0});

    const SyntaxEdit malformed{
        .start_byte = ByteOffset{4},
        .old_end_byte = ByteOffset{3},
        .new_end_byte = ByteOffset{4},
        .start_position = {line(0), 4},
        .old_end_position = {line(0), 3},
        .new_end_position = {line(0), 4},
    };
    const auto malformed_request =
        request_for(model, Revision{1}, "abc", {malformed});
    ASSERT_EQ(malformed_request.error, SyntaxRequestError::malformed_edits);
    ASSERT_EQ(model.view_state().revision(), Revision{0});

    const auto valid = request_for(model, Revision{1}, "let 1\n");
    ASSERT_TRUE(valid.accepted());
    auto mismatched = model.run(*valid.request);
    mismatched.revision = Revision{2};
    ASSERT_EQ(model.accept(valid.request, mismatched).error,
              SyntaxAcceptError::malformed_output);
    ASSERT_EQ(model.view_state().revision(), Revision{0});

    ASSERT_TRUE(parse_and_accept(model, valid).accepted());
    const auto stale = request_for(model, Revision{1}, "let 2\n");
    ASSERT_EQ(stale.error, SyntaxRequestError::stale_revision);
    ASSERT_EQ(model.view_state().revision(), Revision{1});

    const auto switched_language = model.request(
        Revision{2}, LanguageId{"other"}, "let 1\n");
    ASSERT_TRUE(switched_language.accepted());
    ASSERT_TRUE(switched_language.request->prior_parse() == nullptr);
    const auto completed_switch = model.run(*switched_language.request);

    const auto rejected_newer =
        request_for(model, Revision{3}, "123456789");
    ASSERT_EQ(rejected_newer.error, SyntaxRequestError::document_too_large);
    ASSERT_TRUE(switched_language.request->cancelled());
    ASSERT_EQ(model.accept(switched_language.request, completed_switch).error,
              SyntaxAcceptError::cancelled);
    ASSERT_EQ(model.view_state().revision(), Revision{1});
}

} // namespace

int main() {
    RUN(hand_computed_metadata_golden_covers_all_exported_sections);
    RUN(injected_parser_receives_prior_parse_and_edits_and_matches_full_parse);
    RUN(immutable_delta_derives_only_changed_sections_and_replays_exactly);
    RUN(superseded_and_cancelled_results_never_replace_newer_state);
    RUN(no_parser_unavailable_grammar_and_failed_parse_share_fallback_snapshot);
    RUN(request_and_result_validation_is_failure_atomic);
    return failed == 0 ? 0 : 1;
}
