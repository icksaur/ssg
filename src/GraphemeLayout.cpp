// UTF-8 grapheme segmentation and terminal cell layout.
//
// See include/ssg/GraphemeLayout.h for the public contract. Cell-width rules
// follow UAX #11; tests/test_cell_layout.cpp is the oracle.
//
// Unicode version: 15.0.0 (released 2022-09-13).
// Grapheme clusters: UAX #29 extended grapheme clusters.
//   All rules applicable under the single-logical-line precondition. CR/LF
//   cases are excluded; GB4/GB5 still enforce boundaries for other Controls:
//     GB6    — L × (L|V|LV|LVT)                      [Hangul leading jamo]
//     GB7    — (LV|V) × (V|T)                        [Hangul vowel/syllable]
//     GB8    — (LVT|T) × T                           [Hangul trailing jamo]
//     GB9    — × (Extend | ZWJ)                      [combining marks, ZWJ]
//     GB9a   — × SpacingMark                         [Indic/script spacing marks]
//     GB9b   — Prepend ×                             [Prepend chars absorb next]
//     GB11   — ExtPic Extend* ZWJ × ExtPic           [emoji ZWJ sequences]
//     GB12/13 — RI × RI                              [regional-indicator flag pairs]
// Width:  UAX #11 East Asian Width (W and F → 2 cells) + Emoji_Presentation
//         (→ 2 cells) + Emoji + VS-16 sequences (→ 2 cells).
//
// Data sources (all Unicode 15.0.0):
//   k_gcb       — GraphemeBreakProperty.txt (full GCB table; LV/LVT computed inline)
//   k_extpic    — emoji-data.txt (Extended_Pictographic; exact merged ranges)
//   k_wide      — EastAsianWidth.txt (EAW=W or F exactly; 121 merged ranges)
//   k_emoji_pres — emoji-data.txt (Emoji_Presentation; 81 ranges)
//   k_emoji     — emoji-data.txt (Emoji property; 151 ranges, for VS-16 sequences)
//
// Width rules (applied to the effective base code point of each cluster):
//   EAW=W or F (is_eaw_wide)             → 2 cells
//   Emoji_Presentation=Yes (is_emoji_pres) → 2 cells
//   Emoji + VS-16 absorbed (is_emoji + saw_vs16) → 2 cells (emoji presentation seq)
//   All other printable code points        → 1 cell

#include <ssg/GraphemeLayout.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ssg {

namespace {
thread_local std::uint64_t gCellRunCalls = 0;
}

struct URange {
    uint32_t lo;
    uint32_t hi;
};

// GCB property classification — used by gcb_prop_of().
// Other: default for code points not listed in k_gcb (most printable text).
enum class GcbProp : uint8_t {
    Other, CR, LF, Control, Extend, ZWJ, SpacingMark,
    L, V, T, LV, LVT, Prepend, RI
};

struct GcbRange {
    uint32_t lo;
    uint32_t hi;
    GcbProp prop;
};

// Requires ranges to be sorted by lo and non-overlapping.
static bool inRanges(const URange* ranges, int n, uint32_t cp) noexcept {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cp < ranges[mid].lo) hi = mid - 1;
        else if (cp > ranges[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

// Requires ranges sorted by lo and non-overlapping; returns Other when cp is in none.
static GcbProp gcbLookup(const GcbRange* ranges, int n, uint32_t cp) noexcept {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cp < ranges[mid].lo) hi = mid - 1;
        else if (cp > ranges[mid].hi) lo = mid + 1;
        else return ranges[mid].prop;
    }
    return GcbProp::Other;
}

// Unicode 15.0.0 — EAW=W or EAW=F code points
//
// Source: EastAsianWidth.txt, Unicode 15.0.0 (2022-09-13).
// SHA-256: 743e7bc435c04ab1a8459710b1c3cad56eedced5b806b4659b6e69b85d0adf2a
// Regenerate with: python3 tools/gen_eaw_table.py data/unicode/east_asian_width.txt
// 121 ranges, 182516 code points.

static constexpr URange kWide[] = {
    {0x1100, 0x115F},
    {0x231A, 0x231B},
    {0x2329, 0x232A},
    {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},
    {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},
    {0x2614, 0x2615},
    {0x2648, 0x2653},
    {0x267F, 0x267F},
    {0x2693, 0x2693},
    {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},
    {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},
    {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},
    {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},
    {0x26FD, 0x26FD},
    {0x2705, 0x2705},
    {0x270A, 0x270B},
    {0x2728, 0x2728},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2795, 0x2797},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x2E80, 0x2E99},
    {0x2E9B, 0x2EF3},
    {0x2F00, 0x2FD5},
    {0x2FF0, 0x2FFB},
    {0x3000, 0x303E},
    {0x3041, 0x3096},
    {0x3099, 0x30FF},
    {0x3105, 0x312F},
    {0x3131, 0x318E},
    {0x3190, 0x31E3},
    {0x31F0, 0x321E},
    {0x3220, 0x3247},
    {0x3250, 0x4DBF},
    {0x4E00, 0xA48C},
    {0xA490, 0xA4C6},
    {0xA960, 0xA97C},
    {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},
    {0xFE10, 0xFE19},
    {0xFE30, 0xFE52},
    {0xFE54, 0xFE66},
    {0xFE68, 0xFE6B},
    {0xFF01, 0xFF60},
    {0xFFE0, 0xFFE6},
    {0x16FE0, 0x16FE4},
    {0x16FF0, 0x16FF1},
    {0x17000, 0x187F7},
    {0x18800, 0x18CD5},
    {0x18D00, 0x18D08},
    {0x1AFF0, 0x1AFF3},
    {0x1AFF5, 0x1AFFB},
    {0x1AFFD, 0x1AFFE},
    {0x1B000, 0x1B122},
    {0x1B132, 0x1B132},
    {0x1B150, 0x1B152},
    {0x1B155, 0x1B155},
    {0x1B164, 0x1B167},
    {0x1B170, 0x1B2FB},
    {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F200, 0x1F202},
    {0x1F210, 0x1F23B},
    {0x1F240, 0x1F248},
    {0x1F250, 0x1F251},
    {0x1F260, 0x1F265},
    {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6DF},
    {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF},
    {0x1FA70, 0x1FA7C},
    {0x1FA80, 0x1FA88},
    {0x1FA90, 0x1FABD},
    {0x1FABF, 0x1FAC5},
    {0x1FACE, 0x1FADB},
    {0x1FAE0, 0x1FAE8},
    {0x1FAF0, 0x1FAF8},
    {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD},
};

static constexpr int kWideN =
    static_cast<int>(sizeof(kWide) / sizeof(kWide[0]));

// Unicode 15.0.0 — Extended_Pictographic code points
//
// Used by GB11: ExtPic Extend* ZWJ × ExtPic.  Only code points with this
// property may continue an emoji ZWJ sequence.  Wide CJK characters are NOT
// Extended_Pictographic and must NOT be joined merely because they are wide.
// Source: emoji-data.txt, Unicode 15.0.0 (2022-09-13).
// https://unicode.org/Public/15.0.0/ucd/emoji/emoji-data.txt
// Exact merged ranges: 78 ranges covering 3537 code points.
// Regenerate with: python3 tools/gen_extpic_table.py data/unicode/emoji-data.txt

static constexpr URange kExtpic[] = {
    {0x00A9, 0x00A9},
    {0x00AE, 0x00AE},
    {0x203C, 0x203C},
    {0x2049, 0x2049},
    {0x2122, 0x2122},
    {0x2139, 0x2139},
    {0x2194, 0x2199},
    {0x21A9, 0x21AA},
    {0x231A, 0x231B},
    {0x2328, 0x2328},
    {0x2388, 0x2388},
    {0x23CF, 0x23CF},
    {0x23E9, 0x23F3},
    {0x23F8, 0x23FA},
    {0x24C2, 0x24C2},
    {0x25AA, 0x25AB},
    {0x25B6, 0x25B6},
    {0x25C0, 0x25C0},
    {0x25FB, 0x25FE},
    {0x2600, 0x2605},
    {0x2607, 0x2612},
    {0x2614, 0x2685},
    {0x2690, 0x2705},
    {0x2708, 0x2712},
    {0x2714, 0x2714},
    {0x2716, 0x2716},
    {0x271D, 0x271D},
    {0x2721, 0x2721},
    {0x2728, 0x2728},
    {0x2733, 0x2734},
    {0x2744, 0x2744},
    {0x2747, 0x2747},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2763, 0x2767},
    {0x2795, 0x2797},
    {0x27A1, 0x27A1},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2934, 0x2935},
    {0x2B05, 0x2B07},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x3030, 0x3030},
    {0x303D, 0x303D},
    {0x3297, 0x3297},
    {0x3299, 0x3299},
    {0x1F000, 0x1F0FF},
    {0x1F10D, 0x1F10F},
    {0x1F12F, 0x1F12F},
    {0x1F16C, 0x1F171},
    {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F1AD, 0x1F1E5},
    {0x1F201, 0x1F20F},
    {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A},
    {0x1F23C, 0x1F23F},
    {0x1F249, 0x1F3FA},
    {0x1F400, 0x1F53D},
    {0x1F546, 0x1F64F},
    {0x1F680, 0x1F6FF},
    {0x1F774, 0x1F77F},
    {0x1F7D5, 0x1F7FF},
    {0x1F80C, 0x1F80F},
    {0x1F848, 0x1F84F},
    {0x1F85A, 0x1F85F},
    {0x1F888, 0x1F88F},
    {0x1F8AE, 0x1F8FF},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1FAFF},
    {0x1FC00, 0x1FFFD},
};

