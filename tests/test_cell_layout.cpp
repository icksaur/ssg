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
        ASSERT_EQ((run).spans[(idx)].byteOffset,                  \
                  static_cast<uint32_t>(off));                     \
        ASSERT_EQ((run).spans[(idx)].byteLen,                     \
                  static_cast<uint32_t>(len));                     \
        ASSERT_EQ((run).spans[(idx)].cellWidth,                   \
                  static_cast<uint32_t>(wid));                     \
        ASSERT_EQ((run).spans[(idx)].kind, (k));                   \
    } while (0)

static constexpr auto kT   = ssg::CellKind::Text;
static constexpr auto kC   = ssg::CellKind::Combining;
static constexpr auto kTab = ssg::CellKind::Tab;
static constexpr auto kCtl = ssg::CellKind::Control;
static constexpr auto kInv = ssg::CellKind::InvalidUtf8;

// ---------------------------------------------------------------------------
// ASCII fixtures (ascii.txt)

TEST(asciiEmpty) {
    auto run = ssg::computeCellRun("");
    ASSERT_EQ(run.totalCells, 0u);
    ASSERT_EQ(run.spans.size(), 0u);
}

TEST(asciiSingleSpace) {
    auto run = ssg::computeCellRun(" ");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
}

TEST(asciiHello) {
    auto run = ssg::computeCellRun("hello");
    ASSERT_EQ(run.totalCells, 5u);
    ASSERT_EQ(run.spans.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK_SPAN(run, i, i, 1, 1, kT);
    }
}

TEST(asciiTildeBoundary) {
    // U+007E '~' is the last printable ASCII character; 1 cell
    auto run = ssg::computeCellRun("~");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
}

// ---------------------------------------------------------------------------
// Combining mark fixtures (combining.txt)
//
// Combining marks are absorbed into the preceding cluster; the cluster
// width equals the base character's width.

TEST(combiningLatinAAcute) {
    // 'a' (61) + COMBINING ACUTE ACCENT U+0301 (CC 81) → 1 cluster, 3 bytes, 1 cell
    auto run = ssg::computeCellRun("a\xCC\x81");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, kT);
}

TEST(combiningLatinEMacron) {
    // 'e' (65) + COMBINING MACRON U+0304 (CC 84) → 1 cluster, 3 bytes, 1 cell
    auto run = ssg::computeCellRun("e\xCC\x84");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, kT);
}

TEST(combiningLatinATwoCombining) {
    // 'a' + U+0300 (CC 80, combining grave) + U+0303 (CC 83, combining tilde)
    // → 1 cluster, 5 bytes, 1 cell
    auto run = ssg::computeCellRun("a\xCC\x80\xCC\x83");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 5, 1, kT);
}

TEST(combiningLoneAcute) {
    // COMBINING ACUTE ACCENT alone (CC 81): no base → kind=combining, width=0
    auto run = ssg::computeCellRun("\xCC\x81");
    ASSERT_EQ(run.totalCells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, kC);
}

TEST(combiningTwoLone) {
    // Two consecutive combining marks with no base:
    // U+0301 (CC 81) + U+0300 (CC 80)
    // First U+0301 starts a combining cluster; U+0300 extends it.
    // → 1 span, 4 bytes, 0 cells, kind=combining
    auto run = ssg::computeCellRun("\xCC\x81\xCC\x80");
    ASSERT_EQ(run.totalCells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 0, kC);
}

TEST(combiningNTilde) {
    // 'n' + COMBINING TILDE U+0303 (CC 83) → "ñ", 1 cluster, 3 bytes, 1 cell
    auto run = ssg::computeCellRun("n\xCC\x83");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, kT);
}

TEST(combiningWideBase) {
    // U+4E2D 中 (E4 B8 AD, 3 bytes) + COMBINING ACUTE (CC 81, 2 bytes)
    // → 1 cluster, 5 bytes, 2 cells (wide base), kind=text
    auto run = ssg::computeCellRun("\xE4\xB8\xAD\xCC\x81");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 5, 2, kT);
}

// ---------------------------------------------------------------------------
// Double-width fixtures (double_width.txt)

