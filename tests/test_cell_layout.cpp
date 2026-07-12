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

static constexpr auto T   = ssg::CellKind::text;
static constexpr auto C   = ssg::CellKind::combining;
static constexpr auto TAB = ssg::CellKind::tab;
static constexpr auto CTL = ssg::CellKind::control;
static constexpr auto INV = ssg::CellKind::invalid_utf8;

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
    // ZWJ is in k_combining; WOMAN (wide) follows ZWJ → GB11, absorbed into cluster.
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
    // VS-16 is in k_combining (range FE00-FE0F) → extends '#' cluster
    // '#' is ASCII narrow (1 cell); VS-16 adds 0 cells
    // → 1 span, 4 bytes, 1 cell
    auto run = ssg::compute_cell_run("#\xEF\xB8\x8F");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 1, T);
}

TEST(edge_space_is_printable) {
    // Space (U+0020) is printable ASCII, 1 cell, NOT a control character
    auto run = ssg::compute_cell_run(" ");
    ASSERT_EQ(run.total_cells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, T);
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== SSG cell layout tests ===\n";

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

    // Edge cases
    RUN(edge_tab_then_combining);
    RUN(edge_valid_3byte_cjk);
    RUN(edge_variation_selector);
    RUN(edge_space_is_printable);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