static constexpr int kExtpicN =
    static_cast<int>(sizeof(kExtpic) / sizeof(kExtpic[0]));

// Unicode 15.0.0 — Emoji_Presentation code points
//
// Characters that default to emoji presentation and occupy 2 terminal columns.
// Covers the 26 Regional Indicator Symbols (U+1F1E6..U+1F1FF) not in k_wide[],
// plus all other Emoji_Presentation characters that are also EAW=W/F (kept here
// for completeness; is_emoji_pres() is checked alongside is_eaw_wide()).
// Source: emoji-data.txt, Unicode 15.0.0 (2022-09-13).
// SHA-256: 29071dba22c72c27783a73016afb8ffaeb025866740791f9c2d0b55cc45a3470
// Regenerate with: python3 tools/gen_emoji_props_table.py data/unicode/emoji-data.txt

static constexpr URange kEmojiPres[] = {
    {0x231A, 0x231B},
    {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},
    {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},
    {0x2614, 0x2615},
    {0x2648, 0x2653},
    {0x267F, 0x267F},
    {0x2693, 0x2693},
    {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},
    {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},
    {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},
    {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},
    {0x26FD, 0x26FD},
    {0x2705, 0x2705},
    {0x270A, 0x270B},
    {0x2728, 0x2728},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2795, 0x2797},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F1E6, 0x1F1FF},
    {0x1F201, 0x1F201},
    {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F236},
    {0x1F238, 0x1F23A},
    {0x1F250, 0x1F251},
    {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6DF},
    {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF},
    {0x1FA70, 0x1FA7C},
    {0x1FA80, 0x1FA88},
    {0x1FA90, 0x1FABD},
    {0x1FABF, 0x1FAC5},
    {0x1FACE, 0x1FADB},
    {0x1FAE0, 0x1FAE8},
    {0x1FAF0, 0x1FAF8},
};

static constexpr int kEmojiPresN =
    static_cast<int>(sizeof(kEmojiPres) / sizeof(kEmojiPres[0]));

// Unicode 15.0.0 — Emoji code points
//
// Characters with the Emoji property.  Used to detect emoji presentation
// sequences: an Emoji character followed by U+FE0F (VS-16) displays in emoji
// presentation (2 terminal columns) regardless of the base's default width.
// Source: emoji-data.txt, Unicode 15.0.0 (2022-09-13).
// Regenerate with: python3 tools/gen_emoji_props_table.py data/unicode/emoji-data.txt

static constexpr URange kEmoji[] = {
    {0x0023, 0x0023},
    {0x002A, 0x002A},
    {0x0030, 0x0039},
    {0x00A9, 0x00A9},
    {0x00AE, 0x00AE},
    {0x203C, 0x203C},
    {0x2049, 0x2049},
    {0x2122, 0x2122},
    {0x2139, 0x2139},
    {0x2194, 0x2199},
    {0x21A9, 0x21AA},
    {0x231A, 0x231B},
    {0x2328, 0x2328},
    {0x23CF, 0x23CF},
    {0x23E9, 0x23F3},
    {0x23F8, 0x23FA},
    {0x24C2, 0x24C2},
    {0x25AA, 0x25AB},
    {0x25B6, 0x25B6},
    {0x25C0, 0x25C0},
    {0x25FB, 0x25FE},
    {0x2600, 0x2604},
    {0x260E, 0x260E},
    {0x2611, 0x2611},
    {0x2614, 0x2615},
    {0x2618, 0x2618},
    {0x261D, 0x261D},
    {0x2620, 0x2620},
    {0x2622, 0x2623},
    {0x2626, 0x2626},
    {0x262A, 0x262A},
    {0x262E, 0x262F},
    {0x2638, 0x263A},
    {0x2640, 0x2640},
    {0x2642, 0x2642},
    {0x2648, 0x2653},
    {0x265F, 0x2660},
    {0x2663, 0x2663},
    {0x2665, 0x2666},
    {0x2668, 0x2668},
    {0x267B, 0x267B},
    {0x267E, 0x267F},
    {0x2692, 0x2697},
    {0x2699, 0x2699},
    {0x269B, 0x269C},
    {0x26A0, 0x26A1},
    {0x26A7, 0x26A7},
    {0x26AA, 0x26AB},
    {0x26B0, 0x26B1},
    {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},
    {0x26C8, 0x26C8},
    {0x26CE, 0x26CF},
    {0x26D1, 0x26D1},
    {0x26D3, 0x26D4},
    {0x26E9, 0x26EA},
    {0x26F0, 0x26F5},
    {0x26F7, 0x26FA},
    {0x26FD, 0x26FD},
    {0x2702, 0x2702},
    {0x2705, 0x2705},
    {0x2708, 0x270D},
    {0x270F, 0x270F},
    {0x2712, 0x2712},
    {0x2714, 0x2714},
    {0x2716, 0x2716},
    {0x271D, 0x271D},
    {0x2721, 0x2721},
    {0x2728, 0x2728},
    {0x2733, 0x2734},
    {0x2744, 0x2744},
    {0x2747, 0x2747},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2763, 0x2764},
    {0x2795, 0x2797},
    {0x27A1, 0x27A1},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2934, 0x2935},
    {0x2B05, 0x2B07},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x3030, 0x3030},
    {0x303D, 0x303D},
    {0x3297, 0x3297},
    {0x3299, 0x3299},
    {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF},
    {0x1F170, 0x1F171},
    {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F1E6, 0x1F1FF},
    {0x1F201, 0x1F202},
    {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A},
    {0x1F250, 0x1F251},
    {0x1F300, 0x1F321},
    {0x1F324, 0x1F393},
    {0x1F396, 0x1F397},
    {0x1F399, 0x1F39B},
    {0x1F39E, 0x1F3F0},
    {0x1F3F3, 0x1F3F5},
    {0x1F3F7, 0x1F4FD},
    {0x1F4FF, 0x1F53D},
    {0x1F549, 0x1F54E},
    {0x1F550, 0x1F567},
    {0x1F56F, 0x1F570},
    {0x1F573, 0x1F57A},
    {0x1F587, 0x1F587},
    {0x1F58A, 0x1F58D},
    {0x1F590, 0x1F590},
    {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A5},
    {0x1F5A8, 0x1F5A8},
    {0x1F5B1, 0x1F5B2},
    {0x1F5BC, 0x1F5BC},
    {0x1F5C2, 0x1F5C4},
    {0x1F5D1, 0x1F5D3},
    {0x1F5DC, 0x1F5DE},
    {0x1F5E1, 0x1F5E1},
    {0x1F5E3, 0x1F5E3},
    {0x1F5E8, 0x1F5E8},
    {0x1F5EF, 0x1F5EF},
    {0x1F5F3, 0x1F5F3},
    {0x1F5FA, 0x1F64F},
    {0x1F680, 0x1F6C5},
    {0x1F6CB, 0x1F6D2},
    {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6E5},
    {0x1F6E9, 0x1F6E9},
    {0x1F6EB, 0x1F6EC},
    {0x1F6F0, 0x1F6F0},
    {0x1F6F3, 0x1F6FC},
    {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF},
    {0x1FA70, 0x1FA7C},
    {0x1FA80, 0x1FA88},
    {0x1FA90, 0x1FABD},
    {0x1FABF, 0x1FAC5},
    {0x1FACE, 0x1FADB},
    {0x1FAE0, 0x1FAE8},
    {0x1FAF0, 0x1FAF8},
};

static constexpr int kEmojiN =
    static_cast<int>(sizeof(kEmoji) / sizeof(kEmoji[0]));

// Unicode 15.0.0 — GCB property table
//
// Source: GraphemeBreakProperty.txt, Unicode 15.0.0 (2022-09-13).
// https://unicode.org/Public/15.0.0/ucd/auxiliary/GraphemeBreakProperty.txt
// LV and LVT Hangul syllable entries are excluded (computed in gcb_prop_of).
// Generated by tools/gen_gcb_table.py.