TEST(doubleWidthCjkZhong) {
    // U+4E2D '中' (E4 B8 AD): EAW=W, 2 cells
    auto run = ssg::computeCellRun("\xE4\xB8\xAD");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

TEST(doubleWidthFullwidthA) {
    // U+FF21 'Ａ' (EF BC A1): EAW=F (fullwidth), 2 cells
    auto run = ssg::computeCellRun("\xEF\xBC\xA1");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

TEST(doubleWidthHangulGa) {
    // U+AC00 '가' (EA B0 80): EAW=W, 2 cells
    auto run = ssg::computeCellRun("\xEA\xB0\x80");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

TEST(doubleWidthTwoCjk) {
    // 中文: U+4E2D (E4 B8 AD) + U+6587 (E6 96 87)
    auto run = ssg::computeCellRun("\xE4\xB8\xAD\xE6\x96\x87");
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
    CHECK_SPAN(run, 1, 3, 3, 2, kT);
}

TEST(doubleWidthMixedNarrowWide) {
    // "a中b": 'a' (61) + U+4E2D (E4 B8 AD) + 'b' (62)
    // cells: 1 + 2 + 1 = 4
    auto run = ssg::computeCellRun("a\xE4\xB8\xAD" "b");
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 3, 2, kT);
    CHECK_SPAN(run, 2, 4, 1, 1, kT);
}

TEST(doubleWidthHiraganaA) {
    // U+3042 'あ' (E3 81 82): EAW=W, 2 cells
    auto run = ssg::computeCellRun("\xE3\x81\x82");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

TEST(doubleWidthFullwidthBang) {
    // U+FF01 '！' (EF BC 81): EAW=F, 2 cells
    auto run = ssg::computeCellRun("\xEF\xBC\x81");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

// ---------------------------------------------------------------------------
// Emoji fixtures (emoji.txt)

TEST(emojiGrinningFace) {
    // U+1F600 😀 (F0 9F 98 80): wide emoji, 2 cells
    auto run = ssg::computeCellRun("\xF0\x9F\x98\x80");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
}

TEST(emojiSlightSmile) {
    // U+1F642 🙂 (F0 9F 99 82): wide emoji, 2 cells
    auto run = ssg::computeCellRun("\xF0\x9F\x99\x82");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
}

TEST(emojiManStandalone) {
    // U+1F468 👨 MAN (F0 9F 91 A8): wide emoji, 2 cells
    auto run = ssg::computeCellRun("\xF0\x9F\x91\xA8");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
}

TEST(emojiManZwjWoman) {
    // MAN (F0 9F 91 A8) + ZWJ (E2 80 8D) + WOMAN (F0 9F 91 A9)
    // ZWJ has GCB=ZWJ; WOMAN follows the armed GB11 pattern and is absorbed.
    // → 1 span, 11 bytes, 2 cells, kind=text
    const std::string_view seq = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 11, 2, kT);
}

TEST(emojiUsFlag) {
    // 🇺🇸 = U+1F1FA (F0 9F 87 BA) + U+1F1F8 (F0 9F 87 B8)
    // Regional Indicator pair → GB12/GB13: one cluster, 2 cells
    const std::string_view seq = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 8, 2, kT);
}

TEST(emojiThenAscii) {
    // 😀 + 'A': 2 + 1 = 3 cells, 2 spans
    const std::string_view seq = "\xF0\x9F\x98\x80" "A";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
    CHECK_SPAN(run, 1, 4, 1, 1, kT);
}

TEST(emojiManZwjFullwidthA) {
    // Adversarial GB11: ZWJ + wide non-ExtPic must NOT join.
    // U+1F468 (man, ExtPic) + ZWJ + U+FF21 (Ａ, wide but NOT ExtPic)
    // ZWJ absorbed into man via GB9; GB11 checks is_extpic(Ａ) → false → break.
    // → 2 clusters: [man+ZWJ, 7 bytes, 2 cells] + [Ａ, 3 bytes, 2 cells]
    const std::string_view seq = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xEF\xBC\xA1";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 7, 2, kT);
    CHECK_SPAN(run, 1, 7, 3, 2, kT);
}

TEST(emojiThumbsSkinTone) {
    // 👍 (U+1F44D, F0 9F 91 8D) + 🏻 (U+1F3FB, F0 9F 8F BB)
    // U+1F3FB has UAX #29 GCB=Extend → absorbed into 👍's cluster
    // → 1 span, 8 bytes, 2 cells (base=wide emoji)
    const std::string_view seq = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 8, 2, kT);
}

TEST(emojiTwo) {
    // 😀 + 🙂: 2 + 2 = 4 cells, 2 spans
    const std::string_view seq = "\xF0\x9F\x98\x80\xF0\x9F\x99\x82";
    auto run = ssg::computeCellRun(seq);
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
    CHECK_SPAN(run, 1, 4, 4, 2, kT);
}

// ---------------------------------------------------------------------------
// Tab fixtures (tab.txt)
//
// Tab advances to the next column that is a multiple of tab_width (≥ 1 col).

TEST(tabCol0W4) {
    auto run = ssg::computeCellRun("\t", 4);
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 4, kTab);
}

