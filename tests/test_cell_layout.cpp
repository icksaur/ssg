// Oracle: tests/test_cell_layout.cpp
//
// Hand-authored grapheme segmentation and cell-run golden fixtures for
// include/ssg/layout.h.  Expected values are derived from the pinned
// fixture files in tests/fixtures/layout/cells/ and the normative contract
// in doc/features/presentation-shell.md §Cell-width rules.
//
// Unicode version: 15.0.0 (2022-09-13)
// Grapheme cluster rules: UAX #29 extended grapheme clusters
// Width rules: UAX #11 East Asian Width (W/F → 2 cells) + emoji-data

#include <ssg/layout.h>

#include "test_helpers.h"

#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Helpers

// Verify a single CellSpan field by field.
#define CHECK_SPAN(run, idx, off, len, wid, k)                     \
    do {                                                            \
        ASSERT_EQ((run).spans[(idx)].byte_offset,                  \
                  static_cast<uint32_t>(off));                     \
        ASSERT_EQ((run).spans[(idx)].byte_len,                     \
                  static_cast<uint32_t>(len));                     \
        ASSERT_EQ((run).spans[(idx)].cell_width,                   \
                  static_cast<uint32_t>(wid));                     \
        ASSERT_EQ((run).spans[(idx)].kind, (k));                   \
    } while (0)

static constexpr auto T   = ssg::CellKind::Text;
static constexpr auto C   = ssg::CellKind::Combining;
static constexpr auto TAB = ssg::CellKind::Tab;
static constexpr auto CTL = ssg::CellKind::Control;
static constexpr auto INV = ssg::CellKind::InvalidUtf8;

// ---------------------------------------------------------------------------
// ASCII fixtures (ascii.txt)

TEST(ascii_empty) {
    auto run = ssg::compute_cell_run("");
    ASSERT_EQ(run.total_cells, 0u);
    ASSERT_EQ(run.spans.size(), 0u);
}

TEST(ascii_single_space) {
    auto run = ssg::compute_cell_run(" ");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
}

TEST(ascii_hello) {
    auto run = ssg::compute_cell_run("hello");
    ASSERT_EQ(run.total_cells, 5u);
    ASSERT_EQ(run.spans.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK_SPAN(run, i, i, 1, 1, T);
    }
}

TEST(ascii_tilde_boundary) {
    // U+007E '~' is the last printable ASCII character; 1 cell
    auto run = ssg::compute_cell_run("~");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
}

// ---------------------------------------------------------------------------
// Combining mark fixtures (combining.txt)
//
// Combining marks are absorbed into the preceding cluster; the cluster
// width equals the base character's width.

TEST(combining_latin_a_acute) {
    // 'a' (61) + COMBINING ACUTE ACCENT U+0301 (CC 81) → 1 cluster, 3 bytes, 1 cell
    auto run = ssg::compute_cell_run("a\xCC\x81");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, T);
}

TEST(combining_latin_e_macron) {
    // 'e' (65) + COMBINING MACRON U+0304 (CC 84) → 1 cluster, 3 bytes, 1 cell
    auto run = ssg::compute_cell_run("e\xCC\x84");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, T);
}

TEST(combining_latin_a_two_combining) {
    // 'a' + U+0300 (CC 80, combining grave) + U+0303 (CC 83, combining tilde)
    // → 1 cluster, 5 bytes, 1 cell
    auto run = ssg::compute_cell_run("a\xCC\x80\xCC\x83");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 5, 1, T);
}

TEST(combining_lone_acute) {
    // COMBINING ACUTE ACCENT alone (CC 81): no base → kind=combining, width=0
    auto run = ssg::compute_cell_run("\xCC\x81");
    ASSERT_EQ(run.total_cells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, C);
}

TEST(combining_two_lone) {
    // Two consecutive combining marks with no base:
    // U+0301 (CC 81) + U+0300 (CC 80)
    // First U+0301 starts a combining cluster; U+0300 extends it.
    // → 1 span, 4 bytes, 0 cells, kind=combining
    auto run = ssg::compute_cell_run("\xCC\x81\xCC\x80");
    ASSERT_EQ(run.total_cells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 0, C);
}

TEST(combining_n_tilde) {
    // 'n' + COMBINING TILDE U+0303 (CC 83) → "ñ", 1 cluster, 3 bytes, 1 cell
    auto run = ssg::compute_cell_run("n\xCC\x83");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, T);
}

TEST(combining_wide_base) {
    // U+4E2D 中 (E4 B8 AD, 3 bytes) + COMBINING ACUTE (CC 81, 2 bytes)
    // → 1 cluster, 5 bytes, 2 cells (wide base), kind=text
    auto run = ssg::compute_cell_run("\xE4\xB8\xAD\xCC\x81");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 5, 2, T);
}