static constexpr GcbRange kGcb[] = {
    {0x0000, 0x0009, GcbProp::Control},
    {0x000A, 0x000A, GcbProp::LF},
    {0x000B, 0x000C, GcbProp::Control},
    {0x000D, 0x000D, GcbProp::CR},
    {0x000E, 0x001F, GcbProp::Control},
    {0x007F, 0x009F, GcbProp::Control},
    {0x00AD, 0x00AD, GcbProp::Control},
    {0x0300, 0x036F, GcbProp::Extend},
    {0x0483, 0x0487, GcbProp::Extend},
    {0x0488, 0x0489, GcbProp::Extend},
    {0x0591, 0x05BD, GcbProp::Extend},
    {0x05BF, 0x05BF, GcbProp::Extend},
    {0x05C1, 0x05C2, GcbProp::Extend},
    {0x05C4, 0x05C5, GcbProp::Extend},
    {0x05C7, 0x05C7, GcbProp::Extend},
    {0x0600, 0x0605, GcbProp::Prepend},
    {0x0610, 0x061A, GcbProp::Extend},
    {0x061C, 0x061C, GcbProp::Control},
    {0x064B, 0x065F, GcbProp::Extend},
    {0x0670, 0x0670, GcbProp::Extend},
    {0x06D6, 0x06DC, GcbProp::Extend},
    {0x06DD, 0x06DD, GcbProp::Prepend},
    {0x06DF, 0x06E4, GcbProp::Extend},
    {0x06E7, 0x06E8, GcbProp::Extend},
    {0x06EA, 0x06ED, GcbProp::Extend},
    {0x070F, 0x070F, GcbProp::Prepend},
    {0x0711, 0x0711, GcbProp::Extend},
    {0x0730, 0x074A, GcbProp::Extend},
    {0x07A6, 0x07B0, GcbProp::Extend},
    {0x07EB, 0x07F3, GcbProp::Extend},
    {0x07FD, 0x07FD, GcbProp::Extend},
    {0x0816, 0x0819, GcbProp::Extend},
    {0x081B, 0x0823, GcbProp::Extend},
    {0x0825, 0x0827, GcbProp::Extend},
    {0x0829, 0x082D, GcbProp::Extend},
    {0x0859, 0x085B, GcbProp::Extend},
    {0x0890, 0x0891, GcbProp::Prepend},
    {0x0898, 0x089F, GcbProp::Extend},
    {0x08CA, 0x08E1, GcbProp::Extend},
    {0x08E2, 0x08E2, GcbProp::Prepend},
    {0x08E3, 0x0902, GcbProp::Extend},
    {0x0903, 0x0903, GcbProp::SpacingMark},
    {0x093A, 0x093A, GcbProp::Extend},
    {0x093B, 0x093B, GcbProp::SpacingMark},
    {0x093C, 0x093C, GcbProp::Extend},
    {0x093E, 0x0940, GcbProp::SpacingMark},
    {0x0941, 0x0948, GcbProp::Extend},
    {0x0949, 0x094C, GcbProp::SpacingMark},
    {0x094D, 0x094D, GcbProp::Extend},
    {0x094E, 0x094F, GcbProp::SpacingMark},
    {0x0951, 0x0957, GcbProp::Extend},
    {0x0962, 0x0963, GcbProp::Extend},
    {0x0981, 0x0981, GcbProp::Extend},
    {0x0982, 0x0983, GcbProp::SpacingMark},
    {0x09BC, 0x09BC, GcbProp::Extend},
    {0x09BE, 0x09BE, GcbProp::Extend},
    {0x09BF, 0x09C0, GcbProp::SpacingMark},
    {0x09C1, 0x09C4, GcbProp::Extend},
    {0x09C7, 0x09C8, GcbProp::SpacingMark},
    {0x09CB, 0x09CC, GcbProp::SpacingMark},
    {0x09CD, 0x09CD, GcbProp::Extend},
    {0x09D7, 0x09D7, GcbProp::Extend},
    {0x09E2, 0x09E3, GcbProp::Extend},
    {0x09FE, 0x09FE, GcbProp::Extend},
    {0x0A01, 0x0A02, GcbProp::Extend},
    {0x0A03, 0x0A03, GcbProp::SpacingMark},
    {0x0A3C, 0x0A3C, GcbProp::Extend},
    {0x0A3E, 0x0A40, GcbProp::SpacingMark},
    {0x0A41, 0x0A42, GcbProp::Extend},
    {0x0A47, 0x0A48, GcbProp::Extend},
    {0x0A4B, 0x0A4D, GcbProp::Extend},
    {0x0A51, 0x0A51, GcbProp::Extend},
    {0x0A70, 0x0A71, GcbProp::Extend},
    {0x0A75, 0x0A75, GcbProp::Extend},
    {0x0A81, 0x0A82, GcbProp::Extend},
    {0x0A83, 0x0A83, GcbProp::SpacingMark},
    {0x0ABC, 0x0ABC, GcbProp::Extend},
    {0x0ABE, 0x0AC0, GcbProp::SpacingMark},
    {0x0AC1, 0x0AC5, GcbProp::Extend},
    {0x0AC7, 0x0AC8, GcbProp::Extend},
    {0x0AC9, 0x0AC9, GcbProp::SpacingMark},
    {0x0ACB, 0x0ACC, GcbProp::SpacingMark},
    {0x0ACD, 0x0ACD, GcbProp::Extend},
    {0x0AE2, 0x0AE3, GcbProp::Extend},
    {0x0AFA, 0x0AFF, GcbProp::Extend},
    {0x0B01, 0x0B01, GcbProp::Extend},
    {0x0B02, 0x0B03, GcbProp::SpacingMark},
    {0x0B3C, 0x0B3C, GcbProp::Extend},
    {0x0B3E, 0x0B3E, GcbProp::Extend},
    {0x0B3F, 0x0B3F, GcbProp::Extend},
    {0x0B40, 0x0B40, GcbProp::SpacingMark},
    {0x0B41, 0x0B44, GcbProp::Extend},
    {0x0B47, 0x0B48, GcbProp::SpacingMark},
    {0x0B4B, 0x0B4C, GcbProp::SpacingMark},
    {0x0B4D, 0x0B4D, GcbProp::Extend},
    {0x0B55, 0x0B56, GcbProp::Extend},
    {0x0B57, 0x0B57, GcbProp::Extend},
    {0x0B62, 0x0B63, GcbProp::Extend},
    {0x0B82, 0x0B82, GcbProp::Extend},
    {0x0BBE, 0x0BBE, GcbProp::Extend},
    {0x0BBF, 0x0BBF, GcbProp::SpacingMark},
    {0x0BC0, 0x0BC0, GcbProp::Extend},
    {0x0BC1, 0x0BC2, GcbProp::SpacingMark},
    {0x0BC6, 0x0BC8, GcbProp::SpacingMark},
    {0x0BCA, 0x0BCC, GcbProp::SpacingMark},
    {0x0BCD, 0x0BCD, GcbProp::Extend},
    {0x0BD7, 0x0BD7, GcbProp::Extend},
    {0x0C00, 0x0C00, GcbProp::Extend},
    {0x0C01, 0x0C03, GcbProp::SpacingMark},
    {0x0C04, 0x0C04, GcbProp::Extend},
    {0x0C3C, 0x0C3C, GcbProp::Extend},
    {0x0C3E, 0x0C40, GcbProp::Extend},
    {0x0C41, 0x0C44, GcbProp::SpacingMark},
    {0x0C46, 0x0C48, GcbProp::Extend},
    {0x0C4A, 0x0C4D, GcbProp::Extend},
    {0x0C55, 0x0C56, GcbProp::Extend},
    {0x0C62, 0x0C63, GcbProp::Extend},
    {0x0C81, 0x0C81, GcbProp::Extend},
    {0x0C82, 0x0C83, GcbProp::SpacingMark},
    {0x0CBC, 0x0CBC, GcbProp::Extend},
    {0x0CBE, 0x0CBE, GcbProp::SpacingMark},
    {0x0CBF, 0x0CBF, GcbProp::Extend},
    {0x0CC0, 0x0CC1, GcbProp::SpacingMark},
    {0x0CC2, 0x0CC2, GcbProp::Extend},
    {0x0CC3, 0x0CC4, GcbProp::SpacingMark},
    {0x0CC6, 0x0CC6, GcbProp::Extend},
    {0x0CC7, 0x0CC8, GcbProp::SpacingMark},
    {0x0CCA, 0x0CCB, GcbProp::SpacingMark},
    {0x0CCC, 0x0CCD, GcbProp::Extend},
    {0x0CD5, 0x0CD6, GcbProp::Extend},
    {0x0CE2, 0x0CE3, GcbProp::Extend},
    {0x0CF3, 0x0CF3, GcbProp::SpacingMark},
    {0x0D00, 0x0D01, GcbProp::Extend},
    {0x0D02, 0x0D03, GcbProp::SpacingMark},
    {0x0D3B, 0x0D3C, GcbProp::Extend},
    {0x0D3E, 0x0D3E, GcbProp::Extend},
    {0x0D3F, 0x0D40, GcbProp::SpacingMark},
    {0x0D41, 0x0D44, GcbProp::Extend},
    {0x0D46, 0x0D48, GcbProp::SpacingMark},
    {0x0D4A, 0x0D4C, GcbProp::SpacingMark},
    {0x0D4D, 0x0D4D, GcbProp::Extend},
    {0x0D4E, 0x0D4E, GcbProp::Prepend},
    {0x0D57, 0x0D57, GcbProp::Extend},
    {0x0D62, 0x0D63, GcbProp::Extend},
    {0x0D81, 0x0D81, GcbProp::Extend},
    {0x0D82, 0x0D83, GcbProp::SpacingMark},
    {0x0DCA, 0x0DCA, GcbProp::Extend},
    {0x0DCF, 0x0DCF, GcbProp::Extend},
    {0x0DD0, 0x0DD1, GcbProp::SpacingMark},
    {0x0DD2, 0x0DD4, GcbProp::Extend},
    {0x0DD6, 0x0DD6, GcbProp::Extend},
    {0x0DD8, 0x0DDE, GcbProp::SpacingMark},
    {0x0DDF, 0x0DDF, GcbProp::Extend},
    {0x0DF2, 0x0DF3, GcbProp::SpacingMark},
    {0x0E31, 0x0E31, GcbProp::Extend},
    {0x0E33, 0x0E33, GcbProp::SpacingMark},
    {0x0E34, 0x0E3A, GcbProp::Extend},
    {0x0E47, 0x0E4E, GcbProp::Extend},
    {0x0EB1, 0x0EB1, GcbProp::Extend},
    {0x0EB3, 0x0EB3, GcbProp::SpacingMark},
    {0x0EB4, 0x0EBC, GcbProp::Extend},
    {0x0EC8, 0x0ECE, GcbProp::Extend},
    {0x0F18, 0x0F19, GcbProp::Extend},
    {0x0F35, 0x0F35, GcbProp::Extend},
    {0x0F37, 0x0F37, GcbProp::Extend},
    {0x0F39, 0x0F39, GcbProp::Extend},
    {0x0F3E, 0x0F3F, GcbProp::SpacingMark},
    {0x0F71, 0x0F7E, GcbProp::Extend},
    {0x0F7F, 0x0F7F, GcbProp::SpacingMark},
    {0x0F80, 0x0F84, GcbProp::Extend},
    {0x0F86, 0x0F87, GcbProp::Extend},
    {0x0F8D, 0x0F97, GcbProp::Extend},
    {0x0F99, 0x0FBC, GcbProp::Extend},
    {0x0FC6, 0x0FC6, GcbProp::Extend},
    {0x102D, 0x1030, GcbProp::Extend},
    {0x1031, 0x1031, GcbProp::SpacingMark},
    {0x1032, 0x1037, GcbProp::Extend},
    {0x1039, 0x103A, GcbProp::Extend},
    {0x103B, 0x103C, GcbProp::SpacingMark},
    {0x103D, 0x103E, GcbProp::Extend},
    {0x1056, 0x1057, GcbProp::SpacingMark},
    {0x1058, 0x1059, GcbProp::Extend},
    {0x105E, 0x1060, GcbProp::Extend},
    {0x1071, 0x1074, GcbProp::Extend},
    {0x1082, 0x1082, GcbProp::Extend},
    {0x1084, 0x1084, GcbProp::SpacingMark},
    {0x1085, 0x1086, GcbProp::Extend},
    {0x108D, 0x108D, GcbProp::Extend},
    {0x109D, 0x109D, GcbProp::Extend},
    {0x1100, 0x115F, GcbProp::L},
    {0x1160, 0x11A7, GcbProp::V},
    {0x11A8, 0x11FF, GcbProp::T},
    {0x135D, 0x135F, GcbProp::Extend},
    {0x1712, 0x1714, GcbProp::Extend},
    {0x1715, 0x1715, GcbProp::SpacingMark},
    {0x1732, 0x1733, GcbProp::Extend},
    {0x1734, 0x1734, GcbProp::SpacingMark},
    {0x1752, 0x1753, GcbProp::Extend},
    {0x1772, 0x1773, GcbProp::Extend},
    {0x17B4, 0x17B5, GcbProp::Extend},
    {0x17B6, 0x17B6, GcbProp::SpacingMark},
    {0x17B7, 0x17BD, GcbProp::Extend},
    {0x17BE, 0x17C5, GcbProp::SpacingMark},
    {0x17C6, 0x17C6, GcbProp::Extend},
    {0x17C7, 0x17C8, GcbProp::SpacingMark},
    {0x17C9, 0x17D3, GcbProp::Extend},
    {0x17DD, 0x17DD, GcbProp::Extend},
    {0x180B, 0x180D, GcbProp::Extend},
    {0x180E, 0x180E, GcbProp::Control},
    {0x180F, 0x180F, GcbProp::Extend},
    {0x1885, 0x1886, GcbProp::Extend},
    {0x18A9, 0x18A9, GcbProp::Extend},
    {0x1920, 0x1922, GcbProp::Extend},
    {0x1923, 0x1926, GcbProp::SpacingMark},
    {0x1927, 0x1928, GcbProp::Extend},
    {0x1929, 0x192B, GcbProp::SpacingMark},
    {0x1930, 0x1931, GcbProp::SpacingMark},
    {0x1932, 0x1932, GcbProp::Extend},
    {0x1933, 0x1938, GcbProp::SpacingMark},
    {0x1939, 0x193B, GcbProp::Extend},
    {0x1A17, 0x1A18, GcbProp::Extend},
    {0x1A19, 0x1A1A, GcbProp::SpacingMark},
    {0x1A1B, 0x1A1B, GcbProp::Extend},
    {0x1A55, 0x1A55, GcbProp::SpacingMark},
    {0x1A56, 0x1A56, GcbProp::Extend},
    {0x1A57, 0x1A57, GcbProp::SpacingMark},
    {0x1A58, 0x1A5E, GcbProp::Extend},
    {0x1A60, 0x1A60, GcbProp::Extend},
    {0x1A62, 0x1A62, GcbProp::Extend},
    {0x1A65, 0x1A6C, GcbProp::Extend},
    {0x1A6D, 0x1A72, GcbProp::SpacingMark},
    {0x1A73, 0x1A7C, GcbProp::Extend},
    {0x1A7F, 0x1A7F, GcbProp::Extend},
    {0x1AB0, 0x1ABD, GcbProp::Extend},
    {0x1ABE, 0x1ABE, GcbProp::Extend},
    {0x1ABF, 0x1ACE, GcbProp::Extend},
    {0x1B00, 0x1B03, GcbProp::Extend},
    {0x1B04, 0x1B04, GcbProp::SpacingMark},
    {0x1B34, 0x1B34, GcbProp::Extend},
    {0x1B35, 0x1B35, GcbProp::Extend},
    {0x1B36, 0x1B3A, GcbProp::Extend},
    {0x1B3B, 0x1B3B, GcbProp::SpacingMark},
    {0x1B3C, 0x1B3C, GcbProp::Extend},
    {0x1B3D, 0x1B41, GcbProp::SpacingMark},
    {0x1B42, 0x1B42, GcbProp::Extend},
    {0x1B43, 0x1B44, GcbProp::SpacingMark},
    {0x1B6B, 0x1B73, GcbProp::Extend},
    {0x1B80, 0x1B81, GcbProp::Extend},
    {0x1B82, 0x1B82, GcbProp::SpacingMark},
    {0x1BA1, 0x1BA1, GcbProp::SpacingMark},
    {0x1BA2, 0x1BA5, GcbProp::Extend},
    {0x1BA6, 0x1BA7, GcbProp::SpacingMark},
    {0x1BA8, 0x1BA9, GcbProp::Extend},
    {0x1BAA, 0x1BAA, GcbProp::SpacingMark},
    {0x1BAB, 0x1BAD, GcbProp::Extend},
    {0x1BE6, 0x1BE6, GcbProp::Extend},
    {0x1BE7, 0x1BE7, GcbProp::SpacingMark},
    {0x1BE8, 0x1BE9, GcbProp::Extend},
    {0x1BEA, 0x1BEC, GcbProp::SpacingMark},
    {0x1BED, 0x1BED, GcbProp::Extend},
    {0x1BEE, 0x1BEE, GcbProp::SpacingMark},
    {0x1BEF, 0x1BF1, GcbProp::Extend},
    {0x1BF2, 0x1BF3, GcbProp::SpacingMark},
    {0x1C24, 0x1C2B, GcbProp::SpacingMark},
    {0x1C2C, 0x1C33, GcbProp::Extend},
    {0x1C34, 0x1C35, GcbProp::SpacingMark},
    {0x1C36, 0x1C37, GcbProp::Extend},
    {0x1CD0, 0x1CD2, GcbProp::Extend},
    {0x1CD4, 0x1CE0, GcbProp::Extend},
    {0x1CE1, 0x1CE1, GcbProp::SpacingMark},
    {0x1CE2, 0x1CE8, GcbProp::Extend},
    {0x1CED, 0x1CED, GcbProp::Extend},
    {0x1CF4, 0x1CF4, GcbProp::Extend},
    {0x1CF7, 0x1CF7, GcbProp::SpacingMark},
    {0x1CF8, 0x1CF9, GcbProp::Extend},
    {0x1DC0, 0x1DFF, GcbProp::Extend},
    {0x200B, 0x200B, GcbProp::Control},
    {0x200C, 0x200C, GcbProp::Extend},
    {0x200D, 0x200D, GcbProp::ZWJ},
    {0x200E, 0x200F, GcbProp::Control},
    {0x2028, 0x2028, GcbProp::Control},
    {0x2029, 0x2029, GcbProp::Control},
    {0x202A, 0x202E, GcbProp::Control},
    {0x2060, 0x2064, GcbProp::Control},
    {0x2065, 0x2065, GcbProp::Control},
    {0x2066, 0x206F, GcbProp::Control},
    {0x20D0, 0x20DC, GcbProp::Extend},
    {0x20DD, 0x20E0, GcbProp::Extend},
    {0x20E1, 0x20E1, GcbProp::Extend},
    {0x20E2, 0x20E4, GcbProp::Extend},
    {0x20E5, 0x20F0, GcbProp::Extend},
    {0x2CEF, 0x2CF1, GcbProp::Extend},
    {0x2D7F, 0x2D7F, GcbProp::Extend},
    {0x2DE0, 0x2DFF, GcbProp::Extend},
    {0x302A, 0x302D, GcbProp::Extend},
    {0x302E, 0x302F, GcbProp::Extend},
    {0x3099, 0x309A, GcbProp::Extend},
    {0xA66F, 0xA66F, GcbProp::Extend},
    {0xA670, 0xA672, GcbProp::Extend},
    {0xA674, 0xA67D, GcbProp::Extend},
    {0xA69E, 0xA69F, GcbProp::Extend},
    {0xA6F0, 0xA6F1, GcbProp::Extend},
    {0xA802, 0xA802, GcbProp::Extend},
    {0xA806, 0xA806, GcbProp::Extend},
    {0xA80B, 0xA80B, GcbProp::Extend},
    {0xA823, 0xA824, GcbProp::SpacingMark},
    {0xA825, 0xA826, GcbProp::Extend},
    {0xA827, 0xA827, GcbProp::SpacingMark},
    {0xA82C, 0xA82C, GcbProp::Extend},
    {0xA880, 0xA881, GcbProp::SpacingMark},
    {0xA8B4, 0xA8C3, GcbProp::SpacingMark},
    {0xA8C4, 0xA8C5, GcbProp::Extend},
    {0xA8E0, 0xA8F1, GcbProp::Extend},
    {0xA8FF, 0xA8FF, GcbProp::Extend},
    {0xA926, 0xA92D, GcbProp::Extend},
    {0xA947, 0xA951, GcbProp::Extend},
    {0xA952, 0xA953, GcbProp::SpacingMark},
    {0xA960, 0xA97C, GcbProp::L},
    {0xA980, 0xA982, GcbProp::Extend},
    {0xA983, 0xA983, GcbProp::SpacingMark},
    {0xA9B3, 0xA9B3, GcbProp::Extend},
    {0xA9B4, 0xA9B5, GcbProp::SpacingMark},
    {0xA9B6, 0xA9B9, GcbProp::Extend},
    {0xA9BA, 0xA9BB, GcbProp::SpacingMark},
    {0xA9BC, 0xA9BD, GcbProp::Extend},
    {0xA9BE, 0xA9C0, GcbProp::SpacingMark},
    {0xA9E5, 0xA9E5, GcbProp::Extend},
    {0xAA29, 0xAA2E, GcbProp::Extend},
    {0xAA2F, 0xAA30, GcbProp::SpacingMark},
    {0xAA31, 0xAA32, GcbProp::Extend},
    {0xAA33, 0xAA34, GcbProp::SpacingMark},
    {0xAA35, 0xAA36, GcbProp::Extend},
    {0xAA43, 0xAA43, GcbProp::Extend},
    {0xAA4C, 0xAA4C, GcbProp::Extend},
    {0xAA4D, 0xAA4D, GcbProp::SpacingMark},
    {0xAA7C, 0xAA7C, GcbProp::Extend},
    {0xAAB0, 0xAAB0, GcbProp::Extend},
    {0xAAB2, 0xAAB4, GcbProp::Extend},
    {0xAAB7, 0xAAB8, GcbProp::Extend},
    {0xAABE, 0xAABF, GcbProp::Extend},
    {0xAAC1, 0xAAC1, GcbProp::Extend},
    {0xAAEB, 0xAAEB, GcbProp::SpacingMark},
    {0xAAEC, 0xAAED, GcbProp::Extend},
    {0xAAEE, 0xAAEF, GcbProp::SpacingMark},
    {0xAAF5, 0xAAF5, GcbProp::SpacingMark},
    {0xAAF6, 0xAAF6, GcbProp::Extend},
    {0xABE3, 0xABE4, GcbProp::SpacingMark},
    {0xABE5, 0xABE5, GcbProp::Extend},
    {0xABE6, 0xABE7, GcbProp::SpacingMark},
    {0xABE8, 0xABE8, GcbProp::Extend},
    {0xABE9, 0xABEA, GcbProp::SpacingMark},
    {0xABEC, 0xABEC, GcbProp::SpacingMark},
    {0xABED, 0xABED, GcbProp::Extend},
    {0xD7B0, 0xD7C6, GcbProp::V},
    {0xD7CB, 0xD7FB, GcbProp::T},
    {0xFB1E, 0xFB1E, GcbProp::Extend},
    {0xFE00, 0xFE0F, GcbProp::Extend},
    {0xFE20, 0xFE2F, GcbProp::Extend},
    {0xFEFF, 0xFEFF, GcbProp::Control},
    {0xFF9E, 0xFF9F, GcbProp::Extend},
    {0xFFF0, 0xFFF8, GcbProp::Control},
    {0xFFF9, 0xFFFB, GcbProp::Control},
    {0x101FD, 0x101FD, GcbProp::Extend},
    {0x102E0, 0x102E0, GcbProp::Extend},
    {0x10376, 0x1037A, GcbProp::Extend},
    {0x10A01, 0x10A03, GcbProp::Extend},
    {0x10A05, 0x10A06, GcbProp::Extend},
    {0x10A0C, 0x10A0F, GcbProp::Extend},
    {0x10A38, 0x10A3A, GcbProp::Extend},
    {0x10A3F, 0x10A3F, GcbProp::Extend},
    {0x10AE5, 0x10AE6, GcbProp::Extend},
    {0x10D24, 0x10D27, GcbProp::Extend},
    {0x10EAB, 0x10EAC, GcbProp::Extend},
    {0x10EFD, 0x10EFF, GcbProp::Extend},
    {0x10F46, 0x10F50, GcbProp::Extend},
    {0x10F82, 0x10F85, GcbProp::Extend},
    {0x11000, 0x11000, GcbProp::SpacingMark},
    {0x11001, 0x11001, GcbProp::Extend},
    {0x11002, 0x11002, GcbProp::SpacingMark},
    {0x11038, 0x11046, GcbProp::Extend},
    {0x11070, 0x11070, GcbProp::Extend},
    {0x11073, 0x11074, GcbProp::Extend},
    {0x1107F, 0x11081, GcbProp::Extend},
    {0x11082, 0x11082, GcbProp::SpacingMark},
    {0x110B0, 0x110B2, GcbProp::SpacingMark},
    {0x110B3, 0x110B6, GcbProp::Extend},
    {0x110B7, 0x110B8, GcbProp::SpacingMark},
    {0x110B9, 0x110BA, GcbProp::Extend},
    {0x110BD, 0x110BD, GcbProp::Prepend},
    {0x110C2, 0x110C2, GcbProp::Extend},
    {0x110CD, 0x110CD, GcbProp::Prepend},
    {0x11100, 0x11102, GcbProp::Extend},
    {0x11127, 0x1112B, GcbProp::Extend},
    {0x1112C, 0x1112C, GcbProp::SpacingMark},
    {0x1112D, 0x11134, GcbProp::Extend},
    {0x11145, 0x11146, GcbProp::SpacingMark},
    {0x11173, 0x11173, GcbProp::Extend},
    {0x11180, 0x11181, GcbProp::Extend},
    {0x11182, 0x11182, GcbProp::SpacingMark},
    {0x111B3, 0x111B5, GcbProp::SpacingMark},
    {0x111B6, 0x111BE, GcbProp::Extend},
    {0x111BF, 0x111C0, GcbProp::SpacingMark},
    {0x111C2, 0x111C3, GcbProp::Prepend},
    {0x111C9, 0x111CC, GcbProp::Extend},
    {0x111CE, 0x111CE, GcbProp::SpacingMark},
    {0x111CF, 0x111CF, GcbProp::Extend},
    {0x1122C, 0x1122E, GcbProp::SpacingMark},
    {0x1122F, 0x11231, GcbProp::Extend},
    {0x11232, 0x11233, GcbProp::SpacingMark},
    {0x11234, 0x11234, GcbProp::Extend},
    {0x11235, 0x11235, GcbProp::SpacingMark},
    {0x11236, 0x11237, GcbProp::Extend},
    {0x1123E, 0x1123E, GcbProp::Extend},
    {0x11241, 0x11241, GcbProp::Extend},
    {0x112DF, 0x112DF, GcbProp::Extend},
    {0x112E0, 0x112E2, GcbProp::SpacingMark},
    {0x112E3, 0x112EA, GcbProp::Extend},
    {0x11300, 0x11301, GcbProp::Extend},
    {0x11302, 0x11303, GcbProp::SpacingMark},
    {0x1133B, 0x1133C, GcbProp::Extend},
    {0x1133E, 0x1133E, GcbProp::Extend},
    {0x1133F, 0x1133F, GcbProp::SpacingMark},
    {0x11340, 0x11340, GcbProp::Extend},
    {0x11341, 0x11344, GcbProp::SpacingMark},
    {0x11347, 0x11348, GcbProp::SpacingMark},
    {0x1134B, 0x1134D, GcbProp::SpacingMark},
    {0x11357, 0x11357, GcbProp::Extend},
    {0x11362, 0x11363, GcbProp::SpacingMark},
    {0x11366, 0x1136C, GcbProp::Extend},
    {0x11370, 0x11374, GcbProp::Extend},
    {0x11435, 0x11437, GcbProp::SpacingMark},
    {0x11438, 0x1143F, GcbProp::Extend},
    {0x11440, 0x11441, GcbProp::SpacingMark},
    {0x11442, 0x11444, GcbProp::Extend},
    {0x11445, 0x11445, GcbProp::SpacingMark},
    {0x11446, 0x11446, GcbProp::Extend},
    {0x1145E, 0x1145E, GcbProp::Extend},
    {0x114B0, 0x114B0, GcbProp::Extend},
    {0x114B1, 0x114B2, GcbProp::SpacingMark},
    {0x114B3, 0x114B8, GcbProp::Extend},
    {0x114B9, 0x114B9, GcbProp::SpacingMark},
    {0x114BA, 0x114BA, GcbProp::Extend},
    {0x114BB, 0x114BC, GcbProp::SpacingMark},
    {0x114BD, 0x114BD, GcbProp::Extend},
    {0x114BE, 0x114BE, GcbProp::SpacingMark},
    {0x114BF, 0x114C0, GcbProp::Extend},
    {0x114C1, 0x114C1, GcbProp::SpacingMark},
    {0x114C2, 0x114C3, GcbProp::Extend},
    {0x115AF, 0x115AF, GcbProp::Extend},
    {0x115B0, 0x115B1, GcbProp::SpacingMark},
    {0x115B2, 0x115B5, GcbProp::Extend},
    {0x115B8, 0x115BB, GcbProp::SpacingMark},
    {0x115BC, 0x115BD, GcbProp::Extend},
    {0x115BE, 0x115BE, GcbProp::SpacingMark},
    {0x115BF, 0x115C0, GcbProp::Extend},
    {0x115DC, 0x115DD, GcbProp::Extend},
    {0x11630, 0x11632, GcbProp::SpacingMark},
    {0x11633, 0x1163A, GcbProp::Extend},
    {0x1163B, 0x1163C, GcbProp::SpacingMark},
    {0x1163D, 0x1163D, GcbProp::Extend},
    {0x1163E, 0x1163E, GcbProp::SpacingMark},
    {0x1163F, 0x11640, GcbProp::Extend},
    {0x116AB, 0x116AB, GcbProp::Extend},
    {0x116AC, 0x116AC, GcbProp::SpacingMark},
    {0x116AD, 0x116AD, GcbProp::Extend},
    {0x116AE, 0x116AF, GcbProp::SpacingMark},
    {0x116B0, 0x116B5, GcbProp::Extend},
    {0x116B6, 0x116B6, GcbProp::SpacingMark},
    {0x116B7, 0x116B7, GcbProp::Extend},
    {0x1171D, 0x1171F, GcbProp::Extend},
    {0x11722, 0x11725, GcbProp::Extend},
    {0x11726, 0x11726, GcbProp::SpacingMark},
    {0x11727, 0x1172B, GcbProp::Extend},
    {0x1182C, 0x1182E, GcbProp::SpacingMark},
    {0x1182F, 0x11837, GcbProp::Extend},
    {0x11838, 0x11838, GcbProp::SpacingMark},
    {0x11839, 0x1183A, GcbProp::Extend},
    {0x11930, 0x11930, GcbProp::Extend},
    {0x11931, 0x11935, GcbProp::SpacingMark},
    {0x11937, 0x11938, GcbProp::SpacingMark},
    {0x1193B, 0x1193C, GcbProp::Extend},
    {0x1193D, 0x1193D, GcbProp::SpacingMark},
    {0x1193E, 0x1193E, GcbProp::Extend},
    {0x1193F, 0x1193F, GcbProp::Prepend},
    {0x11940, 0x11940, GcbProp::SpacingMark},
    {0x11941, 0x11941, GcbProp::Prepend},
    {0x11942, 0x11942, GcbProp::SpacingMark},
    {0x11943, 0x11943, GcbProp::Extend},
    {0x119D1, 0x119D3, GcbProp::SpacingMark},
    {0x119D4, 0x119D7, GcbProp::Extend},
    {0x119DA, 0x119DB, GcbProp::Extend},
    {0x119DC, 0x119DF, GcbProp::SpacingMark},
    {0x119E0, 0x119E0, GcbProp::Extend},
    {0x119E4, 0x119E4, GcbProp::SpacingMark},
    {0x11A01, 0x11A0A, GcbProp::Extend},
    {0x11A33, 0x11A38, GcbProp::Extend},
    {0x11A39, 0x11A39, GcbProp::SpacingMark},
    {0x11A3A, 0x11A3A, GcbProp::Prepend},
    {0x11A3B, 0x11A3E, GcbProp::Extend},
    {0x11A47, 0x11A47, GcbProp::Extend},
    {0x11A51, 0x11A56, GcbProp::Extend},
    {0x11A57, 0x11A58, GcbProp::SpacingMark},
    {0x11A59, 0x11A5B, GcbProp::Extend},
    {0x11A84, 0x11A89, GcbProp::Prepend},
    {0x11A8A, 0x11A96, GcbProp::Extend},
    {0x11A97, 0x11A97, GcbProp::SpacingMark},
    {0x11A98, 0x11A99, GcbProp::Extend},
    {0x11C2F, 0x11C2F, GcbProp::SpacingMark},
    {0x11C30, 0x11C36, GcbProp::Extend},
    {0x11C38, 0x11C3D, GcbProp::Extend},
    {0x11C3E, 0x11C3E, GcbProp::SpacingMark},
    {0x11C3F, 0x11C3F, GcbProp::Extend},
    {0x11C92, 0x11CA7, GcbProp::Extend},
    {0x11CA9, 0x11CA9, GcbProp::SpacingMark},
    {0x11CAA, 0x11CB0, GcbProp::Extend},
    {0x11CB1, 0x11CB1, GcbProp::SpacingMark},
    {0x11CB2, 0x11CB3, GcbProp::Extend},
    {0x11CB4, 0x11CB4, GcbProp::SpacingMark},
    {0x11CB5, 0x11CB6, GcbProp::Extend},
    {0x11D31, 0x11D36, GcbProp::Extend},
    {0x11D3A, 0x11D3A, GcbProp::Extend},
    {0x11D3C, 0x11D3D, GcbProp::Extend},
    {0x11D3F, 0x11D45, GcbProp::Extend},
    {0x11D46, 0x11D46, GcbProp::Prepend},
    {0x11D47, 0x11D47, GcbProp::Extend},
    {0x11D8A, 0x11D8E, GcbProp::SpacingMark},
    {0x11D90, 0x11D91, GcbProp::Extend},
    {0x11D93, 0x11D94, GcbProp::SpacingMark},
    {0x11D95, 0x11D95, GcbProp::Extend},
    {0x11D96, 0x11D96, GcbProp::SpacingMark},
    {0x11D97, 0x11D97, GcbProp::Extend},
    {0x11EF3, 0x11EF4, GcbProp::Extend},
    {0x11EF5, 0x11EF6, GcbProp::SpacingMark},
    {0x11F00, 0x11F01, GcbProp::Extend},
    {0x11F02, 0x11F02, GcbProp::Prepend},
    {0x11F03, 0x11F03, GcbProp::SpacingMark},
    {0x11F34, 0x11F35, GcbProp::SpacingMark},
    {0x11F36, 0x11F3A, GcbProp::Extend},
    {0x11F3E, 0x11F3F, GcbProp::SpacingMark},
    {0x11F40, 0x11F40, GcbProp::Extend},
    {0x11F41, 0x11F41, GcbProp::SpacingMark},
    {0x11F42, 0x11F42, GcbProp::Extend},
    {0x13430, 0x1343F, GcbProp::Control},
    {0x13440, 0x13440, GcbProp::Extend},
    {0x13447, 0x13455, GcbProp::Extend},
    {0x16AF0, 0x16AF4, GcbProp::Extend},
    {0x16B30, 0x16B36, GcbProp::Extend},
    {0x16F4F, 0x16F4F, GcbProp::Extend},
    {0x16F51, 0x16F87, GcbProp::SpacingMark},
    {0x16F8F, 0x16F92, GcbProp::Extend},
    {0x16FE4, 0x16FE4, GcbProp::Extend},
    {0x16FF0, 0x16FF1, GcbProp::SpacingMark},
    {0x1BC9D, 0x1BC9E, GcbProp::Extend},
    {0x1BCA0, 0x1BCA3, GcbProp::Control},
    {0x1CF00, 0x1CF2D, GcbProp::Extend},
    {0x1CF30, 0x1CF46, GcbProp::Extend},
    {0x1D165, 0x1D165, GcbProp::Extend},
    {0x1D166, 0x1D166, GcbProp::SpacingMark},
    {0x1D167, 0x1D169, GcbProp::Extend},
    {0x1D16D, 0x1D16D, GcbProp::SpacingMark},
    {0x1D16E, 0x1D172, GcbProp::Extend},
    {0x1D173, 0x1D17A, GcbProp::Control},
    {0x1D17B, 0x1D182, GcbProp::Extend},
    {0x1D185, 0x1D18B, GcbProp::Extend},
    {0x1D1AA, 0x1D1AD, GcbProp::Extend},
    {0x1D242, 0x1D244, GcbProp::Extend},
    {0x1DA00, 0x1DA36, GcbProp::Extend},
    {0x1DA3B, 0x1DA6C, GcbProp::Extend},
    {0x1DA75, 0x1DA75, GcbProp::Extend},
    {0x1DA84, 0x1DA84, GcbProp::Extend},
    {0x1DA9B, 0x1DA9F, GcbProp::Extend},
    {0x1DAA1, 0x1DAAF, GcbProp::Extend},
    {0x1E000, 0x1E006, GcbProp::Extend},
    {0x1E008, 0x1E018, GcbProp::Extend},
    {0x1E01B, 0x1E021, GcbProp::Extend},
    {0x1E023, 0x1E024, GcbProp::Extend},
    {0x1E026, 0x1E02A, GcbProp::Extend},
    {0x1E08F, 0x1E08F, GcbProp::Extend},
    {0x1E130, 0x1E136, GcbProp::Extend},
    {0x1E2AE, 0x1E2AE, GcbProp::Extend},
    {0x1E2EC, 0x1E2EF, GcbProp::Extend},
    {0x1E4EC, 0x1E4EF, GcbProp::Extend},
    {0x1E8D0, 0x1E8D6, GcbProp::Extend},
    {0x1E944, 0x1E94A, GcbProp::Extend},
    {0x1F1E6, 0x1F1FF, GcbProp::RI},
    {0x1F3FB, 0x1F3FF, GcbProp::Extend},
    {0xE0000, 0xE0000, GcbProp::Control},
    {0xE0001, 0xE0001, GcbProp::Control},
    {0xE0002, 0xE001F, GcbProp::Control},
    {0xE0020, 0xE007F, GcbProp::Extend},
    {0xE0080, 0xE00FF, GcbProp::Control},
    {0xE0100, 0xE01EF, GcbProp::Extend},
    {0xE01F0, 0xE0FFF, GcbProp::Control},
};
static constexpr int kGcbN =
    static_cast<int>(sizeof(kGcb) / sizeof(kGcb[0]));