TEST(tabCol0W8) {
    auto run = ssg::computeCellRun("\t", 8);
    ASSERT_EQ(run.totalCells, 8u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 8, kTab);
}

TEST(tabCol0W1) {
    auto run = ssg::computeCellRun("\t", 1);
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kTab);
}

TEST(tabCol0W2) {
    auto run = ssg::computeCellRun("\t", 2);
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 2, kTab);
}

TEST(tabAbTabW4) {
    // "ab\t" tab_width=4:
    //   'a' at col 0 → col 1; 'b' at col 1 → col 2; '\t' at col 2 → col 4 (2 cells)
    auto run = ssg::computeCellRun("ab\t", 4);
    ASSERT_EQ(run.totalCells, 4u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 1, 1, kT);
    CHECK_SPAN(run, 2, 2, 1, 2, kTab);
}

TEST(tabAbcdTabW4) {
    // "abcd\t" tab_width=4:
    //   'a'..'d' land at cols 0-3; '\t' at col 4 (on stop) → advance 4 cells to col 8
    auto run = ssg::computeCellRun("abcd\t", 4);
    ASSERT_EQ(run.totalCells, 8u);
    ASSERT_EQ(run.spans.size(), 5u);
    CHECK_SPAN(run, 4, 4, 1, 4, kTab);
}

TEST(tabATabBW4) {
    // "a\tb" tab_width=4:
    //   'a' at col 0 → col 1; '\t' at col 1 → col 4 (3 cells); 'b' at col 4 → col 5
    auto run = ssg::computeCellRun("a\tb", 4);
    ASSERT_EQ(run.totalCells, 5u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 1, 3, kTab);
    CHECK_SPAN(run, 2, 2, 1, 1, kT);
}

TEST(tabTwoTabsW4) {
    // Two consecutive tabs at col 0, tab_width=4:
    //   first: col 0 → col 4 (4 cells); second: col 4 → col 8 (4 cells)
    auto run = ssg::computeCellRun("\t\t", 4);
    ASSERT_EQ(run.totalCells, 8u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 4, kTab);
    CHECK_SPAN(run, 1, 1, 1, 4, kTab);
}

TEST(tabWidthInvalidZero) {
    ASSERT_THROWS(ssg::computeCellRun("", 0), std::invalid_argument);
}

TEST(tabWidthInvalidNegative) {
    ASSERT_THROWS(ssg::computeCellRun("", -1), std::invalid_argument);
}

TEST(tabWidthInvalidTooLarge) {
    ASSERT_THROWS(ssg::computeCellRun("", 17), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Control character fixtures (control.txt)
//
// C0 controls (except tab), DEL, C1 controls → kind=control, cell_width=1.

TEST(controlNul) {
    // NUL (U+0000) = 00
    auto run = ssg::computeCellRun(std::string_view("\x00", 1));
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlSoh) {
    auto run = ssg::computeCellRun("\x01");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlBel) {
    auto run = ssg::computeCellRun("\x07");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlLf) {
    // LF (0x0A) — line terminator; treated as control by layout
    auto run = ssg::computeCellRun("\x0A");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlEsc) {
    auto run = ssg::computeCellRun("\x1B");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlUs) {
    // U+001F (last C0 before space)
    auto run = ssg::computeCellRun("\x1F");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlDel) {
    // DEL = 0x7F
    auto run = ssg::computeCellRun("\x7F");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
}

TEST(controlC1Pad) {
    // U+0080 PAD (C1): encoded as C2 80 (2 bytes in UTF-8)
    auto run = ssg::computeCellRun("\xC2\x80");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 1, kCtl);
}

TEST(controlC1Apc) {
    // U+009F APC (last C1): encoded as C2 9F (2 bytes in UTF-8)
    auto run = ssg::computeCellRun("\xC2\x9F");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 1, kCtl);
}

TEST(controlTwoControls) {
    auto run = ssg::computeCellRun("\x07\x1B");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
    CHECK_SPAN(run, 1, 1, 1, 1, kCtl);
}

TEST(controlCtlThenText) {
    auto run = ssg::computeCellRun("\x07" "A");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kCtl);
    CHECK_SPAN(run, 1, 1, 1, 1, kT);
}

TEST(controlTextThenCtl) {
    auto run = ssg::computeCellRun("A\x07");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 1, 1, kCtl);
}

// ---------------------------------------------------------------------------
// Invalid UTF-8 fixtures (invalid_utf8.txt)
//
// Each invalid byte → one span: kind=invalid_utf8, byte_len=1, cell_width=1.

