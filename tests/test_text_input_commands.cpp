#include <ssg/text_input_commands.h>

#include "test_helpers.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using ssg::ByteOffset;
using ssg::Document;
using ssg::DocumentMode;
using ssg::DocumentPosition;
using ssg::IndentStyle;
using ssg::LineEnding;
using ssg::Selection;
using ssg::SelectionSet;
using ssg::TextInputCommand;
using ssg::TextInputError;
using ssg::TextInputSettings;

DocumentPosition position(std::string_view text, std::uint64_t offset,
                          int tab_width = 4) {
    const auto resolved =
        ssg::resolve_document_position(text, ByteOffset{offset}, tab_width);
    ASSERT_TRUE(resolved.has_value());
    return resolved.value();
}

SelectionSet selections(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges,
    int tab_width = 4) {
    std::vector<Selection> values;
    for (const auto [anchor, active] : ranges) {
        values.push_back(
            Selection{position(text, anchor, tab_width),
                      position(text, active, tab_width)});
    }
    return SelectionSet{std::move(values)};
}

TextInputSettings settings(
    LineEnding ending = LineEnding::lf,
    IndentStyle style = IndentStyle::spaces, std::uint32_t width = 4,
    bool auto_indent = false) {
    return TextInputSettings{style, width, auto_indent, ending};
}

std::vector<std::uint64_t> caret_offsets(const SelectionSet& value) {
    std::vector<std::uint64_t> offsets;
    for (const auto& selection : value.items()) {
        ASSERT_TRUE(selection.is_caret());
        offsets.push_back(selection.active.byte_offset.value());
    }
    return offsets;
}

std::string apply_result(Document& document,
                         const ssg::TextInputResult& result) {
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.selections.has_value());
    if (result.transaction.has_value()) {
        const auto applied = document.apply(*result.transaction);
        ASSERT_TRUE(applied.accepted());
    }
    ASSERT_EQ(document.snapshot().text, result.resulting_text);
    return document.snapshot().text;
}

TEST(command_set_is_exact_and_immutable) {
    static_assert(!std::is_copy_assignable_v<ssg::TextInputCommandSet>);
    constexpr std::array<std::string_view, 6> expected{{
        "text.insert",
        "text.newline",
        "text.delete_backward",
        "text.delete_forward",
        "text.delete_word_backward",
        "text.delete_word_forward",
    }};

    const auto commands = ssg::text_input_command_set();
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
    }
}

TEST(single_caret_insert_and_selection_replacement) {
    Document insert_document{"abcd"};
    auto inserted = ssg::apply_text_input(
        insert_document.snapshot(), selections("abcd", {{2, 2}}), settings(),
        TextInputCommand::insert, {.text = "XY"});
    ASSERT_EQ(apply_result(insert_document, inserted), std::string{"abXYcd"});
    ASSERT_EQ(caret_offsets(*inserted.selections),
              (std::vector<std::uint64_t>{4}));

    Document replace_document{"abcdef"};
    auto replaced = ssg::apply_text_input(
        replace_document.snapshot(), selections("abcdef", {{5, 2}}),
        settings(), TextInputCommand::insert, {.text = "Q"});
    ASSERT_EQ(apply_result(replace_document, replaced),
              std::string{"abQf"});
    ASSERT_EQ(caret_offsets(*replaced.selections),
              (std::vector<std::uint64_t>{3}));
}

TEST(multiple_carets_insert_once_each) {
    Document document{"abcd"};
    auto result = ssg::apply_text_input(
        document.snapshot(), selections("abcd", {{1, 1}, {3, 3}}),
        settings(), TextInputCommand::insert, {.text = "_"});

    ASSERT_EQ(apply_result(document, result), std::string{"a_bc_d"});
    ASSERT_EQ(caret_offsets(*result.selections),
              (std::vector<std::uint64_t>{2, 5}));
}