// EAW=W or EAW=F: code point occupies 2 terminal columns by East Asian Width.
static bool isEawWide(uint32_t cp) noexcept {
    return inRanges(kWide, kWideN, cp);
}

// Emoji_Presentation=Yes: code point defaults to emoji presentation (2 cells).
// Covers all Emoji_Presentation characters, including Regional Indicators
// (U+1F1E6..U+1F1FF) which have EAW=N but are visually 2 cells.
static bool isEmojiPres(uint32_t cp) noexcept {
    return inRanges(kEmojiPres, kEmojiPresN, cp);
}

// Emoji=Yes: code point is in the Unicode Emoji property set.
// Used to detect emoji presentation sequences (Emoji + U+FE0F → 2 cells).
static bool isEmoji(uint32_t cp) noexcept {
    return inRanges(kEmoji, kEmojiN, cp);
}

static bool isExtendedPictographic(uint32_t cp) noexcept {
    return inRanges(kExtpic, kExtpicN, cp);
}

// Returns the GCB property of cp per GraphemeBreakProperty.txt.
// LV syllables (cp in AC00..D7A3 where (cp-AC00)%28==0) and LVT syllables
// ((cp-AC00)%28!=0) are computed directly; all others use the k_gcb table.
static GcbProp gcbPropOf(uint32_t cp) noexcept {
    if (cp >= 0xAC00u && cp <= 0xD7A3u)
        return ((cp - 0xAC00u) % 28u == 0u) ? GcbProp::LV : GcbProp::LVT;
    return gcbLookup(kGcb, kGcbN, cp);
}

