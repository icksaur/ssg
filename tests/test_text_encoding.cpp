#include "ssg/text_encoding.h"

#include "test_helpers.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ssg::FinalNewlinePolicy;
using ssg::LineEnding;
using ssg::LineTerminator;
using ssg::TextEncoding;

std::vector<std::uint8_t> fixture(std::string_view name) {
    const auto path = std::filesystem::path{SSG_ENCODING_FIXTURE_DIR} /
                      (std::string{name} + ".hex");
    std::ifstream input{path};
    std::vector<std::uint8_t> bytes;
    std::string token;
    while (input >> token) {
        bytes.push_back(static_cast<std::uint8_t>(
            std::stoul(token, nullptr, 16)));
    }
    return bytes;
}

std::string bytes(std::initializer_list<std::uint8_t> values) {
    return {reinterpret_cast<const char*>(values.begin()), values.size()};
}

TEST(auto_detects_utf8_and_preserves_lf_and_final_newline) {
    const auto original = fixture("utf8-lf");
    const auto decoded = ssg::decode_text(original);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.text->utf8, bytes({
        0x61, 0x6c, 0x70, 0x68, 0x61, 0x0a, 0xce, 0xb2, 0x0a}));
    ASSERT_EQ(decoded.text->status.encoding, TextEncoding::utf8);
    ASSERT_FALSE(decoded.text->status.had_bom);
    ASSERT_EQ(decoded.text->status.line_ending, LineEnding::lf);
    ASSERT_TRUE(decoded.text->status.final_newline);
    ASSERT_EQ(decoded.text->line_terminators,
              (std::vector{LineTerminator::lf, LineTerminator::lf}));
    ASSERT_EQ(ssg::encode_text(*decoded.text).bytes, original);
}

TEST(auto_detects_utf8_bom_and_preserves_crlf_without_final_newline) {
    const auto original = fixture("utf8-bom-crlf");
    const auto decoded = ssg::decode_text(original);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.text->utf8, bytes({
        0x63, 0x61, 0x66, 0xc3, 0xa9, 0x0a,
        0x6c, 0x61, 0x73, 0x74}));
    ASSERT_EQ(decoded.text->status.encoding, TextEncoding::utf8_bom);
    ASSERT_TRUE(decoded.text->status.had_bom);
    ASSERT_EQ(decoded.text->status.line_ending, LineEnding::crlf);
    ASSERT_FALSE(decoded.text->status.final_newline);
    ASSERT_EQ(decoded.text->line_terminators,
              (std::vector{LineTerminator::crlf, LineTerminator::none}));
    ASSERT_EQ(ssg::encode_text(*decoded.text).bytes, original);
}

TEST(round_trips_utf16_endianness_bom_and_mixed_endings) {
    const auto little_bytes = fixture("utf16le-mixed");
    const auto little = ssg::decode_text(little_bytes);
    ASSERT_TRUE(little.accepted());
    ASSERT_EQ(little.text->utf8, bytes({
        0x41, 0x0a, 0xe2, 0x82, 0xac, 0x0a, 0x5a, 0x0a}));
    ASSERT_EQ(little.text->status.encoding, TextEncoding::utf16le);
    ASSERT_EQ(little.text->status.line_ending, LineEnding::mixed);
    ASSERT_EQ(little.text->line_terminators,
              (std::vector{LineTerminator::crlf, LineTerminator::cr,
                           LineTerminator::lf}));
    ASSERT_EQ(ssg::encode_text(*little.text).bytes, little_bytes);

    const auto big_bytes = fixture("utf16be-cr");
    const auto big = ssg::decode_text(big_bytes);
    ASSERT_TRUE(big.accepted());
    ASSERT_EQ(big.text->utf8, bytes({0x41, 0x0a, 0xce, 0xa9, 0x0a}));
    ASSERT_EQ(big.text->status.encoding, TextEncoding::utf16be);
    ASSERT_EQ(big.text->status.line_ending, LineEnding::cr);
    ASSERT_TRUE(big.text->status.final_newline);
    ASSERT_EQ(ssg::encode_text(*big.text).bytes, big_bytes);
}

TEST(manual_single_byte_decodes_are_byte_exact) {
    const auto windows = ssg::decode_text(
        fixture("windows1252"), TextEncoding::windows1252);
    ASSERT_TRUE(windows.accepted());
    ASSERT_EQ(windows.text->utf8, bytes({
        0xe2, 0x82, 0xac, 0x20, 0xe2, 0x80, 0x9c, 0x78,
        0xe2, 0x80, 0x9d, 0x0a}));
    ASSERT_EQ(windows.text->status.line_ending, LineEnding::crlf);
    ASSERT_EQ(ssg::encode_text(*windows.text).bytes, fixture("windows1252"));

    const auto latin = ssg::decode_text(
        fixture("iso88591"), TextEncoding::iso88591);
    ASSERT_TRUE(latin.accepted());
    ASSERT_EQ(latin.text->utf8, bytes({0xc3, 0xa9, 0x0a}));
    ASSERT_EQ(latin.text->status.line_ending, LineEnding::cr);
    ASSERT_EQ(ssg::encode_text(*latin.text).bytes, fixture("iso88591"));
}