// ---------------------------------------------------------------------------
// Double-width fixtures (double_width.txt)

TEST(double_width_cjk_zhong) {
    // U+4E2D '中' (E4 B8 AD): EAW=W, 2 cells
    auto run = ssg::compute_cell_run("\xE4\xB8\xAD");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

TEST(double_width_fullwidth_A) {
    // U+FF21 'Ａ' (EF BC A1): EAW=F (fullwidth), 2 cells
    auto run = ssg::compute_cell_run("\xEF\xBC\xA1");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

TEST(double_width_hangul_ga) {
    // U+AC00 '가' (EA B0 80): EAW=W, 2 cells
    auto run = ssg::compute_cell_run("\xEA\xB0\x80");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

TEST(double_width_two_cjk) {
    // 中文: U+4E2D (E4 B8 AD) + U+6587 (E6 96 87)
    auto run = ssg::compute_cell_run("\xE4\xB8\xAD\xE6\x96\x87");
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
    CHECK_SPAN(run, 1, 3, 3, 2, T);
}

TEST(double_width_mixed_narrow_wide) {
    // "a中b": 'a' (61) + U+4E2D (E4 B8 AD) + 'b' (62)
    // cells: 1 + 2 + 1 = 4
    auto run = ssg::compute_cell_run("a\xE4\xB8\xAD" "b");
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 3, 2, T);
    CHECK_SPAN(run, 2, 4, 1, 1, T);
}

TEST(double_width_hiragana_a) {
    // U+3042 'あ' (E3 81 82): EAW=W, 2 cells
    auto run = ssg::compute_cell_run("\xE3\x81\x82");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

TEST(double_width_fullwidth_bang) {
    // U+FF01 '！' (EF BC 81): EAW=F, 2 cells
    auto run = ssg::compute_cell_run("\xEF\xBC\x81");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

// ---------------------------------------------------------------------------
// Emoji fixtures (emoji.txt)

TEST(emoji_grinning_face) {
    // U+1F600 😀 (F0 9F 98 80): wide emoji, 2 cells
    auto run = ssg::compute_cell_run("\xF0\x9F\x98\x80");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
}

TEST(emoji_slight_smile) {
    // U+1F642 🙂 (F0 9F 99 82): wide emoji, 2 cells
    auto run = ssg::compute_cell_run("\xF0\x9F\x99\x82");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
}

TEST(emoji_man_standalone) {
    // U+1F468 👨 MAN (F0 9F 91 A8): wide emoji, 2 cells
    auto run = ssg::compute_cell_run("\xF0\x9F\x91\xA8");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
}

TEST(emoji_man_zwj_woman) {
    // MAN (F0 9F 91 A8) + ZWJ (E2 80 8D) + WOMAN (F0 9F 91 A9)
    // ZWJ has GCB=ZWJ; WOMAN follows the armed GB11 pattern and is absorbed.
    // → 1 span, 11 bytes, 2 cells, kind=text
    const std::string_view seq = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 11, 2, T);
}

TEST(emoji_us_flag) {
    // 🇺🇸 = U+1F1FA (F0 9F 87 BA) + U+1F1F8 (F0 9F 87 B8)
    // Regional Indicator pair → GB12/GB13: one cluster, 2 cells
    const std::string_view seq = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 8, 2, T);
}

TEST(emoji_then_ascii) {
    // 😀 + 'A': 2 + 1 = 3 cells, 2 spans
    const std::string_view seq = "\xF0\x9F\x98\x80" "A";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
    CHECK_SPAN(run, 1, 4, 1, 1, T);
}

TEST(emoji_man_zwj_fullwidth_a) {
    // Adversarial GB11: ZWJ + wide non-ExtPic must NOT join.
    // U+1F468 (man, ExtPic) + ZWJ + U+FF21 (Ａ, wide but NOT ExtPic)
    // ZWJ absorbed into man via GB9; GB11 checks is_extpic(Ａ) → false → break.
    // → 2 clusters: [man+ZWJ, 7 bytes, 2 cells] + [Ａ, 3 bytes, 2 cells]
    const std::string_view seq = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xEF\xBC\xA1";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 7, 2, T);
    CHECK_SPAN(run, 1, 7, 3, 2, T);
}

TEST(emoji_thumbs_skin_tone) {
    // 👍 (U+1F44D, F0 9F 91 8D) + 🏻 (U+1F3FB, F0 9F 8F BB)
    // U+1F3FB has UAX #29 GCB=Extend → absorbed into 👍's cluster
    // → 1 span, 8 bytes, 2 cells (base=wide emoji)
    const std::string_view seq = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 8, 2, T);
}

TEST(emoji_two) {
    // 😀 + 🙂: 2 + 2 = 4 cells, 2 spans
    const std::string_view seq = "\xF0\x9F\x98\x80\xF0\x9F\x99\x82";
    auto run = ssg::compute_cell_run(seq);
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
    CHECK_SPAN(run, 1, 4, 4, 2, T);
}

// ---------------------------------------------------------------------------
// Tab fixtures (tab.txt)
//
// Tab advances to the next column that is a multiple of tab_width (≥ 1 col).

TEST(tab_col0_w4) {
    auto run = ssg::compute_cell_run("\t", 4);
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 4, TAB);
}