// Returns the display width of a code point when it is the base of a new
// grapheme cluster.  Segmentation (GCB) and display width are orthogonal:
//   GcbProp::Extend: wide emoji modifiers (EAW=W, e.g. U+1F3FB–U+1F3FF) → 2;
//     all other Extend (diacritics, variation selectors, etc.) → 0.
//   GcbProp::ZWJ, SpacingMark, Prepend: 0.
//   GcbProp::Control + C0/DEL/C1 (cp ≤ U+009F): 1 (visible replacement glyph).
//   GcbProp::Control + non-C0/C1 (cp > U+009F): 0 (Unicode Cf format control).
//   GcbProp::CR, LF: 0 (line terminators; forbidden in compute_cell_run input).
//   All other code points: 2 if EAW=W/F (is_eaw_wide) or Emoji_Presentation
//     (is_emoji_pres), else 1.
// NOTE: VS-16 (U+FE0F) upgrade for text-default emoji (Emoji=Yes,
//   Emoji_Presentation=No) is applied in compute_cell_run() after absorption,
//   not here, because it depends on whether U+FE0F was absorbed into the cluster.
// NOTE: Control/CR/LF are short-circuited before this function in compute_cell_run.
static uint32_t displayWidthOf(uint32_t cp, GcbProp gcb) noexcept {
    switch (gcb) {
    case GcbProp::Extend:
        return (isEawWide(cp) || isEmojiPres(cp)) ? 2u : 0u;
    case GcbProp::ZWJ:
    case GcbProp::SpacingMark:
    case GcbProp::Prepend:
        return 0u;
    case GcbProp::Control:
    case GcbProp::CR:
    case GcbProp::LF:
        return (cp <= 0x009Fu) ? 1u : 0u;
    default:
        return (isEawWide(cp) || isEmojiPres(cp)) ? 2u : 1u;
    }
}