TEST(invalidLoneFf) {
    auto run = ssg::computeCellRun("\xFF");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
}

TEST(invalidLoneFe) {
    auto run = ssg::computeCellRun("\xFE");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
}

TEST(invalidLoneContinuation80) {
    // Lone continuation byte 0x80 at start
    auto run = ssg::computeCellRun("\x80");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
}

TEST(invalidLoneContinuationBf) {
    // Lone continuation byte 0xBF at start
    auto run = ssg::computeCellRun("\xBF");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
}

TEST(invalidOverlongC080) {
    // 0xC0 0x80: C0 is an invalid lead (overlong), then 0x80 is a lone continuation
    auto run = ssg::computeCellRun("\xC0\x80");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidOverlongC180) {
    // 0xC1 0x80: C1 is an invalid lead (overlong)
    auto run = ssg::computeCellRun("\xC1\x80");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidTwoFf) {
    auto run = ssg::computeCellRun("\xFF\xFF");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidTruncatedE4) {
    // 0xE4: 3-byte lead with no continuations → 1 invalid byte
    auto run = ssg::computeCellRun("\xE4");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
}

TEST(invalidTruncatedE4B8) {
    // 0xE4 0xB8: truncated 3-byte sequence (missing third byte)
    // 0xE4 → invalid lead (truncated); 0xB8 → standalone continuation → invalid
    auto run = ssg::computeCellRun("\xE4\xB8");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidTruncatedF09f) {
    // 0xF0 0x9F: truncated 4-byte sequence
    auto run = ssg::computeCellRun("\xF0\x9F");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidValidThenFf) {
    auto run = ssg::computeCellRun("A\xFF");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
}

TEST(invalidFfThenValid) {
    auto run = ssg::computeCellRun("\xFF" "A");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kT);
}

TEST(invalidBadContinuationE4B841) {
    // 0xE4 0xB8 0x41: 3-byte lead + valid first cont + non-continuation
    // 0xE4 fails (third byte 0x41 is not 0x80-0xBF) → 0xE4 is invalid
    // 0xB8 is a standalone continuation → invalid
    // 0x41 'A' is valid ASCII
    auto run = ssg::computeCellRun("\xE4\xB8\x41");
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
    CHECK_SPAN(run, 2, 2, 1, 1, kT);
}

TEST(invalidOverlongE08080) {
    // 0xE0 requires second byte >= 0xA0; 0x80 < 0xA0 → 0xE0 is invalid
    // Both continuations become standalone invalids
    auto run = ssg::computeCellRun("\xE0\x80\x80");
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
    CHECK_SPAN(run, 2, 2, 1, 1, kInv);
}

TEST(invalidSurrogateHigh) {
    // 0xED 0xA0 0x80 → U+D800 high surrogate; 0xED requires second byte <= 0x9F
    // 0xA0 > 0x9F → 0xED is invalid
    auto run = ssg::computeCellRun("\xED\xA0\x80");
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 3u);
    CHECK_SPAN(run, 0, 0, 1, 1, kInv);
    CHECK_SPAN(run, 1, 1, 1, 1, kInv);
    CHECK_SPAN(run, 2, 2, 1, 1, kInv);
}

// ---------------------------------------------------------------------------
// Hangul jamo composition fixtures (hangul.txt) — GB6/GB7/GB8

TEST(hangulLV) {
    // U+1100 ᄀ (E1 84 80) + U+1161 ᅡ (E1 85 A1) → 1 cluster via GB6 (L × V)
    // Base L jamo is wide (EAW=W) → cluster width = 2
    auto run = ssg::computeCellRun("\xE1\x84\x80\xE1\x85\xA1");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, kT);
}

TEST(hangulLVT) {
    // L (E1 84 80) + V (E1 85 A1) + T/U+11A8 (E1 86 88) → 1 cluster, GB6+GB7
    auto run = ssg::computeCellRun("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 9, 2, kT);
}

TEST(hangulLvT) {
    // U+AC00 가 (EA B0 80) + U+11A8 ᆨ (E1 86 88) → 1 cluster via GB7 (LV × T)
    // LV syllable is wide → cluster width = 2
    auto run = ssg::computeCellRun("\xEA\xB0\x80\xE1\x86\xA8");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, kT);
}

TEST(hangulLvtT) {
    // U+AC01 각 (EA B0 81) + U+11A8 ᆨ (E1 86 88) → 1 cluster via GB8 (LVT × T)
    auto run = ssg::computeCellRun("\xEA\xB0\x81\xE1\x86\xA8");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, kT);
}