TEST(tab_col0_w8) {
    auto run = ssg::compute_cell_run("\t", 8);
    ASSERT_EQ(run.total_cells, 8u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 8, TAB);
}

TEST(tab_col0_w1) {
    auto run = ssg::compute_cell_run("\t", 1);
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, TAB);
}

TEST(tab_col0_w2) {
    auto run = ssg::compute_cell_run("\t", 2);
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 2, TAB);
}

TEST(tab_ab_tab_w4) {
    // "ab\t" tab_width=4:
    //   'a' at col 0 → col 1; 'b' at col 1 → col 2; '\t' at col 2 → col 4 (2 cells)
    auto run = ssg::compute_cell_run("ab\t", 4);
    ASSERT_EQ(run.total_cells, 4u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 1, 1, T);
    CHECK_SPAN(run, 2, 2, 1, 2, TAB);
}

TEST(tab_abcd_tab_w4) {
    // "abcd\t" tab_width=4:
    //   'a'..'d' land at cols 0-3; '\t' at col 4 (on stop) → advance 4 cells to col 8
    auto run = ssg::compute_cell_run("abcd\t", 4);
    ASSERT_EQ(run.total_cells, 8u);
    ASSERT_EQ(run.spans.size(), 5u);
    CHECK_SPAN(run, 4, 4, 1, 4, TAB);
}

TEST(tab_a_tab_b_w4) {
    // "a\tb" tab_width=4:
    //   'a' at col 0 → col 1; '\t' at col 1 → col 4 (3 cells); 'b' at col 4 → col 5
    auto run = ssg::compute_cell_run("a\tb", 4);
    ASSERT_EQ(run.total_cells, 5u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 1, 3, TAB);
    CHECK_SPAN(run, 2, 2, 1, 1, T);
}

TEST(tab_two_tabs_w4) {
    // Two consecutive tabs at col 0, tab_width=4:
    //   first: col 0 → col 4 (4 cells); second: col 4 → col 8 (4 cells)
    auto run = ssg::compute_cell_run("\t\t", 4);
    ASSERT_EQ(run.total_cells, 8u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 4, TAB);
    CHECK_SPAN(run, 1, 1, 1, 4, TAB);
}

TEST(tab_width_invalid_zero) {
    ASSERT_THROWS(ssg::compute_cell_run("", 0), std::invalid_argument);
}

TEST(tab_width_invalid_negative) {
    ASSERT_THROWS(ssg::compute_cell_run("", -1), std::invalid_argument);
}

TEST(tab_width_invalid_too_large) {
    ASSERT_THROWS(ssg::compute_cell_run("", 17), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Control character fixtures (control.txt)
//
// C0 controls (except tab), DEL, C1 controls → kind=control, cell_width=1.

TEST(control_nul) {
    // NUL (U+0000) = 00
    auto run = ssg::compute_cell_run(std::string_view("\x00", 1));
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_soh) {
    auto run = ssg::compute_cell_run("\x01");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_bel) {
    auto run = ssg::compute_cell_run("\x07");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_lf) {
    // LF (0x0A) — line terminator; treated as control by layout
    auto run = ssg::compute_cell_run("\x0A");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_esc) {
    auto run = ssg::compute_cell_run("\x1B");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_us) {
    // U+001F (last C0 before space)
    auto run = ssg::compute_cell_run("\x1F");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_del) {
    // DEL = 0x7F
    auto run = ssg::compute_cell_run("\x7F");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
}

TEST(control_c1_pad) {
    // U+0080 PAD (C1): encoded as C2 80 (2 bytes in UTF-8)
    auto run = ssg::compute_cell_run("\xC2\x80");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 1, CTL);
}

TEST(control_c1_apc) {
    // U+009F APC (last C1): encoded as C2 9F (2 bytes in UTF-8)
    auto run = ssg::compute_cell_run("\xC2\x9F");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 1, CTL);
}