// Returns true when a Hangul code point of GCB type `next` may extend a cluster
// whose most-recently-added non-Extend Hangul code point had type `last`.
// Only the Hangul GCB types (L, V, T, LV, LVT) are meaningful; all other GcbProp
// values return false so callers need not pre-filter.
static bool hangulExtends(GcbProp last, GcbProp next) noexcept {
    switch (last) {
    case GcbProp::L:
        // GB6: L × (L | V | LV | LVT)
        return next == GcbProp::L   || next == GcbProp::V  ||
               next == GcbProp::LV  || next == GcbProp::LVT;
    case GcbProp::LV:
    case GcbProp::V:
        // GB7: (LV | V) × (V | T)
        return next == GcbProp::V || next == GcbProp::T;
    case GcbProp::LVT:
    case GcbProp::T:
        // GB8: (LVT | T) × T
        return next == GcbProp::T;
    default:
        return false;
    }
}

// GB11 emoji ZWJ sequence state machine
//
// Tracks whether the cluster tail is in a state that can continue an
// ExtPic Extend* ZWJ sequence (GB11).
//   None   — no active ExtPic chain
//   ExtPic — last Hangul-unrelated base (or post-GB11 absorbed ExtPic) was
//            Extended_Pictographic; Extend* may grow it
//   Zwj    — ExtPic Extend* ZWJ pattern complete; next ExtPic absorbs via GB11
enum class GB11State : uint8_t { None, ExtPic, Zwj };