TEST(hangulLExtendVNoCompose) {
    // Adversarial per UAX #29 GraphemeBreakTest.txt: ÷ 1100 × 0308 ÷ 1160 ÷
    // L + Extend (combining diaeresis U+0308, CC 88) + V (E1 85 A1)
    // GB6–GB8 have no Extend*: the Extend severs Hangul composition.
    // → 2 clusters: [L+Extend=5 bytes=2 cells] + [V=3 bytes=1 cell]
    auto run = ssg::computeCellRun("\xE1\x84\x80\xCC\x88\xE1\x85\xA1");
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 5, 2, kT);
    CHECK_SPAN(run, 1, 5, 3, 1, kT);
}

TEST(hangulLAsciiNoCompose) {
    // Adversarial: L jamo + ASCII 'A' must NOT compose (only L/V/LV/LVT follow L)
    // → 2 clusters: [L=2 cells] + [A=1 cell]
    auto run = ssg::computeCellRun("\xE1\x84\x80\x41");
    ASSERT_EQ(run.totalCells, 3u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
    CHECK_SPAN(run, 1, 3, 1, 1, kT);
}

TEST(hangulLvAlone) {
    // Standalone LV syllable 가 (U+AC00, EA B0 80): 1 cluster, 2 cells
    auto run = ssg::computeCellRun("\xEA\xB0\x80");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 2, kT);
}

// ---------------------------------------------------------------------------
// SpacingMark fixtures (spacing_mark.txt) — GB9a

TEST(spacingMarkDevanagariKaa) {
    // क (U+0915, E0 A4 95) + ā (U+093E, E0 A4 BE, SpacingMark)
    // GB9a: SpacingMark extends base → 1 cluster, 1 cell (narrow base)
    // Adversarial: without GB9a, would be 2 clusters (2 cells)
    auto run = ssg::computeCellRun("\xE0\xA4\x95\xE0\xA4\xBE");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, kT);
}

TEST(spacingMarkDevanagariKo) {
    // क (U+0915) + ो (U+094B, E0 A5 8B, SpacingMark) → को, 1 cluster, 1 cell
    auto run = ssg::computeCellRun("\xE0\xA4\x95\xE0\xA5\x8B");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, kT);
}

TEST(spacingMarkLone) {
    // Lone SpacingMark U+093E (E0 A4 BE) at line start → kind=combining, width=0
    auto run = ssg::computeCellRun("\xE0\xA4\xBE");
    ASSERT_EQ(run.totalCells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, kC);
}

TEST(spacingMarkBengaliKaa) {
    // ক (U+0995, E0 A6 95) + া (U+09BE, E0 A6 BE, SpacingMark) → কা, 1 cluster
    auto run = ssg::computeCellRun("\xE0\xA6\x95\xE0\xA6\xBE");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 1, kT);
}

// ---------------------------------------------------------------------------
// Prepend fixtures (prepend.txt) — GB9b

TEST(prepend0600Digit) {
    // U+0600 Arabic Number Sign (D8 80, Prepend) + '1' (31)
    // GB9b: Prepend absorbs '1'; cluster width = 1 (digit is narrow)
    auto run = ssg::computeCellRun("\xD8\x80\x31");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, kT);
}

TEST(prependAThenPrependDigit) {
    // Adversarial: 'a' then Prepend+digit → 2 clusters.
    // Prepend does NOT extend the preceding 'a' cluster.
    // 'a' = cluster 1 {0, 1, 1, T}; [U+0600 + '1'] = cluster 2 {1, 3, 1, T}
    auto run = ssg::computeCellRun("a\xD8\x80\x31");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
    CHECK_SPAN(run, 1, 1, 3, 1, kT);
}

TEST(prependLoneAtEol) {
    // Standalone Prepend U+0600 at end of line: no char to absorb → width=0
    auto run = ssg::computeCellRun("\xD8\x80");
    ASSERT_EQ(run.totalCells, 0u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, kC);
}

TEST(prependBeforeControl) {
    // Prepend (D8 80) before BEL (07): control is GCB-Control → not absorbed.
    // → Prepend cluster {0, 2, 0, C} + control cluster {2, 1, 1, CTL}
    auto run = ssg::computeCellRun("\xD8\x80\x07");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 2, 0, kC);
    CHECK_SPAN(run, 1, 2, 1, 1, kCtl);
}

// ---------------------------------------------------------------------------
// Additional edge cases