TEST(control_two_controls) {
    auto run = ssg::compute_cell_run("\x07\x1B");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
    CHECK_SPAN(run, 1, 1, 1, 1, CTL);
}

TEST(control_ctl_then_text) {
    auto run = ssg::compute_cell_run("\x07" "A");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, CTL);
    CHECK_SPAN(run, 1, 1, 1, 1, T);
}

TEST(control_text_then_ctl) {
    auto run = ssg::compute_cell_run("A\x07");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 1, 1, CTL);
}

// ---------------------------------------------------------------------------
// Invalid UTF-8 fixtures (invalid_utf8.txt)
//
// Each invalid byte → one span: kind=invalid_utf8, byte_len=1, cell_width=1.

TEST(invalid_lone_ff) {
    auto run = ssg::compute_cell_run("\xFF");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
}

TEST(invalid_lone_fe) {
    auto run = ssg::compute_cell_run("\xFE");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
}

TEST(invalid_lone_continuation_80) {
    // Lone continuation byte 0x80 at start
    auto run = ssg::compute_cell_run("\x80");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
}

TEST(invalid_lone_continuation_bf) {
    // Lone continuation byte 0xBF at start
    auto run = ssg::compute_cell_run("\xBF");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
}

TEST(invalid_overlong_c0_80) {
    // 0xC0 0x80: C0 is an invalid lead (overlong), then 0x80 is a lone continuation
    auto run = ssg::compute_cell_run("\xC0\x80");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_overlong_c1_80) {
    // 0xC1 0x80: C1 is an invalid lead (overlong)
    auto run = ssg::compute_cell_run("\xC1\x80");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_two_ff) {
    auto run = ssg::compute_cell_run("\xFF\xFF");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_truncated_e4) {
    // 0xE4: 3-byte lead with no continuations → 1 invalid byte
    auto run = ssg::compute_cell_run("\xE4");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
}

TEST(invalid_truncated_e4_b8) {
    // 0xE4 0xB8: truncated 3-byte sequence (missing third byte)
    // 0xE4 → invalid lead (truncated); 0xB8 → standalone continuation → invalid
    auto run = ssg::compute_cell_run("\xE4\xB8");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_truncated_f0_9f) {
    // 0xF0 0x9F: truncated 4-byte sequence
    auto run = ssg::compute_cell_run("\xF0\x9F");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_valid_then_ff) {
    auto run = ssg::compute_cell_run("A\xFF");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
}

TEST(invalid_ff_then_valid) {
    auto run = ssg::compute_cell_run("\xFF" "A");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, T);
}

TEST(invalid_bad_continuation_e4_b8_41) {
    // 0xE4 0xB8 0x41: 3-byte lead + valid first cont + non-continuation
    // 0xE4 fails (third byte 0x41 is not 0x80-0xBF) → 0xE4 is invalid
    // 0xB8 is a standalone continuation → invalid
    // 0x41 'A' is valid ASCII
    auto run = ssg::compute_cell_run("\xE4\xB8\x41");
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
    CHECK_SPAN(run, 2, 2, 1, 1, T);
}

TEST(invalid_overlong_e0_80_80) {
    // 0xE0 requires second byte >= 0xA0; 0x80 < 0xA0 → 0xE0 is invalid
    // Both continuations become standalone invalids
    auto run = ssg::compute_cell_run("\xE0\x80\x80");
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
    CHECK_SPAN(run, 2, 2, 1, 1, INV);
}

TEST(invalid_surrogate_high) {
    // 0xED 0xA0 0x80 → U+D800 high surrogate; 0xED requires second byte <= 0x9F
    // 0xA0 > 0x9F → 0xED is invalid
    auto run = ssg::compute_cell_run("\xED\xA0\x80");
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, INV);
    CHECK_SPAN(run, 1, 1, 1, 1, INV);
    CHECK_SPAN(run, 2, 2, 1, 1, INV);
}

// ---------------------------------------------------------------------------
// Hangul jamo composition fixtures (hangul.txt) — GB6/GB7/GB8

TEST(hangul_L_V) {
    // U+1100 ᄀ (E1 84 80) + U+1161 ᅡ (E1 85 A1) → 1 cluster via GB6 (L × V)
    // Base L jamo is wide (EAW=W) → cluster width = 2
    auto run = ssg::compute_cell_run("\xE1\x84\x80\xE1\x85\xA1");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, T);
}

TEST(hangul_L_V_T) {
    // L (E1 84 80) + V (E1 85 A1) + T/U+11A8 (E1 86 88) → 1 cluster, GB6+GB7
    auto run = ssg::compute_cell_run("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 9, 2, T);
}

TEST(hangul_LV_T) {
    // U+AC00 가 (EA B0 80) + U+11A8 ᆨ (E1 86 88) → 1 cluster via GB7 (LV × T)
    // LV syllable is wide → cluster width = 2
    auto run = ssg::compute_cell_run("\xEA\xB0\x80\xE1\x86\xA8");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, T);
}