struct DecodeResult {
    uint32_t codepoint; // Decoded code point (0 when invalid)
    uint32_t byteLen; // Bytes this result consumed (always >= 1)
    bool valid; // True when a well-formed sequence was decoded
};

// Decode one UTF-8 sequence starting at data[pos].  end is the exclusive
// limit.  On invalid input, returns {0, 1, false} — the caller must advance
// by exactly 1 byte and emit one invalid_utf8 span per call.
//
// Rejects: continuation bytes at sequence start, overlong encodings (0xC0/C1
// leads, 0xE0 requiring second byte < 0xA0, 0xF0 requiring second byte <
// 0x90), surrogates (0xED second byte >= 0xA0), out-of-range leads (> 0xF4),
// and truncated sequences (not enough continuation bytes before end).
static DecodeResult decodeUtf8(const uint8_t* data,
                                size_t pos,
                                size_t end) noexcept {
    const uint8_t b0 = data[pos];

    if (b0 < 0x80)
        return {b0, 1, true};

    if (b0 < 0xC2 || b0 > 0xF4)
        return {0, 1, false};

    if (b0 < 0xE0) {
        if (pos + 2 > end)          return {0, 1, false}; // truncated
        const uint8_t b1 = data[pos + 1];
        if (b1 < 0x80 || b1 > 0xBF) return {0, 1, false}; // bad continuation
        return {static_cast<uint32_t>((b0 & 0x1Fu) << 6 | (b1 & 0x3Fu)), 2, true};
    }

    if (b0 < 0xF0) {
        if (pos + 3 > end)           return {0, 1, false}; // truncated
        const uint8_t b1 = data[pos + 1];
        const uint8_t b2 = data[pos + 2];
        if (b0 == 0xE0 && b1 < 0xA0) return {0, 1, false}; // overlong
        if (b0 == 0xED && b1 > 0x9F) return {0, 1, false}; // surrogate
        if (b1 < 0x80 || b1 > 0xBF)  return {0, 1, false};
        if (b2 < 0x80 || b2 > 0xBF)  return {0, 1, false};
        return {static_cast<uint32_t>(
                    (b0 & 0x0Fu) << 12 | (b1 & 0x3Fu) << 6 | (b2 & 0x3Fu)),
                3, true};
    }

    if (pos + 4 > end)           return {0, 1, false}; // truncated
    const uint8_t b1 = data[pos + 1];
    const uint8_t b2 = data[pos + 2];
    const uint8_t b3 = data[pos + 3];
    if (b0 == 0xF0 && b1 < 0x90) return {0, 1, false}; // overlong
    if (b0 == 0xF4 && b1 > 0x8F) return {0, 1, false}; // > U+10FFFF
    if (b1 < 0x80 || b1 > 0xBF)  return {0, 1, false};
    if (b2 < 0x80 || b2 > 0xBF)  return {0, 1, false};
    if (b3 < 0x80 || b3 > 0xBF)  return {0, 1, false};
    return {static_cast<uint32_t>(
                (b0 & 0x07u) << 18 | (b1 & 0x3Fu) << 12 |
                (b2 & 0x3Fu) << 6  | (b3 & 0x3Fu)),
            4, true};
}