TEST(edgeTabThenCombining) {
    // '\t' (tab) is never extended; a combining mark after tab starts its own cluster
    // "\ta\xCC\x81": tab(4 cells) + a+combining_acute(1 cluster, 3 bytes, 1 cell)
    auto run = ssg::computeCellRun("\ta\xCC\x81", 4);
    ASSERT_EQ(run.totalCells, 5u);
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 1, 4, kTab);
    CHECK_SPAN(run, 1, 1, 3, 1, kT);
}

TEST(edgeValid3byteCjk) {
    // U+4E2D (中): full valid 3-byte sequence
    const std::string_view zhong = "\xE4\xB8\xAD";
    ASSERT_EQ(ssg::computeCellRun(zhong).totalCells, 2u);
}

TEST(edgeVariationSelector) {
    // U+0023 '#' + U+FE0F VS-16 (EF B8 8F, variation selector 16)
    // '#' is Emoji=Yes, Emoji_Presentation=No → alone=1 cell (text-default emoji)
    // VS-16 (GCB=Extend) is absorbed via GB9; saw_vs16=true triggers upgrade to 2
    // → 1 span, 4 bytes, 2 cells (emoji presentation sequence)
    auto run = ssg::computeCellRun("#\xEF\xB8\x8F");
    ASSERT_EQ(run.totalCells, 2u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
}

TEST(emojiScissorsAlone) {
    // U+2702 ✂ BLACK SCISSORS (E2 9C 82): EAW=N, Emoji=Yes, Emoji_Presentation=No
    // Text-default emoji; alone → 1 cell (was INCORRECTLY 2 in old k_wide[])
    auto run = ssg::computeCellRun("\xE2\x9C\x82");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 1, kT);
    ASSERT_EQ(run.totalCells, 1u);
}

TEST(emojiScissorsVs16) {
    // U+2702 ✂ + U+FE0F VS-16 (E2 9C 82 EF B8 8F) = 6 bytes
    // Emoji=Yes + VS-16 absorbed → upgrade to 2 cells
    auto run = ssg::computeCellRun("\xE2\x9C\x82\xEF\xB8\x8F");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 6, 2, kT);
    ASSERT_EQ(run.totalCells, 2u);
}

TEST(edgeSpaceIsPrintable) {
    // Space (U+0020) is printable ASCII, 1 cell, NOT a control character
    auto run = ssg::computeCellRun(" ");
    ASSERT_EQ(run.totalCells, 1u);
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 1, 1, kT);
}

// ---------------------------------------------------------------------------
// Adversarial fixtures — GB11 state machine bugs (must pass with new code)
// Bug 1: lone ZWJ base followed by ExtPic → was 1 cluster (ZWJ treated as ExtPic
//         base), should be 2 clusters.
// Bug 2: ExtPic + ZWJ + ZWJ + ExtPic → was 1 cluster (second ZWJ re-armed
//         after_zwj), should be 2 clusters (ZWJ ZWJ breaks Extend* pattern).

TEST(advLoneZwjBeforeExtpic) {
    // U+200D ZWJ (E2 80 8D) + U+1F600 GRINNING FACE (F0 9F 98 80)
    // ZWJ alone is not ExtPic; it does not satisfy the ExtPic precondition of GB11.
    // Expected: 2 clusters — [ZWJ] {0,3,0,C} and [😀] {3,4,2,T}
    auto run = ssg::computeCellRun("\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 3, 0, kC);  // ZWJ: combining, 0 cells
    CHECK_SPAN(run, 1, 3, 4, 2, kT);  // 😀: text, 2 cells
    ASSERT_EQ(run.totalCells, 2u);
}

TEST(advExtpicZwjZwjExtpic) {
    // U+1F600 (F0 9F 98 80) + ZWJ (E2 80 8D) + ZWJ (E2 80 8D) + U+1F600
    // Pattern: ExtPic ZWJ ZWJ ExtPic.  The second ZWJ has GCB=ZWJ (not Extend)
    // so it is NOT part of Extend*; GB11 requires ExtPic Extend* ZWJ × ExtPic.
    // Two ZWJs break the pattern → 2 clusters.
    // Cluster 1: 1F600 + ZWJ + ZWJ (11 bytes, 2 cells)
    // Cluster 2: 1F600 (4 bytes, 2 cells)
    auto run = ssg::computeCellRun(
        "\xF0\x9F\x98\x80\xE2\x80\x8D\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0,  10, 2, kT);  // 😀+ZWJ+ZWJ absorbed via GB9, no GB11
    CHECK_SPAN(run, 1, 10, 4,  2, kT);  // 😀 in its own cluster
    ASSERT_EQ(run.totalCells, 4u);
}

TEST(advSoftHyphenOwnCluster) {
    // U+00AD SOFT HYPHEN (C2 AD): GCB=Control (not Extend).
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::computeCellRun("\xC2\xAD");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 2, 0, kCtl);
    ASSERT_EQ(run.totalCells, 0u);
}

TEST(advZwspOwnCluster) {
    // U+200B ZERO WIDTH SPACE (E2 80 8B): GCB=Control (not Extend).
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::computeCellRun("\xE2\x80\x8B");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, kCtl);
    ASSERT_EQ(run.totalCells, 0u);
}