TEST(hangul_LVT_T) {
    // U+AC01 각 (EA B0 81) + U+11A8 ᆨ (E1 86 88) → 1 cluster via GB8 (LVT × T)
    auto run = ssg::compute_cell_run("\xEA\xB0\x81\xE1\x86\xA8");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, T);
}

TEST(hangul_L_extend_V_no_compose) {
    // Adversarial per UAX #29 GraphemeBreakTest.txt: ÷ 1100 × 0308 ÷ 1160 ÷
    // L + Extend (combining diaeresis U+0308, CC 88) + V (E1 85 A1)
    // GB6–GB8 have no Extend*: the Extend severs Hangul composition.
    // → 2 clusters: [L+Extend=5 bytes=2 cells] + [V=3 bytes=1 cell]
    auto run = ssg::compute_cell_run("\xE1\x84\x80\xCC\x88\xE1\x85\xA1");
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 5, 2, T);
    CHECK_SPAN(run, 1, 5, 3, 1, T);
}

TEST(hangul_L_ascii_no_compose) {
    // Adversarial: L jamo + ASCII 'A' must NOT compose (only L/V/LV/LVT follow L)
    // → 2 clusters: [L=2 cells] + [A=1 cell]
    auto run = ssg::compute_cell_run("\xE1\x84\x80\x41");
    ASSERT_EQ(run.total_cells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
    CHECK_SPAN(run, 1, 3, 1, 1, T);
}

TEST(hangul_lv_alone) {
    // Standalone LV syllable 가 (U+AC00, EA B0 80): 1 cluster, 2 cells
    auto run = ssg::compute_cell_run("\xEA\xB0\x80");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, T);
}

// ---------------------------------------------------------------------------
// SpacingMark fixtures (spacing_mark.txt) — GB9a

TEST(spacing_mark_devanagari_kaa) {
    // क (U+0915, E0 A4 95) + ā (U+093E, E0 A4 BE, SpacingMark)
    // GB9a: SpacingMark extends base → 1 cluster, 1 cell (narrow base)
    // Adversarial: without GB9a, would be 2 clusters (2 cells)
    auto run = ssg::compute_cell_run("\xE0\xA4\x95\xE0\xA4\xBE");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, T);
}

TEST(spacing_mark_devanagari_ko) {
    // क (U+0915) + ो (U+094B, E0 A5 8B, SpacingMark) → को, 1 cluster, 1 cell
    auto run = ssg::compute_cell_run("\xE0\xA4\x95\xE0\xA5\x8B");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, T);
}

TEST(spacing_mark_lone) {
    // Lone SpacingMark U+093E (E0 A4 BE) at line start → kind=combining, width=0
    auto run = ssg::compute_cell_run("\xE0\xA4\xBE");
    ASSERT_EQ(run.total_cells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, C);
}

TEST(spacing_mark_bengali_kaa) {
    // ক (U+0995, E0 A6 95) + া (U+09BE, E0 A6 BE, SpacingMark) → কা, 1 cluster
    auto run = ssg::compute_cell_run("\xE0\xA6\x95\xE0\xA6\xBE");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, T);
}

// ---------------------------------------------------------------------------
// Prepend fixtures (prepend.txt) — GB9b

TEST(prepend_0600_digit) {
    // U+0600 Arabic Number Sign (D8 80, Prepend) + '1' (31)
    // GB9b: Prepend absorbs '1'; cluster width = 1 (digit is narrow)
    auto run = ssg::compute_cell_run("\xD8\x80\x31");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, T);
}

TEST(prepend_a_then_prepend_digit) {
    // Adversarial: 'a' then Prepend+digit → 2 clusters.
    // Prepend does NOT extend the preceding 'a' cluster.
    // 'a' = cluster 1 {0, 1, 1, T}; [U+0600 + '1'] = cluster 2 {1, 3, 1, T}
    auto run = ssg::compute_cell_run("a\xD8\x80\x31");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
    CHECK_SPAN(run, 1, 1, 3, 1, T);
}

TEST(prepend_lone_at_eol) {
    // Standalone Prepend U+0600 at end of line: no char to absorb → width=0
    auto run = ssg::compute_cell_run("\xD8\x80");
    ASSERT_EQ(run.total_cells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, C);
}