CellRun computeCellRun(std::string_view lineUtf8, int tabWidth) {
    if (tabWidth < 1 || tabWidth > 16) {
        throw std::invalid_argument("tab width must be between 1 and 16");
    }
    ++gCellRunCalls;

    CellRun result;
    result.totalCells = 0;

    const auto* data = reinterpret_cast<const uint8_t*>(lineUtf8.data());
    const size_t end  = lineUtf8.size();
    size_t pos        = 0;
    uint32_t curCell = 0; // Running column count, used for tab expansion

    while (pos < end) {
        const size_t clusterStart = pos;

        const DecodeResult base = decodeUtf8(data, pos, end);

        if (!base.valid) {
            result.spans.push_back({
                static_cast<uint32_t>(clusterStart), 1u, 1u,
                CellKind::InvalidUtf8
            });
            result.totalCells += 1;
            curCell            += 1;
            pos                 += 1;
            continue;
        }

        const uint32_t cp = base.codepoint;
        pos += base.byteLen;

        // Tab: advance to next tab stop, minimum 1 column
        if (cp == 0x09u) {
            const uint32_t tw      = static_cast<uint32_t>(tabWidth);
            const uint32_t advance = tw - (curCell % tw);
            result.spans.push_back({
                static_cast<uint32_t>(clusterStart), 1u, advance, CellKind::Tab
            });
            result.totalCells += advance;
            curCell            += advance;
            continue; // Tab is never extended
        }

        const GcbProp gcb = gcbPropOf(cp);

        // GCB=Control breaks as its own cluster.  C0/DEL/C1 (cp ≤ U+009F) are
        // visible replacement glyphs (width=1); non-C0/C1 GCB=Control are Unicode
        // Cf format characters (soft hyphen, ZWSP, bidi controls) with width=0.
        // CR and LF do not appear in input by precondition
        // (grapheme_layout.h contract).
        if (gcb == GcbProp::Control || gcb == GcbProp::CR || gcb == GcbProp::LF) {
            const uint32_t ctrlWidth = (cp <= 0x009Fu) ? 1u : 0u;
            result.spans.push_back({
                static_cast<uint32_t>(clusterStart),
                static_cast<uint32_t>(base.byteLen),
                ctrlWidth, CellKind::Control
            });
            result.totalCells += ctrlWidth;
            curCell            += ctrlWidth;
            continue; // Control chars are never extended
        }

        // Determine the base cluster kind and cell width.
        // Prepend bases start at width 0; width is updated when they absorb
        // their following character (GB9b).
        uint32_t baseWidth;
        CellKind kind;

        if (gcb == GcbProp::Extend ||
            gcb == GcbProp::ZWJ    ||
            gcb == GcbProp::SpacingMark ||
            gcb == GcbProp::Prepend) {
            // Lone extending mark at line start (no preceding base).
            // Wide Extend code points (e.g. emoji modifiers U+1F3FB–U+1F3FF, EAW=W)
            // render as 2-cell glyphs even without a base; those get kind=text.
            // Zero-width Extend/ZWJ/SpacingMark alone get kind=combining, width=0.
            baseWidth = displayWidthOf(cp, gcb);
            kind       = (baseWidth == 0u) ? CellKind::Combining : CellKind::Text;
        } else {
            baseWidth = displayWidthOf(cp, gcb);
            kind       = CellKind::Text;
        }

        // last_gcb: GCB property of the most recently added code point in this
        // cluster.  Used to evaluate pair rules for each extension candidate.
        // For Hangul composition (GB6–8), we track the last Hangul GcbProp value;
        // Extend/ZWJ/SpacingMark absorptions reset this to GcbProp::Other.
        GcbProp lastGcb = gcb;

        uint32_t clusterLen = base.byteLen;
        GB11State gb11 = isExtendedPictographic(cp)
            ? GB11State::ExtPic
            : GB11State::None;
        bool riPaired = false;
        uint32_t effectiveBaseCp = cp; // updated if Prepend absorbs a real base
        bool sawVs16 = false; // set when U+FE0F is absorbed

        // Absorb extending code points into this grapheme cluster.
        // Rules are checked in UAX #29 priority order.
        while (pos < end) {
            const DecodeResult ext = decodeUtf8(data, pos, end);
            if (!ext.valid) break; // Invalid byte starts its own cluster

            const uint32_t extCp = ext.codepoint;
            const GcbProp extGcb = gcbPropOf(extCp);
            bool extends = false;

            if (extGcb == GcbProp::Extend || extGcb == GcbProp::ZWJ) {
                // GB9: × (Extend | ZWJ) — unconditional
                extends = true;
                if (extGcb == GcbProp::Extend) {
                    if (extCp == 0xFE0Fu) sawVs16 = true; // VS-16
                    gb11 = (gb11 == GB11State::ExtPic) ? GB11State::ExtPic
                                                       : GB11State::None;
                } else {
                    gb11 = (gb11 == GB11State::ExtPic) ? GB11State::Zwj
                                                       : GB11State::None;
                }
                lastGcb = GcbProp::Other; // severs Hangul composition
            } else if (extGcb == GcbProp::SpacingMark) {
                // GB9a: × SpacingMark — unconditional
                extends  = true;
                gb11     = GB11State::None; // SpacingMark breaks ExtPic chain
                lastGcb = GcbProp::Other;
            } else if (gb11 == GB11State::Zwj &&
                       isExtendedPictographic(extCp)) {
                // GB11: ExtPic Extend* ZWJ × ExtPic
                extends  = true;
                gb11     = GB11State::ExtPic;
                lastGcb = extGcb;
            } else if (lastGcb == GcbProp::RI && !riPaired && extGcb == GcbProp::RI) {
                // GB12/13: RI × RI (only when the cluster tail is still an RI,
                // i.e. no Extend/ZWJ/SpacingMark was absorbed after the base RI)
                extends   = true;
                riPaired = true;
                gb11      = GB11State::None;
                lastGcb  = extGcb;
            } else if (hangulExtends(lastGcb, extGcb)) {
                // GB6–GB8: Hangul jamo/syllable composition
                extends  = true;
                gb11     = GB11State::None;
                lastGcb = extGcb;
            } else if (lastGcb == GcbProp::Prepend &&
                       extGcb != GcbProp::Control  &&
                       extGcb != GcbProp::CR       &&
                       extGcb != GcbProp::LF) {
                // GB9b: Prepend × [^(Control|CR|LF)]
                extends = true;
                if (extGcb != GcbProp::Prepend) {
                    // Absorbed a real base: finalise cluster width.
                    effectiveBaseCp = extCp;
                    baseWidth = displayWidthOf(extCp, extGcb);
                    kind = (baseWidth == 0u)
                           ? CellKind::Combining : CellKind::Text;
                    gb11 = isExtendedPictographic(extCp)
                           ? GB11State::ExtPic : GB11State::None;
                }
                lastGcb = extGcb;
            }

            if (!extends) break;
            clusterLen += ext.byteLen;
            pos         += ext.byteLen;
        }

        // VS-16 emoji presentation sequence upgrade:
        // If U+FE0F was absorbed and the effective base is an Emoji character
        // that did not already get 2 cells, upgrade to 2.
        if (sawVs16 && baseWidth < 2u && isEmoji(effectiveBaseCp)) {
            baseWidth = 2u;
            kind = CellKind::Text;
        }

        result.spans.push_back({
            static_cast<uint32_t>(clusterStart),
            clusterLen,
            baseWidth,
            kind
        });
        result.totalCells += baseWidth;
        curCell            += baseWidth;
    }

    return result;
}

std::uint64_t cellRunCalls() { return gCellRunCalls; }
void resetCellRunCalls() { gCellRunCalls = 0; }

}  // namespace ssg