TEST(advThreeRegionalIndicators) {
    // 🇺 (U+1F1FA, F0 9F 87 BA) + 🇸 (U+1F1F8, F0 9F 87 B8) + 🇦 (U+1F1E6, F0 9F 87 A6)
    // GB12/13: first RI pair → cluster 1 [🇺🇸] (8 bytes, 2 cells)
    // Third RI starts a new cluster: cluster 2 [🇦] (4 bytes, 2 cells)
    auto run = ssg::computeCellRun(
        "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xA6");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 8, 2, kT);   // 🇺🇸 flag pair
    CHECK_SPAN(run, 1, 8, 4, 2, kT);   // 🇦 lone RI
    ASSERT_EQ(run.totalCells, 4u);
}

TEST(advExtpicZwjExtendNoGb11) {
    // U+1F600 + ZWJ + Emoji Modifier 🏻 (U+1F3FB, F0 9F 8F BB, GCB=Extend) + U+1F600
    // After ExtPic + ZWJ → gb11=Zwj; then Extend → gb11=None (Extend after Zwj breaks chain)
    // So the second 1F600 does NOT absorb via GB11 → 2 clusters
    // Cluster 1: 1F600 + ZWJ + 1F3FB (11 bytes, 2 cells)
    // Cluster 2: 1F600 (4 bytes, 2 cells)
    auto run = ssg::computeCellRun(
        "\xF0\x9F\x98\x80\xE2\x80\x8D\xF0\x9F\x8F\xBB\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0,  11, 2, kT);
    CHECK_SPAN(run, 1, 11, 4,  2, kT);
    ASSERT_EQ(run.totalCells, 4u);
}

TEST(advExtpicExtendZwjExtpicGb11) {
    // U+1F600 + Variation Selector VS-16 (U+FE0F, GCB=Extend) + ZWJ + U+1F600
    // ExtPic Extend ZWJ ExtPic → all one cluster via GB11
    // F0 9F 98 80  EF B8 8F  E2 80 8D  F0 9F 98 80  = 15 bytes
    auto run = ssg::computeCellRun(
        "\xF0\x9F\x98\x80\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x98\x80");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 14, 2, kT);  // 4+3+3+4 bytes
    ASSERT_EQ(run.totalCells, 2u);
}

TEST(advLoneEmojiModifier) {
    // U+1F3FB EMOJI MODIFIER FITZPATRICK TYPE-1-2 (F0 9F 8F BB)
    // GCB=Extend, EAW=W (in k_wide range 1F3F7–1F4FD).
    // NOT Extended_Pictographic; when standalone (no preceding base) it renders
    // as a 2-cell wide glyph — base_width=2, kind=text (wide lone Extend).
    auto run = ssg::computeCellRun("\xF0\x9F\x8F\xBB");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 4, 2, kT);
    ASSERT_EQ(run.totalCells, 2u);
}

TEST(advBidiControlOwnCluster) {
    // U+202A LEFT-TO-RIGHT EMBEDDING (E2 80 AA): GCB=Control.
    // Non-C0/C1 Cf format control → own cluster with kind=control, width=0.
    auto run = ssg::computeCellRun("\xE2\x80\xAA");
    ASSERT_EQ(run.spans.size(), 1u);
    CHECK_SPAN(run, 0, 0, 3, 0, kCtl);
    ASSERT_EQ(run.totalCells, 0u);
}

TEST(advPrependExtendBreaksGb9b) {
    // U+0600 Prepend + U+0308 Extend + 'A'
    // (0600, 0308): GB9 absorbs Extend → last_gcb=Other (no longer Prepend)
    // (Other/Extend, A): no applicable rule → break
    // Expected: 2 clusters — [0600+0308] {0,4,0,T}, ['A'] {4,1,1,T}
    // D8 80 = U+0600, CC 88 = U+0308, 41 = 'A'
    auto run = ssg::computeCellRun("\xD8\x80\xCC\x88\x41");
    ASSERT_EQ(run.spans.size(), 2u);
    CHECK_SPAN(run, 0, 0, 4, 0, kC);  // Prepend+Extend: no visible base absorbed
    CHECK_SPAN(run, 1, 4, 1, 1, kT);  // 'A': separate cluster
    ASSERT_EQ(run.totalCells, 1u);
}