TEST(prepend_before_control) {
    // Prepend (D8 80) before BEL (07): control is GCB-Control → not absorbed.
    // → Prepend cluster {0, 2, 0, C} + control cluster {2, 1, 1, CTL}
    auto run = ssg::compute_cell_run("\xD8\x80\x07");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 2, 0, C);
    CHECK_SPAN(run, 1, 2, 1, 1, CTL);
}

// ---------------------------------------------------------------------------
// Additional edge cases

TEST(edge_tab_then_combining) {
    // '\t' (tab) is never extended; a combining mark after tab starts its own cluster
    // "\ta\xCC\x81": tab(4 cells) + a+combining_acute(1 cluster, 3 bytes, 1 cell)
    auto run = ssg::compute_cell_run("\ta\xCC\x81", 4);
    ASSERT_EQ(run.total_cells, 5u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 4, TAB);
    CHECK_SPAN(run, 1, 1, 3, 1, T);
}

TEST(edge_valid_3byte_cjk) {
    // U+4E2D (中): full valid 3-byte sequence
    const std::string_view zhong = "\xE4\xB8\xAD";
    ASSERT_EQ(ssg::compute_cell_run(zhong).total_cells, 2u);
}

TEST(edge_variation_selector) {
    // U+0023 '#' + U+FE0F VS-16 (EF B8 8F, variation selector 16)
    // '#' is Emoji=Yes, Emoji_Presentation=No → alone=1 cell (text-default emoji)
    // VS-16 (GCB=Extend) is absorbed via GB9; saw_vs16=true triggers upgrade to 2
    // → 1 span, 4 bytes, 2 cells (emoji presentation sequence)
    auto run = ssg::compute_cell_run("#\xEF\xB8\x8F");
    ASSERT_EQ(run.total_cells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
}

TEST(emoji_scissors_alone) {
    // U+2702 ✂ BLACK SCISSORS (E2 9C 82): EAW=N, Emoji=Yes, Emoji_Presentation=No
    // Text-default emoji; alone → 1 cell (was INCORRECTLY 2 in old k_wide[])
    auto run = ssg::compute_cell_run("\xE2\x9C\x82");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, T);
    ASSERT_EQ(run.total_cells, 1u);
}

TEST(emoji_scissors_vs16) {
    // U+2702 ✂ + U+FE0F VS-16 (E2 9C 82 EF B8 8F) = 6 bytes
    // Emoji=Yes + VS-16 absorbed → upgrade to 2 cells
    auto run = ssg::compute_cell_run("\xE2\x9C\x82\xEF\xB8\x8F");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, T);
    ASSERT_EQ(run.total_cells, 2u);
}

TEST(edge_space_is_printable) {
    // Space (U+0020) is printable ASCII, 1 cell, NOT a control character
    auto run = ssg::compute_cell_run(" ");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
}

// ---------------------------------------------------------------------------
// Adversarial fixtures — GB11 state machine bugs (must pass with new code)
// Bug 1: lone ZWJ base followed by ExtPic → was 1 cluster (ZWJ treated as ExtPic
//         base), should be 2 clusters.
// Bug 2: ExtPic + ZWJ + ZWJ + ExtPic → was 1 cluster (second ZWJ re-armed
//         after_zwj), should be 2 clusters (ZWJ ZWJ breaks Extend* pattern).

TEST(adv_lone_zwj_before_extpic) {
    // U+200D ZWJ (E2 80 8D) + U+1F600 GRINNING FACE (F0 9F 98 80)
    // ZWJ alone is not ExtPic; it does not satisfy the ExtPic precondition of GB11.
    // Expected: 2 clusters — [ZWJ] {0,3,0,C} and [😀] {3,4,2,T}
    auto run = ssg::compute_cell_run("\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 0, C);  // ZWJ: combining, 0 cells
    CHECK_SPAN(run, 1, 3, 4, 2, T);  // 😀: text, 2 cells
    ASSERT_EQ(run.total_cells, 2u);
}

TEST(adv_extpic_zwj_zwj_extpic) {
    // U+1F600 (F0 9F 98 80) + ZWJ (E2 80 8D) + ZWJ (E2 80 8D) + U+1F600
    // Pattern: ExtPic ZWJ ZWJ ExtPic.  The second ZWJ has GCB=ZWJ (not Extend)
    // so it is NOT part of Extend*; GB11 requires ExtPic Extend* ZWJ × ExtPic.
    // Two ZWJs break the pattern → 2 clusters.
    // Cluster 1: 1F600 + ZWJ + ZWJ (11 bytes, 2 cells)
    // Cluster 2: 1F600 (4 bytes, 2 cells)
    auto run = ssg::compute_cell_run(
        "\xF0\x9F\x98\x80\xE2\x80\x8D\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0,  10, 2, T);  // 😀+ZWJ+ZWJ absorbed via GB9, no GB11
    CHECK_SPAN(run, 1, 10, 4,  2, T);  // 😀 in its own cluster
    ASSERT_EQ(run.total_cells, 4u);
}

TEST(adv_soft_hyphen_own_cluster) {
    // U+00AD SOFT HYPHEN (C2 AD): GCB=Control (not Extend).
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::compute_cell_run("\xC2\xAD");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, CTL);
    ASSERT_EQ(run.total_cells, 0u);
}

TEST(adv_zwsp_own_cluster) {
    // U+200B ZERO WIDTH SPACE (E2 80 8B): GCB=Control (not Extend).
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::compute_cell_run("\xE2\x80\x8B");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, CTL);
    ASSERT_EQ(run.total_cells, 0u);
}