TEST(refuses_invalid_input_without_replacement) {
    const auto invalid_utf8 = ssg::decode_text(fixture("invalid-utf8"));
    ASSERT_FALSE(invalid_utf8.accepted());
    ASSERT_EQ(invalid_utf8.error->code, ssg::TextEncodingErrorCode::invalid_input);
    ASSERT_EQ(invalid_utf8.error->utf8_offset, std::size_t{0});

    const auto invalid_utf16 = ssg::decode_text(fixture("invalid-utf16le"));
    ASSERT_FALSE(invalid_utf16.accepted());
    ASSERT_EQ(invalid_utf16.error->code,
              ssg::TextEncodingErrorCode::invalid_input);
}

TEST(refuses_lossy_single_byte_encoding_at_the_offending_offset) {
    const auto decoded = ssg::decode_text(fixture("lossy-utf8"));
    ASSERT_TRUE(decoded.accepted());

    const auto windows = ssg::encode_text(
        *decoded.text, {TextEncoding::windows1252, LineEnding::mixed,
                        FinalNewlinePolicy::preserve});
    ASSERT_FALSE(windows.accepted());
    ASSERT_EQ(windows.error->code, ssg::TextEncodingErrorCode::lossy_conversion);
    ASSERT_EQ(windows.error->utf8_offset, std::size_t{0});

    const auto latin = ssg::encode_text(
        *decoded.text, {TextEncoding::iso88591, LineEnding::mixed,
                        FinalNewlinePolicy::preserve});
    ASSERT_FALSE(latin.accepted());
}

TEST(normalizes_requested_endings_and_applies_final_newline_policy) {
    const auto decoded = ssg::decode_text(fixture("no-final-newline"));
    ASSERT_TRUE(decoded.accepted());

    const auto crlf = ssg::encode_text(
        *decoded.text, {TextEncoding::utf8, LineEnding::crlf,
                        FinalNewlinePolicy::ensure_present});
    ASSERT_TRUE(crlf.accepted());
    ASSERT_EQ(crlf.bytes, (std::vector<std::uint8_t>{
        0x6f, 0x6e, 0x65, 0x0d, 0x0a, 0x74, 0x77, 0x6f, 0x0d, 0x0a}));

    const auto absent = ssg::encode_text(
        *decoded.text, {TextEncoding::utf8, LineEnding::cr,
                        FinalNewlinePolicy::ensure_absent});
    ASSERT_TRUE(absent.accepted());
    ASSERT_EQ(absent.bytes, (std::vector<std::uint8_t>{
        0x6f, 0x6e, 0x65, 0x0d, 0x74, 0x77, 0x6f}));
}

TEST(exports_exact_immutable_command_set_and_typed_view_delta) {
    constexpr std::array expected{
        std::string_view{"file.reopen_with_encoding"},
        std::string_view{"file.set_encoding"},
        std::string_view{"file.set_line_ending"},
        std::string_view{"file.set_final_newline"},
    };
    static_assert(ssg::text_encoding_command_set.descriptors.size() == 4);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(ssg::text_encoding_command_set.descriptors[index].id,
                  expected[index]);
    }

    const auto before = ssg::decode_text(fixture("utf8-lf"));
    const auto after = ssg::decode_text(fixture("utf8-bom-crlf"));
    const auto before_view = ssg::make_text_encoding_view_state(*before.text);
    const auto after_view = ssg::make_text_encoding_view_state(*after.text);
    const auto delta = ssg::derive_text_encoding_delta(before_view, after_view);
    ASSERT_TRUE(delta.has_value());
    ASSERT_EQ(delta->before, before_view);
    ASSERT_EQ(delta->after, after_view);
    ASSERT_FALSE(ssg::derive_text_encoding_delta(after_view, after_view).has_value());
}

} // namespace

int main() {
    RUN(auto_detects_utf8_and_preserves_lf_and_final_newline);
    RUN(auto_detects_utf8_bom_and_preserves_crlf_without_final_newline);
    RUN(round_trips_utf16_endianness_bom_and_mixed_endings);
    RUN(manual_single_byte_decodes_are_byte_exact);
    RUN(refuses_invalid_input_without_replacement);
    RUN(refuses_lossy_single_byte_encoding_at_the_offending_offset);
    RUN(normalizes_requested_endings_and_applies_final_newline_policy);
    RUN(exports_exact_immutable_command_set_and_typed_view_delta);
    return failed == 0 ? 0 : 1;
}