// ---------------------------------------------------------------------------

int main() {

    // ASCII
    RUN(asciiEmpty);
    RUN(asciiSingleSpace);
    RUN(asciiHello);
    RUN(asciiTildeBoundary);

    // Combining
    RUN(combiningLatinAAcute);
    RUN(combiningLatinEMacron);
    RUN(combiningLatinATwoCombining);
    RUN(combiningLoneAcute);
    RUN(combiningTwoLone);
    RUN(combiningNTilde);
    RUN(combiningWideBase);

    // Double-width
    RUN(doubleWidthCjkZhong);
    RUN(doubleWidthFullwidthA);
    RUN(doubleWidthHangulGa);
    RUN(doubleWidthTwoCjk);
    RUN(doubleWidthMixedNarrowWide);
    RUN(doubleWidthHiraganaA);
    RUN(doubleWidthFullwidthBang);

    // Emoji
    RUN(emojiGrinningFace);
    RUN(emojiSlightSmile);
    RUN(emojiManStandalone);
    RUN(emojiManZwjWoman);
    RUN(emojiUsFlag);
    RUN(emojiThenAscii);
    RUN(emojiManZwjFullwidthA);
    RUN(emojiThumbsSkinTone);
    RUN(emojiTwo);

    // Tab
    RUN(tabCol0W4);
    RUN(tabCol0W8);
    RUN(tabCol0W1);
    RUN(tabCol0W2);
    RUN(tabAbTabW4);
    RUN(tabAbcdTabW4);
    RUN(tabATabBW4);
    RUN(tabTwoTabsW4);
    RUN(tabWidthInvalidZero);
    RUN(tabWidthInvalidNegative);
    RUN(tabWidthInvalidTooLarge);

    // Control
    RUN(controlNul);
    RUN(controlSoh);
    RUN(controlBel);
    RUN(controlLf);
    RUN(controlEsc);
    RUN(controlUs);
    RUN(controlDel);
    RUN(controlC1Pad);
    RUN(controlC1Apc);
    RUN(controlTwoControls);
    RUN(controlCtlThenText);
    RUN(controlTextThenCtl);

    // Invalid UTF-8
    RUN(invalidLoneFf);
    RUN(invalidLoneFe);
    RUN(invalidLoneContinuation80);
    RUN(invalidLoneContinuationBf);
    RUN(invalidOverlongC080);
    RUN(invalidOverlongC180);
    RUN(invalidTwoFf);
    RUN(invalidTruncatedE4);
    RUN(invalidTruncatedE4B8);
    RUN(invalidTruncatedF09f);
    RUN(invalidValidThenFf);
    RUN(invalidFfThenValid);
    RUN(invalidBadContinuationE4B841);
    RUN(invalidOverlongE08080);
    RUN(invalidSurrogateHigh);

    // Hangul
    RUN(hangulLV);
    RUN(hangulLVT);
    RUN(hangulLvT);
    RUN(hangulLvtT);
    RUN(hangulLExtendVNoCompose);
    RUN(hangulLAsciiNoCompose);
    RUN(hangulLvAlone);

    // SpacingMark (GB9a)
    RUN(spacingMarkDevanagariKaa);
    RUN(spacingMarkDevanagariKo);
    RUN(spacingMarkLone);
    RUN(spacingMarkBengaliKaa);

    // Prepend (GB9b)
    RUN(prepend0600Digit);
    RUN(prependAThenPrependDigit);
    RUN(prependLoneAtEol);
    RUN(prependBeforeControl);

    // Adversarial fixtures — GB11, GCB=Control chars, RI, Prepend+Extend
    RUN(advLoneZwjBeforeExtpic);
    RUN(advExtpicZwjZwjExtpic);
    RUN(advSoftHyphenOwnCluster);
    RUN(advZwspOwnCluster);
    RUN(advThreeRegionalIndicators);
    RUN(advExtpicZwjExtendNoGb11);
    RUN(advExtpicExtendZwjExtpicGb11);
    RUN(advLoneEmojiModifier);
    RUN(advBidiControlOwnCluster);
    RUN(advPrependExtendBreaksGb9b);

    // Edge cases
    RUN(edgeTabThenCombining);
    RUN(edgeValid3byteCjk);
    RUN(edgeVariationSelector);
    RUN(emojiScissorsAlone);
    RUN(emojiScissorsVs16);
    RUN(edgeSpaceIsPrintable);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