TEST(adv_three_regional_indicators) {
    // 🇺 (U+1F1FA, F0 9F 87 BA) + 🇸 (U+1F1F8, F0 9F 87 B8) + 🇦 (U+1F1E6, F0 9F 87 A6)
    // GB12/13: first RI pair → cluster 1 [🇺🇸] (8 bytes, 2 cells)
    // Third RI starts a new cluster: cluster 2 [🇦] (4 bytes, 2 cells)
    auto run = ssg::compute_cell_run(
        "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xA6");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 8, 2, T);   // 🇺🇸 flag pair
    CHECK_SPAN(run, 1, 8, 4, 2, T);   // 🇦 lone RI
    ASSERT_EQ(run.total_cells, 4u);
}

TEST(adv_extpic_zwj_extend_no_gb11) {
    // U+1F600 + ZWJ + Emoji Modifier 🏻 (U+1F3FB, F0 9F 8F BB, GCB=Extend) + U+1F600
    // After ExtPic + ZWJ → gb11=Zwj; then Extend → gb11=None (Extend after Zwj breaks chain)
    // So the second 1F600 does NOT absorb via GB11 → 2 clusters
    // Cluster 1: 1F600 + ZWJ + 1F3FB (11 bytes, 2 cells)
    // Cluster 2: 1F600 (4 bytes, 2 cells)
    auto run = ssg::compute_cell_run(
        "\xF0\x9F\x98\x80\xE2\x80\x8D\xF0\x9F\x8F\xBB\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0,  11, 2, T);
    CHECK_SPAN(run, 1, 11, 4,  2, T);
    ASSERT_EQ(run.total_cells, 4u);
}

TEST(adv_extpic_extend_zwj_extpic_gb11) {
    // U+1F600 + Variation Selector VS-16 (U+FE0F, GCB=Extend) + ZWJ + U+1F600
    // ExtPic Extend ZWJ ExtPic → all one cluster via GB11
    // F0 9F 98 80  EF B8 8F  E2 80 8D  F0 9F 98 80  = 15 bytes
    auto run = ssg::compute_cell_run(
        "\xF0\x9F\x98\x80\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 14, 2, T);  // 4+3+3+4 bytes
    ASSERT_EQ(run.total_cells, 2u);
}

TEST(adv_lone_emoji_modifier) {
    // U+1F3FB EMOJI MODIFIER FITZPATRICK TYPE-1-2 (F0 9F 8F BB)
    // GCB=Extend, EAW=W (in k_wide range 1F3F7–1F4FD).
    // NOT Extended_Pictographic; when standalone (no preceding base) it renders
    // as a 2-cell wide glyph — base_width=2, kind=text (wide lone Extend).
    auto run = ssg::compute_cell_run("\xF0\x9F\x8F\xBB");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, T);
    ASSERT_EQ(run.total_cells, 2u);
}

TEST(adv_bidi_control_own_cluster) {
    // U+202A LEFT-TO-RIGHT EMBEDDING (E2 80 AA): GCB=Control.
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::compute_cell_run("\xE2\x80\xAA");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, CTL);
    ASSERT_EQ(run.total_cells, 0u);
}

TEST(adv_prepend_extend_breaks_gb9b) {
    // U+0600 Prepend + U+0308 Extend + 'A'
    // (0600, 0308): GB9 absorbs Extend → last_gcb=Other (no longer Prepend)
    // (Other/Extend, A): no applicable rule → break
    // Expected: 2 clusters — [0600+0308] {0,4,0,T}, ['A'] {4,1,1,T}
    // D8 80 = U+0600, CC 88 = U+0308, 41 = 'A'
    auto run = ssg::compute_cell_run("\xD8\x80\xCC\x88\x41");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 0, C);  // Prepend+Extend: no visible base absorbed
    CHECK_SPAN(run, 1, 4, 1, 1, T);  // 'A': separate cluster
    ASSERT_EQ(run.total_cells, 1u);
}