TEST(newline_uses_configured_eol_and_indentation) {
    Document crlf_document{"    alpha"};
    auto crlf = ssg::apply_text_input(
        crlf_document.snapshot(), selections("    alpha", {{9, 9}}),
        settings(LineEnding::crlf, IndentStyle::spaces, 4, true),
        TextInputCommand::newline);
    ASSERT_EQ(apply_result(crlf_document, crlf),
              std::string{"    alpha\r\n    "});
    ASSERT_EQ(caret_offsets(*crlf.selections),
              (std::vector<std::uint64_t>{15}));

    Document tabs_document{" \t value"};
    auto tabs = ssg::apply_text_input(
        tabs_document.snapshot(), selections(" \t value", {{8, 8}}),
        settings(LineEnding::lf, IndentStyle::tabs, 4, true),
        TextInputCommand::newline);
    ASSERT_EQ(apply_result(tabs_document, tabs),
              std::string{" \t value\n\t "});
}

TEST(mixed_eol_matches_current_line_then_falls_back_to_lf) {
    Document matched_document{"one\r\ntwo"};
    auto matched = ssg::apply_text_input(
        matched_document.snapshot(), selections("one\r\ntwo", {{1, 1}}),
        settings(LineEnding::mixed), TextInputCommand::newline);
    ASSERT_EQ(apply_result(matched_document, matched),
              std::string{"o\r\nne\r\ntwo"});

    Document fallback_document{"tail"};
    auto fallback = ssg::apply_text_input(
        fallback_document.snapshot(), selections("tail", {{4, 4}}),
        settings(LineEnding::mixed), TextInputCommand::newline);
    ASSERT_EQ(apply_result(fallback_document, fallback),
              std::string{"tail\n"});
}

TEST(character_deletion_uses_grapheme_clusters_and_crlf) {
    const std::string combining = "A" "e\xCC\x81" "B";
    Document backward_document{combining};
    auto backward = ssg::apply_text_input(
        backward_document.snapshot(), selections(combining, {{4, 4}}),
        settings(), TextInputCommand::delete_backward);
    ASSERT_EQ(apply_result(backward_document, backward), std::string{"AB"});
    ASSERT_EQ(caret_offsets(*backward.selections),
              (std::vector<std::uint64_t>{1}));

    const std::string family =
        "X\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7Y";
    Document forward_document{family};
    auto forward = ssg::apply_text_input(
        forward_document.snapshot(), selections(family, {{1, 1}}), settings(),
        TextInputCommand::delete_forward);
    ASSERT_EQ(apply_result(forward_document, forward), std::string{"XY"});

    Document crlf_document{"a\r\nb"};
    auto crlf = ssg::apply_text_input(
        crlf_document.snapshot(), selections("a\r\nb", {{3, 3}}), settings(),
        TextInputCommand::delete_backward);
    ASSERT_EQ(apply_result(crlf_document, crlf), std::string{"ab"});
}

TEST(word_deletion_and_selected_deletion) {
    Document backward_document{"alpha  beta!"};
    auto backward = ssg::apply_text_input(
        backward_document.snapshot(),
        selections("alpha  beta!", {{11, 11}}), settings(),
        TextInputCommand::delete_word_backward);
    ASSERT_EQ(apply_result(backward_document, backward),
              std::string{"alpha  !"});

    Document forward_document{"alpha  beta!"};
    auto forward = ssg::apply_text_input(
        forward_document.snapshot(), selections("alpha  beta!", {{5, 5}}),
        settings(), TextInputCommand::delete_word_forward);
    ASSERT_EQ(apply_result(forward_document, forward),
              std::string{"alphabeta!"});

    Document selected_document{"012345"};
    auto selected = ssg::apply_text_input(
        selected_document.snapshot(), selections("012345", {{1, 4}}),
        settings(), TextInputCommand::delete_forward);
    ASSERT_EQ(apply_result(selected_document, selected),
              std::string{"045"});
}