// ---------------------------------------------------------------------------

int main() {

    // ASCII
    RUN(ascii_empty);
    RUN(ascii_single_space);
    RUN(ascii_hello);
    RUN(ascii_tilde_boundary);

    // Combining
    RUN(combining_latin_a_acute);
    RUN(combining_latin_e_macron);
    RUN(combining_latin_a_two_combining);
    RUN(combining_lone_acute);
    RUN(combining_two_lone);
    RUN(combining_n_tilde);
    RUN(combining_wide_base);

    // Double-width
    RUN(double_width_cjk_zhong);
    RUN(double_width_fullwidth_A);
    RUN(double_width_hangul_ga);
    RUN(double_width_two_cjk);
    RUN(double_width_mixed_narrow_wide);
    RUN(double_width_hiragana_a);
    RUN(double_width_fullwidth_bang);

    // Emoji
    RUN(emoji_grinning_face);
    RUN(emoji_slight_smile);
    RUN(emoji_man_standalone);
    RUN(emoji_man_zwj_woman);
    RUN(emoji_us_flag);
    RUN(emoji_then_ascii);
    RUN(emoji_man_zwj_fullwidth_a);
    RUN(emoji_thumbs_skin_tone);
    RUN(emoji_two);

    // Tab
    RUN(tab_col0_w4);
    RUN(tab_col0_w8);
    RUN(tab_col0_w1);
    RUN(tab_col0_w2);
    RUN(tab_ab_tab_w4);
    RUN(tab_abcd_tab_w4);
    RUN(tab_a_tab_b_w4);
    RUN(tab_two_tabs_w4);
    RUN(tab_width_invalid_zero);
    RUN(tab_width_invalid_negative);
    RUN(tab_width_invalid_too_large);

    // Control
    RUN(control_nul);
    RUN(control_soh);
    RUN(control_bel);
    RUN(control_lf);
    RUN(control_esc);
    RUN(control_us);
    RUN(control_del);
    RUN(control_c1_pad);
    RUN(control_c1_apc);
    RUN(control_two_controls);
    RUN(control_ctl_then_text);
    RUN(control_text_then_ctl);

    // Invalid UTF-8
    RUN(invalid_lone_ff);
    RUN(invalid_lone_fe);
    RUN(invalid_lone_continuation_80);
    RUN(invalid_lone_continuation_bf);
    RUN(invalid_overlong_c0_80);
    RUN(invalid_overlong_c1_80);
    RUN(invalid_two_ff);
    RUN(invalid_truncated_e4);
    RUN(invalid_truncated_e4_b8);
    RUN(invalid_truncated_f0_9f);
    RUN(invalid_valid_then_ff);
    RUN(invalid_ff_then_valid);
    RUN(invalid_bad_continuation_e4_b8_41);
    RUN(invalid_overlong_e0_80_80);
    RUN(invalid_surrogate_high);

    // Hangul
    RUN(hangul_L_V);
    RUN(hangul_L_V_T);
    RUN(hangul_LV_T);
    RUN(hangul_LVT_T);
    RUN(hangul_L_extend_V_no_compose);
    RUN(hangul_L_ascii_no_compose);
    RUN(hangul_lv_alone);

    // SpacingMark (GB9a)
    RUN(spacing_mark_devanagari_kaa);
    RUN(spacing_mark_devanagari_ko);
    RUN(spacing_mark_lone);
    RUN(spacing_mark_bengali_kaa);

    // Prepend (GB9b)
    RUN(prepend_0600_digit);
    RUN(prepend_a_then_prepend_digit);
    RUN(prepend_lone_at_eol);
    RUN(prepend_before_control);

    // Adversarial fixtures — GB11, GCB=Control chars, RI, Prepend+Extend
    RUN(adv_lone_zwj_before_extpic);
    RUN(adv_extpic_zwj_zwj_extpic);
    RUN(adv_soft_hyphen_own_cluster);
    RUN(adv_zwsp_own_cluster);
    RUN(adv_three_regional_indicators);
    RUN(adv_extpic_zwj_extend_no_gb11);
    RUN(adv_extpic_extend_zwj_extpic_gb11);
    RUN(adv_lone_emoji_modifier);
    RUN(adv_bidi_control_own_cluster);
    RUN(adv_prepend_extend_breaks_gb9b);

    // Edge cases
    RUN(edge_tab_then_combining);
    RUN(edge_valid_3byte_cjk);
    RUN(edge_variation_selector);
    RUN(emoji_scissors_alone);
    RUN(emoji_scissors_vs16);
    RUN(edge_space_is_printable);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