TEST(overlap_normalization_and_coincident_carets_emit_valid_edits) {
    Document overlap_document{"abcdefgh"};
    auto overlap = ssg::apply_text_input(
        overlap_document.snapshot(),
        selections("abcdefgh", {{1, 5}, {3, 7}}), settings(),
        TextInputCommand::insert, {.text = "X"});
    ASSERT_EQ(apply_result(overlap_document, overlap), std::string{"aXh"});
    ASSERT_EQ(overlap.transaction->edits.size(), 1u);

    const auto coincident = selections("abc", {{1, 1}, {1, 1}});
    ASSERT_EQ(coincident.items().size(), 1u);
    Document coincident_document{"abc"};
    auto one_insert = ssg::apply_text_input(
        coincident_document.snapshot(), coincident, settings(),
        TextInputCommand::insert, {.text = "X"});
    ASSERT_EQ(apply_result(coincident_document, one_insert),
              std::string{"aXbc"});
    ASSERT_EQ(one_insert.transaction->edits.size(), 1u);

    Document boundary_document{"abcdefgh"};
    auto boundary = ssg::apply_text_input(
        boundary_document.snapshot(),
        selections("abcdefgh", {{2, 2}, {2, 5}}), settings(),
        TextInputCommand::insert, {.text = "Q"});
    ASSERT_EQ(apply_result(boundary_document, boundary),
              std::string{"abQfgh"});
    ASSERT_EQ(boundary.transaction->edits.size(), 1u);
    ASSERT_EQ(caret_offsets(*boundary.selections),
              (std::vector<std::uint64_t>{3}));
}

TEST(boundary_deletion_is_successful_noop) {
    Document backward_document{"abc"};
    auto backward = ssg::apply_text_input(
        backward_document.snapshot(), selections("abc", {{0, 0}}), settings(),
        TextInputCommand::delete_backward);
    ASSERT_TRUE(backward.accepted());
    ASSERT_FALSE(backward.transaction.has_value());
    ASSERT_EQ(apply_result(backward_document, backward), std::string{"abc"});
    ASSERT_EQ(backward_document.revision(), ssg::Revision{1});

    Document forward_document{"abc"};
    auto forward = ssg::apply_text_input(
        forward_document.snapshot(), selections("abc", {{3, 3}}), settings(),
        TextInputCommand::delete_forward);
    ASSERT_TRUE(forward.accepted());
    ASSERT_FALSE(forward.transaction.has_value());
    ASSERT_EQ(apply_result(forward_document, forward), std::string{"abc"});

    Document mixed_document{"abc"};
    auto mixed = ssg::apply_text_input(
        mixed_document.snapshot(), selections("abc", {{0, 0}, {3, 3}}),
        settings(), TextInputCommand::delete_backward);
    ASSERT_EQ(apply_result(mixed_document, mixed), std::string{"ab"});
    ASSERT_EQ(caret_offsets(*mixed.selections),
              (std::vector<std::uint64_t>{0, 2}));
}

TEST(invalid_input_and_non_edit_modes_fail_atomically) {
    Document document{"abc"};
    auto invalid_utf8 = ssg::apply_text_input(
        document.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::insert, {.text = std::string{"\xFF", 1}});
    ASSERT_EQ(invalid_utf8.error, TextInputError::invalid_utf8);
    ASSERT_FALSE(invalid_utf8.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    auto bad_position = position("abc", 1);
    bad_position.line = ssg::LineIndex{7};
    auto invalid_selection = ssg::apply_text_input(
        document.snapshot(),
        SelectionSet{{Selection{bad_position, bad_position}}}, settings(),
        TextInputCommand::insert, {.text = "x"});
    ASSERT_EQ(invalid_selection.error, TextInputError::invalid_selection);
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    Document read_only{"abc", DocumentMode::read_only};
    auto rejected = ssg::apply_text_input(
        read_only.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::insert, {.text = "x"});
    ASSERT_EQ(rejected.error, TextInputError::read_only);
    ASSERT_EQ(read_only.snapshot().text, std::string{"abc"});
}

}  // namespace

int main() {
    RUN(command_set_is_exact_and_immutable);
    RUN(single_caret_insert_and_selection_replacement);
    RUN(multiple_carets_insert_once_each);
    RUN(newline_uses_configured_eol_and_indentation);
    RUN(mixed_eol_matches_current_line_then_falls_back_to_lf);
    RUN(character_deletion_uses_grapheme_clusters_and_crlf);
    RUN(word_deletion_and_selected_deletion);
    RUN(overlap_normalization_and_coincident_carets_emit_valid_edits);
    RUN(boundary_deletion_is_successful_noop);
    RUN(invalid_input_and_non_edit_modes_fail_atomically);
    return failed == 0 ? 0 : 1;
}
