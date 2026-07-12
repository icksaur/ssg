// UTF-8 grapheme segmentation and terminal cell layout.
//
// See include/ssg/layout.h for the public contract and
// doc/features/presentation-shell.md §Cell-width rules for the normative spec.
//
// Unicode version: 15.0.0 (released 2022-09-13).
// Grapheme clusters: UAX #29 extended grapheme clusters.
//   All rules applicable under the single-logical-line precondition
//   (input contains no CR or LF; GB3/GB4/GB5 are subsumed by that precondition):
//     GB6    — L × (L|V|LV|LVT)                      [Hangul leading jamo]
//     GB7    — (LV|V) × (V|T)                        [Hangul vowel/syllable]
//     GB8    — (LVT|T) × T                           [Hangul trailing jamo]
//     GB9    — × (Extend | ZWJ)                      [combining marks, ZWJ]
//     GB9a   — × SpacingMark                         [Indic/script spacing marks]
//     GB9b   — Prepend ×                             [Prepend chars absorb next]
//     GB11   — ExtPic Extend* ZWJ × ExtPic           [emoji ZWJ sequences]
//     GB12/13 — RI × RI                              [regional-indicator flag pairs]
// Width:  UAX #11 East Asian Width (W and F → 2 cells) + emoji-data.txt Wide.
//
// Data sources (all Unicode 15.0.0):
//   k_combining    — DerivedCoreProperties.txt (GCB=Extend subset: Mn/Me/Cf zero-width)
//   k_spacing_mark — GraphemeBreakProperty.txt (GCB=SpacingMark, i.e., Mc subset)
//   k_extpic       — emoji-data.txt (Extended_Pictographic property)
//   k_prepend      — GraphemeBreakProperty.txt (GCB=Prepend)
//   k_wide         — EastAsianWidth.txt (EAW=W or F) + emoji-data.txt Wide
//
// All property tables are sorted, non-overlapping URange arrays; binary search.

#include <ssg/layout.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace ssg {

// ---------------------------------------------------------------------------
// Internal types

struct URange {
    uint32_t lo;
    uint32_t hi;
};

// Binary search: true when cp falls in any range in ranges[0..n).
// Requires ranges to be sorted by lo and non-overlapping.
static bool in_ranges(const URange* ranges, int n, uint32_t cp) noexcept {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if      (cp < ranges[mid].lo) hi = mid - 1;
        else if (cp > ranges[mid].hi) lo = mid + 1;
        else                          return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Unicode 15.0.0 — Combining / zero-width code points
//
// Includes: General_Category Mn (Non_Spacing_Mark), Me (Enclosing_Mark),
// and zero-width Cf characters (ZWJ, ZWNJ, ZWSP, soft hyphen, bidi
// controls, word joiners, variation selectors, tag characters).
// Source: DerivedCoreProperties.txt, PropList.txt, emoji-data.txt
// Unicode 15.0.0 (2022-09-13).

static constexpr URange k_combining[] = {
    {0x00AD, 0x00AD},   // SOFT HYPHEN
    {0x0300, 0x036F},   // Combining Diacritical Marks (Mn)
    {0x0483, 0x0489},   // Cyrillic combining (Mn/Me)
    {0x0591, 0x05BD},   // Hebrew cantillation/points
    {0x05BF, 0x05BF},
    {0x05C1, 0x05C2},
    {0x05C4, 0x05C5},
    {0x05C7, 0x05C7},
    {0x0610, 0x061A},   // Arabic extended
    {0x064B, 0x065F},
    {0x0670, 0x0670},
    {0x06D6, 0x06DC},
    {0x06DF, 0x06E4},
    {0x06E7, 0x06E8},
    {0x06EA, 0x06ED},
    {0x0711, 0x0711},   // Syriac
    {0x0730, 0x074A},
    {0x07A6, 0x07B0},   // Thaana
    {0x07EB, 0x07F3},   // Nko
    {0x07FD, 0x07FD},
    {0x0816, 0x0823},   // Samaritan
    {0x0825, 0x082D},
    {0x0859, 0x085B},
    {0x0898, 0x089F},   // Arabic Extended-B
    {0x08CA, 0x08E1},   // Arabic Extended-A
    {0x08E3, 0x0902},
    {0x093A, 0x093A},   // Devanagari
    {0x093C, 0x093C},
    {0x0941, 0x0948},
    {0x094D, 0x094D},
    {0x0951, 0x0957},
    {0x0962, 0x0963},
    {0x0981, 0x0981},   // Bengali
    {0x09BC, 0x09BC},
    {0x09C1, 0x09C4},
    {0x09CD, 0x09CD},
    {0x09E2, 0x09E3},
    {0x09FE, 0x09FE},
    {0x0A01, 0x0A02},   // Gurmukhi
    {0x0A3C, 0x0A3C},
    {0x0A41, 0x0A42},
    {0x0A47, 0x0A48},
    {0x0A4B, 0x0A4D},
    {0x0A51, 0x0A51},
    {0x0A70, 0x0A71},
    {0x0A75, 0x0A75},
    {0x0A81, 0x0A82},   // Gujarati
    {0x0ABC, 0x0ABC},
    {0x0AC1, 0x0AC5},
    {0x0AC7, 0x0AC8},
    {0x0ACD, 0x0ACD},
    {0x0AE2, 0x0AE3},
    {0x0AFA, 0x0AFF},
    {0x0B01, 0x0B01},   // Oriya
    {0x0B3C, 0x0B3C},
    {0x0B3F, 0x0B3F},
    {0x0B41, 0x0B44},
    {0x0B4D, 0x0B4D},
    {0x0B55, 0x0B56},
    {0x0B62, 0x0B63},
    {0x0B82, 0x0B82},   // Tamil
    {0x0BC0, 0x0BC0},
    {0x0BCD, 0x0BCD},
    {0x0C00, 0x0C00},   // Telugu
    {0x0C04, 0x0C04},
    {0x0C3C, 0x0C3C},
    {0x0C3E, 0x0C40},
    {0x0C46, 0x0C48},
    {0x0C4A, 0x0C4D},
    {0x0C55, 0x0C56},
    {0x0C62, 0x0C63},
    {0x0C81, 0x0C81},   // Kannada
    {0x0CBC, 0x0CBC},
    {0x0CBF, 0x0CBF},
    {0x0CC6, 0x0CC6},
    {0x0CCC, 0x0CCD},
    {0x0CE2, 0x0CE3},
    {0x0D00, 0x0D01},   // Malayalam
    {0x0D3B, 0x0D3C},
    {0x0D41, 0x0D44},
    {0x0D4D, 0x0D4D},
    {0x0D62, 0x0D63},
    {0x0D81, 0x0D81},
    {0x0DCA, 0x0DCA},   // Sinhala
    {0x0DD2, 0x0DD4},
    {0x0DD6, 0x0DD6},
    {0x0E31, 0x0E31},   // Thai
    {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1},   // Lao
    {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD},
    {0x0F18, 0x0F19},   // Tibetan
    {0x0F35, 0x0F35},
    {0x0F37, 0x0F37},
    {0x0F39, 0x0F39},
    {0x0F71, 0x0F7E},
    {0x0F80, 0x0F84},
    {0x0F86, 0x0F87},
    {0x0F8D, 0x0F97},
    {0x0F99, 0x0FBC},
    {0x0FC6, 0x0FC6},
    {0x102D, 0x1030},   // Myanmar
    {0x1032, 0x1037},
    {0x1039, 0x103A},
    {0x103D, 0x103E},
    {0x1058, 0x1059},
    {0x105E, 0x1060},
    {0x1071, 0x1074},
    {0x1082, 0x1082},
    {0x1085, 0x1086},
    {0x108D, 0x108D},
    {0x109D, 0x109D},
    {0x135D, 0x135F},   // Ethiopic
    {0x1712, 0x1714},   // Tagalog
    {0x1732, 0x1733},   // Hanunoo
    {0x1752, 0x1753},   // Buhid
    {0x1772, 0x1773},   // Tagbanwa
    {0x17B4, 0x17B5},   // Khmer
    {0x17B7, 0x17BD},
    {0x17C6, 0x17C6},
    {0x17C9, 0x17D3},
    {0x17DD, 0x17DD},
    {0x180B, 0x180D},   // Mongolian
    {0x180F, 0x180F},
    {0x1885, 0x1886},
    {0x18A9, 0x18A9},
    {0x1920, 0x1922},   // Limbu
    {0x1927, 0x1928},
    {0x1932, 0x1932},
    {0x1939, 0x193B},
    {0x1A17, 0x1A18},   // Buginese
    {0x1A1B, 0x1A1B},
    {0x1A56, 0x1A56},   // Tai Tham
    {0x1A58, 0x1A5E},
    {0x1A60, 0x1A60},
    {0x1A62, 0x1A62},
    {0x1A65, 0x1A6C},
    {0x1A73, 0x1A7C},
    {0x1A7F, 0x1A7F},
    {0x1AB0, 0x1ACE},   // Combining Diacritical Marks Extended
    {0x1B00, 0x1B03},   // Balinese
    {0x1B34, 0x1B34},
    {0x1B36, 0x1B3A},
    {0x1B3C, 0x1B3C},
    {0x1B42, 0x1B42},
    {0x1B6B, 0x1B73},
    {0x1B80, 0x1B81},   // Sundanese
    {0x1BA2, 0x1BA5},   // Batak
    {0x1BA8, 0x1BA9},
    {0x1BAB, 0x1BAD},
    {0x1BE6, 0x1BE6},
    {0x1BE8, 0x1BE9},
    {0x1BED, 0x1BED},
    {0x1BEF, 0x1BF1},
    {0x1C2C, 0x1C33},   // Lepcha
    {0x1C36, 0x1C37},
    {0x1CD0, 0x1CD2},   // Vedic Extensions
    {0x1CD4, 0x1CE0},
    {0x1CE2, 0x1CE8},
    {0x1CED, 0x1CED},
    {0x1CF4, 0x1CF4},
    {0x1CF8, 0x1CF9},
    {0x1DC0, 0x1DFF},   // Combining Diacritical Marks Supplement
    {0x200B, 0x200D},   // ZWSP, ZWNJ, ZWJ (zero-width; ZWJ extends clusters)
    {0x202A, 0x202E},   // Bidi controls (LRE/RLE/PDF/LRO/RLO)
    {0x2060, 0x2064},   // Word Joiner and invisible operators
    {0x206A, 0x206F},   // Deprecated formatting characters
    {0x20D0, 0x20F0},   // Combining Diacritical Marks for Symbols
    {0x2CEF, 0x2CF1},   // Coptic combining
    {0x2D7F, 0x2D7F},   // Tifinagh
    {0x2DE0, 0x2DFF},   // Cyrillic combining
    {0x302A, 0x302D},   // CJK combining
    {0x3099, 0x309A},   // Combining dakuten / handakuten
    {0xA66F, 0xA672},   // Combining Cyrillic
    {0xA674, 0xA67D},
    {0xA69E, 0xA69F},
    {0xA6F0, 0xA6F1},   // Bamum
    {0xA802, 0xA802},   // Syloti Nagri
    {0xA806, 0xA806},
    {0xA80B, 0xA80B},
    {0xA825, 0xA826},   // Saurashtra
    {0xA82C, 0xA82C},
    {0xA8C4, 0xA8C5},   // Saurashtra
    {0xA8E0, 0xA8F1},   // Devanagari Extended
    {0xA8FF, 0xA8FF},
    {0xA926, 0xA92D},   // Kayah Li
    {0xA947, 0xA951},   // Rejang
    {0xA980, 0xA982},   // Javanese
    {0xA9B3, 0xA9B3},
    {0xA9B6, 0xA9B9},
    {0xA9BC, 0xA9BD},
    {0xA9E5, 0xA9E5},   // Myanmar Extended-B
    {0xAA29, 0xAA2E},   // Cham
    {0xAA31, 0xAA32},
    {0xAA35, 0xAA36},
    {0xAA43, 0xAA43},
    {0xAA4C, 0xAA4C},
    {0xAA7C, 0xAA7C},   // Myanmar Extended-A
    {0xAAB0, 0xAAB0},   // Tai Viet
    {0xAAB2, 0xAAB4},
    {0xAAB7, 0xAAB8},
    {0xAABE, 0xAABF},
    {0xAAC1, 0xAAC1},
    {0xAAEC, 0xAAED},   // Meetei Mayek
    {0xAAF6, 0xAAF6},
    {0xABE5, 0xABE5},   // Meetei Mayek Extensions
    {0xABE8, 0xABE8},
    {0xABED, 0xABED},
    {0xFB1E, 0xFB1E},   // Hebrew point judeo-spanish varika
    {0xFE00, 0xFE0F},   // Variation Selectors VS1–VS16
    {0xFE20, 0xFE2F},   // Combining Half Marks
    {0xFEFF, 0xFEFF},   // BOM / Zero-Width No-Break Space
    {0x101FD, 0x101FD}, // Phaistos Disc combining
    {0x102E0, 0x102E0}, // Coptic Epact combining
    {0x10376, 0x1037A}, // Old Permic combining
    {0x10A01, 0x10A03}, // Kharoshthi vowels
    {0x10A05, 0x10A06},
    {0x10A0C, 0x10A0F},
    {0x10A38, 0x10A3A},
    {0x10A3F, 0x10A3F},
    {0x10AE5, 0x10AE6}, // Manichaean
    {0x10D24, 0x10D27}, // Hanifi Rohingya
    {0x10EAB, 0x10EAC}, // Yezidi
    {0x10EFD, 0x10EFF}, // Arabic Extended-C
    {0x10F46, 0x10F50}, // Sogdian
    {0x10F82, 0x10F85}, // Old Uyghur
    {0x11001, 0x11001}, // Brahmi
    {0x11038, 0x11046},
    {0x11070, 0x11070},
    {0x11073, 0x11074},
    {0x1107F, 0x11081}, // Kaithi
    {0x110B3, 0x110B6},
    {0x110B9, 0x110BA},
    {0x110C2, 0x110C2},
    {0x11100, 0x11102}, // Chakma
    {0x11127, 0x1112B},
    {0x1112D, 0x11134},
    {0x11173, 0x11173}, // Mahajani
    {0x11180, 0x11181}, // Sharada
    {0x111B6, 0x111BE},
    {0x111C9, 0x111CC},
    {0x111CF, 0x111CF},
    {0x1122F, 0x11231}, // Khojki
    {0x11234, 0x11234},
    {0x11236, 0x11237},
    {0x1123E, 0x1123E},
    {0x11241, 0x11241},
    {0x112DF, 0x112DF}, // Khudawadi
    {0x112E3, 0x112EA},
    {0x11300, 0x11301}, // Grantha
    {0x1133B, 0x1133C},
    {0x11340, 0x11340},
    {0x11366, 0x1136C},
    {0x11370, 0x11374},
    {0x11438, 0x1143F}, // Newa
    {0x11442, 0x11444},
    {0x11446, 0x11446},
    {0x1145E, 0x1145E},
    {0x114B3, 0x114B8}, // Tirhuta
    {0x114BA, 0x114BA},
    {0x114BF, 0x114C0},
    {0x114C2, 0x114C3},
    {0x115B2, 0x115B5}, // Siddham
    {0x115BC, 0x115BD},
    {0x115BF, 0x115C0},
    {0x115DC, 0x115DD},
    {0x11633, 0x1163A}, // Modi
    {0x1163D, 0x1163D},
    {0x1163F, 0x11640},
    {0x116AB, 0x116AB}, // Takri
    {0x116AD, 0x116AD},
    {0x116B0, 0x116B5},
    {0x116B7, 0x116B7},
    {0x1171D, 0x1171F}, // Ahom
    {0x11722, 0x11725},
    {0x11727, 0x1172B},
    {0x1182F, 0x11837}, // Dogra
    {0x11839, 0x1183A},
    {0x1193B, 0x1193C}, // Dives Akuru
    {0x1193E, 0x1193E},
    {0x11943, 0x11943},
    {0x119D4, 0x119D7}, // Nandinagari
    {0x119DA, 0x119DB},
    {0x119E0, 0x119E0},
    {0x11A01, 0x11A0A}, // Zanabazar Square
    {0x11A33, 0x11A38},
    {0x11A3B, 0x11A3E},
    {0x11A47, 0x11A47},
    {0x11A51, 0x11A56}, // Soyombo
    {0x11A59, 0x11A5B},
    {0x11A8A, 0x11A96}, // Pau Cin Hau
    {0x11A98, 0x11A99},
    {0x11C30, 0x11C36}, // Bhaiksuki
    {0x11C38, 0x11C3D},
    {0x11C3F, 0x11C3F},
    {0x11C92, 0x11CA7}, // Marchen
    {0x11CAA, 0x11CB0},
    {0x11CB2, 0x11CB3},
    {0x11CB5, 0x11CB6},
    {0x11D31, 0x11D36}, // Masaram Gondi
    {0x11D3A, 0x11D3A},
    {0x11D3C, 0x11D3D},
    {0x11D3F, 0x11D45},
    {0x11D47, 0x11D47},
    {0x11D90, 0x11D91}, // Gunjala Gondi
    {0x11D95, 0x11D95},
    {0x11D97, 0x11D97},
    {0x11EF3, 0x11EF4}, // Makasar
    {0x11F00, 0x11F01}, // Kawi
    {0x11F36, 0x11F3A},
    {0x11F40, 0x11F40},
    {0x11F42, 0x11F42},
    {0x13440, 0x13440}, // Egyptian Hieroglyph combining
    {0x13447, 0x13455},
    {0x16AF0, 0x16AF4}, // Bassa Vah
    {0x16B30, 0x16B36}, // Pahawh Hmong
    {0x16F4F, 0x16F4F}, // Miao
    {0x16F8F, 0x16F92},
    {0x16FE4, 0x16FE4}, // Khitan Small Script
    {0x1BC9D, 0x1BC9E}, // Duployan
    {0x1CF00, 0x1CF2D}, // Znamenny combining
    {0x1CF30, 0x1CF46},
    {0x1D167, 0x1D169}, // Musical combining
    {0x1D17B, 0x1D182},
    {0x1D185, 0x1D18B},
    {0x1D1AA, 0x1D1AD},
    {0x1D242, 0x1D244}, // Combining Greek Musical Symbols
    {0x1DA00, 0x1DA36}, // Sutton SignWriting
    {0x1DA3B, 0x1DA6C},
    {0x1DA75, 0x1DA75},
    {0x1DA84, 0x1DA84},
    {0x1DA9B, 0x1DA9F},
    {0x1DAA1, 0x1DAAF},
    {0x1E000, 0x1E006}, // Glagolitic combining
    {0x1E008, 0x1E018},
    {0x1E01B, 0x1E021},
    {0x1E023, 0x1E024},
    {0x1E026, 0x1E02A},
    {0x1E08F, 0x1E08F}, // Cyrillic combining (Unicode 15.0 addition)
    {0x1E130, 0x1E136}, // Nyiakeng Puachue Hmong
    {0x1E2AE, 0x1E2AE}, // Toto
    {0x1E2EC, 0x1E2EF}, // Wancho
    {0x1E4EC, 0x1E4EF}, // Nag Mundari
    {0x1E8D0, 0x1E8D6}, // Mende Kikakui
    {0x1E944, 0x1E94A}, // Adlam
    {0x1F3FB, 0x1F3FF}, // Emoji Modifier Fitzpatrick (GCB=Extend per UAX #29)
    {0xE0001, 0xE0001}, // Language Tag (deprecated)
    {0xE0020, 0xE007F}, // Tag characters
    {0xE0100, 0xE01EF}, // Variation Selectors Supplement VS17–VS256
};

static constexpr int k_combining_n =
    static_cast<int>(sizeof(k_combining) / sizeof(k_combining[0]));

// ---------------------------------------------------------------------------
// Unicode 15.0.0 — Wide code points (EAW = W or F)
//
// Source: EastAsianWidth.txt, emoji-data.txt, Unicode 15.0.0 (2022-09-13).
// These code points occupy 2 terminal columns.

static constexpr URange k_wide[] = {
    {0x1100, 0x115F},   // Hangul Jamo leading consonants
    {0x231A, 0x231B},   // Watch, Hourglass
    {0x2329, 0x232A},   // CJK angle brackets
    {0x23E9, 0x23F3},
    {0x23F8, 0x23FA},
    {0x25FD, 0x25FE},
    {0x2614, 0x2615},
    {0x2648, 0x2653},   // Zodiac signs
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
    {0x2E80, 0x2EFF},   // CJK Radicals Supplement
    {0x2F00, 0x2FDF},   // Kangxi Radicals
    {0x2FF0, 0x2FFB},   // Ideographic Description Characters
    {0x3000, 0x303F},   // CJK Symbols and Punctuation
    {0x3040, 0x33FF},   // Hiragana through CJK Compatibility
    {0x3400, 0x4DBF},   // CJK Unified Ideographs Extension A
    {0x4E00, 0x9FFF},   // CJK Unified Ideographs
    {0xA000, 0xA4CF},   // Yi
    {0xA960, 0xA97C},   // Hangul Jamo Extended-A
    {0xAC00, 0xD7A3},   // Hangul Syllables
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0xFE10, 0xFE19},   // Vertical Forms
    {0xFE30, 0xFE4F},   // CJK Compatibility Forms
    {0xFF01, 0xFF60},   // Fullwidth ASCII variants
    {0xFFE0, 0xFFE6},   // Fullwidth signs
    {0x16FE0, 0x16FE4}, // Tangut / Khitan iteration marks (wide)
    {0x16FF0, 0x16FF1},
    {0x17000, 0x187F7}, // Tangut
    {0x18800, 0x18CD5}, // Tangut Components
    {0x18D00, 0x18D08}, // Tangut Supplement
    {0x1AFF0, 0x1AFF3}, // Katakana Phonetic Extensions
    {0x1AFF5, 0x1AFFB},
    {0x1AFFD, 0x1AFFE},
    {0x1B000, 0x1B122}, // Kana Supplement / Extended
    {0x1B132, 0x1B132},
    {0x1B150, 0x1B152}, // Small Kana Extension
    {0x1B155, 0x1B155},
    {0x1B164, 0x1B167},
    {0x1B170, 0x1B2FF}, // Nushu
    {0x1F004, 0x1F004}, // Mahjong Tile Red Dragon
    {0x1F0CF, 0x1F0CF}, // Joker
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F1E0, 0x1F1FF}, // Regional Indicator Symbols
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
    {0x1F6F0, 0x1F6FC},
    {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF},
    {0x1FA00, 0x1FA6F},
    {0x1FA70, 0x1FA7C},
    {0x1FA80, 0x1FA88},
    {0x1FA90, 0x1FABD},
    {0x1FABF, 0x1FAC5},
    {0x1FACE, 0x1FADB},
    {0x1FAE0, 0x1FAE8},
    {0x1FAF0, 0x1FAF8},
    {0x20000, 0x2FFFD}, // CJK Extension B through F
    {0x30000, 0x3FFFD}, // CJK Extension G, H
};

static constexpr int k_wide_n =
    static_cast<int>(sizeof(k_wide) / sizeof(k_wide[0]));

// ---------------------------------------------------------------------------
// Unicode 15.0.0 — SpacingMark code points (GCB=SpacingMark, i.e., Mc subset)
//
// These code points extend the preceding grapheme cluster (GB9a) and add
// 0 terminal cells.  They are spacing combining marks (category Mc) that
// visually modify a base character within the same terminal cell.
// Source: GraphemeBreakProperty.txt, Unicode 15.0.0 (2022-09-13).

static constexpr URange k_spacing_mark[] = {
    {0x0903, 0x0903},   // Devanagari sign visarga
    {0x093B, 0x093B},   // Devanagari vowel sign OOE
    {0x093E, 0x0940},   // Devanagari vowel signs AA/I/II
    {0x0949, 0x094C},   // Devanagari vowel signs O/OO/AU
    {0x094E, 0x094F},   // Devanagari vowel signs OE/OOE
    {0x0982, 0x0983},   // Bengali
    {0x09BE, 0x09C0},   // Bengali vowel signs
    {0x09C7, 0x09C8},   // Bengali
    {0x09CB, 0x09CC},   // Bengali
    {0x09D7, 0x09D7},   // Bengali AU length mark
    {0x0A03, 0x0A03},   // Gurmukhi
    {0x0A3E, 0x0A40},   // Gurmukhi
    {0x0A83, 0x0A83},   // Gujarati
    {0x0ABE, 0x0AC0},   // Gujarati
    {0x0AC9, 0x0AC9},
    {0x0ACB, 0x0ACC},   // Gujarati
    {0x0B02, 0x0B03},   // Oriya
    {0x0B3E, 0x0B3E},
    {0x0B40, 0x0B40},
    {0x0B47, 0x0B48},
    {0x0B4B, 0x0B4C},
    {0x0B57, 0x0B57},
    {0x0BBE, 0x0BBF},   // Tamil
    {0x0BC1, 0x0BC2},
    {0x0BC6, 0x0BC8},
    {0x0BCA, 0x0BCC},
    {0x0BD7, 0x0BD7},
    {0x0C01, 0x0C03},   // Telugu
    {0x0C41, 0x0C44},
    {0x0C82, 0x0C83},   // Kannada
    {0x0CBE, 0x0CBE},
    {0x0CC0, 0x0CC4},
    {0x0CC7, 0x0CC8},
    {0x0CCA, 0x0CCB},
    {0x0CD5, 0x0CD6},
    {0x0D02, 0x0D03},   // Malayalam
    {0x0D3E, 0x0D40},
    {0x0D46, 0x0D48},
    {0x0D4A, 0x0D4C},
    {0x0D57, 0x0D57},
    {0x0D82, 0x0D83},   // Sinhala
    {0x0DCF, 0x0DD1},
    {0x0DD8, 0x0DDF},
    {0x0DF2, 0x0DF3},
    {0x0E33, 0x0E33},   // Thai SARA AM
    {0x0EB3, 0x0EB3},   // Lao
    {0x0F3E, 0x0F3F},   // Tibetan
    {0x0F7F, 0x0F7F},
    {0x102B, 0x102C},   // Myanmar
    {0x1031, 0x1031},
    {0x1038, 0x1038},
    {0x103B, 0x103C},
    {0x1056, 0x1057},
    {0x1062, 0x1064},
    {0x1067, 0x106D},
    {0x1083, 0x1084},
    {0x1087, 0x108C},
    {0x108F, 0x108F},
    {0x109A, 0x109C},
    {0x1A61, 0x1A61},   // Tai Tham
    {0x1A63, 0x1A64},
    {0x1A6D, 0x1A72},
    {0x1B04, 0x1B04},   // Balinese
    {0x1B35, 0x1B35},
    {0x1B3B, 0x1B3B},
    {0x1B3D, 0x1B41},
    {0x1B43, 0x1B44},
    {0x1B82, 0x1B82},   // Sundanese
    {0x1BA1, 0x1BA1},   // Batak
    {0x1BA6, 0x1BA7},
    {0x1BAA, 0x1BAA},
    {0x1BE7, 0x1BE7},
    {0x1BEA, 0x1BEC},
    {0x1BEE, 0x1BEE},
    {0x1BF2, 0x1BF3},
    {0x1C24, 0x1C2B},   // Lepcha
    {0x1C34, 0x1C35},
    {0x1CE1, 0x1CE1},   // Vedic
    {0x1CF7, 0x1CF7},
    {0x302E, 0x302F},   // CJK tone marks
    {0xA823, 0xA824},   // Sylheti Nagri
    {0xA827, 0xA827},
    {0xA880, 0xA881},   // Saurashtra
    {0xA8B4, 0xA8C3},
    {0xA952, 0xA953},   // Rejang
    {0xA983, 0xA983},   // Javanese
    {0xA9B4, 0xA9B5},
    {0xA9BA, 0xA9BB},
    {0xA9BE, 0xA9C0},
    {0xAA2F, 0xAA30},   // Cham
    {0xAA33, 0xAA34},
    {0xAA4D, 0xAA4D},
    {0xAA7B, 0xAA7B},   // Myanmar Extended-A
    {0xAA7D, 0xAA7D},
    {0xAAEB, 0xAAEB},   // Meetei Mayek
    {0xAAEE, 0xAAEF},
    {0xAAF5, 0xAAF5},
    {0xABE3, 0xABE4},   // Meetei Mayek Extensions
    {0xABE6, 0xABE7},
    {0xABE9, 0xABEA},
    {0xABEC, 0xABEC},
    {0x11000, 0x11000}, // Brahmi
    {0x11002, 0x11002},
    {0x11082, 0x11082}, // Kaithi
    {0x110B0, 0x110B2},
    {0x110B7, 0x110B8},
    {0x1112C, 0x1112C}, // Chakma
    {0x11145, 0x11146}, // Newa
    {0x11182, 0x11182}, // Sharada
    {0x111B3, 0x111B5},
    {0x111BF, 0x111C0},
    {0x111CE, 0x111CE},
    {0x1122C, 0x1122E}, // Khojki
    {0x11232, 0x11233},
    {0x11235, 0x11235},
    {0x112E0, 0x112E2}, // Khudawadi
    {0x11302, 0x11303}, // Grantha
    {0x1133E, 0x1133F},
    {0x11341, 0x11344},
    {0x11347, 0x11348},
    {0x1134B, 0x1134D},
    {0x11362, 0x11363},
    {0x11435, 0x11437}, // Newa
    {0x11440, 0x11441},
    {0x11445, 0x11445},
    {0x114B0, 0x114B2}, // Tirhuta
    {0x114B9, 0x114B9},
    {0x114BB, 0x114BE},
    {0x114C1, 0x114C1},
    {0x115AF, 0x115B1}, // Siddham
    {0x115B8, 0x115BB},
    {0x115BE, 0x115BE},
    {0x11630, 0x11632}, // Modi
    {0x1163B, 0x1163C},
    {0x1163E, 0x1163E},
    {0x116AC, 0x116AC}, // Takri
    {0x116AE, 0x116AF},
    {0x116B6, 0x116B6},
    {0x11720, 0x11721}, // Ahom
    {0x11726, 0x11726},
    {0x1182C, 0x1182E}, // Dogra
    {0x11838, 0x11838},
    {0x11930, 0x11935}, // Dives Akuru
    {0x11937, 0x11938},
    {0x1193D, 0x1193D},
    {0x11940, 0x11940},
    {0x11942, 0x11942},
    {0x119D1, 0x119D3}, // Nandinagari
    {0x119DC, 0x119DF},
    {0x119E4, 0x119E4},
    {0x11A39, 0x11A39}, // Zanabazar
    {0x11A57, 0x11A58}, // Soyombo
    {0x11A97, 0x11A97}, // Pau Cin Hau
    {0x11C2F, 0x11C2F}, // Bhaiksuki
    {0x11C3E, 0x11C3E},
    {0x11CA9, 0x11CA9}, // Marchen
    {0x11CB1, 0x11CB1},
    {0x11CB4, 0x11CB4},
    {0x11D8A, 0x11D8E}, // Masaram Gondi
    {0x11D93, 0x11D94},
    {0x11D96, 0x11D96},
    {0x11EF5, 0x11EF6}, // Makasar
    {0x11F03, 0x11F03}, // Kawi (Unicode 15.0)
    {0x11F34, 0x11F35},
    {0x11F3E, 0x11F3F},
    {0x11F41, 0x11F41},
    {0x16F51, 0x16F87}, // Miao
    {0x16FF0, 0x16FF1}, // Khitan Small Script
    {0x1D165, 0x1D166}, // Musical combining
    {0x1D16D, 0x1D172},
};

static constexpr int k_spacing_mark_n =
    static_cast<int>(sizeof(k_spacing_mark) / sizeof(k_spacing_mark[0]));

// ---------------------------------------------------------------------------
// Unicode 15.0.0 — Extended_Pictographic code points
//
// Used by GB11: ExtPic Extend* ZWJ × ExtPic.  Only code points with this
// property may continue an emoji ZWJ sequence.  Wide CJK characters are NOT
// Extended_Pictographic and must NOT be joined merely because they are wide.
// Source: emoji-data.txt, Unicode 15.0.0 (2022-09-13).
// https://unicode.org/Public/15.0.0/ucd/emoji/emoji-data.txt

static constexpr URange k_extpic[] = {
    {0x00A9, 0x00A9},   // © COPYRIGHT SIGN
    {0x00AE, 0x00AE},   // ® REGISTERED SIGN
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
    {0x1F1E0, 0x1F1FF}, // Regional Indicators (also ExtPic per emoji-data.txt)
    {0x1F201, 0x1F202},
    {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A},
    {0x1F250, 0x1F251},
    {0x1F300, 0x1F6FF}, // Misc symbols, emoticons, transport (incl. 1F468 man, 1F469 woman)
    {0x1F700, 0x1F77F}, // Alchemical Symbols
    {0x1F780, 0x1F7FF}, // Geometric Shapes Extended
    {0x1F800, 0x1F8FF}, // Supplemental Arrows-C
    {0x1F900, 0x1FA6F}, // Supplemental Symbols and Pictographs + Chess
    {0x1FA70, 0x1FAFF}, // Symbols and Pictographs Extended-A
};

static constexpr int k_extpic_n =
    static_cast<int>(sizeof(k_extpic) / sizeof(k_extpic[0]));

// ---------------------------------------------------------------------------
// Unicode 15.0.0 — Prepend code points (GCB=Prepend)
//
// GB9b: Prepend × [^(Control|CR|LF)].  A Prepend character starts a cluster
// and absorbs the following non-control code point into the same cluster.
// Source: GraphemeBreakProperty.txt, Unicode 15.0.0 (2022-09-13).
// https://unicode.org/Public/15.0.0/ucd/auxiliary/GraphemeBreakProperty.txt
// (15 ranges exactly matching the official file)

static constexpr URange k_prepend[] = {
    {0x0600, 0x0605},   // Arabic Number Signs (Cf)
    {0x06DD, 0x06DD},   // Arabic End of Ayah
    {0x070F, 0x070F},   // Syriac Abbreviation Mark (Cf)
    {0x0890, 0x0891},   // Arabic Pound/Piastre Marks (Cf)
    {0x08E2, 0x08E2},   // Arabic Disputed End of Ayah (Cf)
    {0x0D4E, 0x0D4E},   // Malayalam Letter Dot Reph
    {0x110BD, 0x110BD}, // Kaithi Number Sign
    {0x110CD, 0x110CD}, // Kaithi Number Sign Above
    {0x111C2, 0x111C3}, // Sharada sign jihvamuliya/upadhmaniya
    {0x1193F, 0x1193F}, // Dives Akuru prefixed nasal sign
    {0x11941, 0x11941}, // Dives Akuru initial ra
    {0x11A3A, 0x11A3A}, // Zanabazar Square cluster-initial letter ra
    {0x11A84, 0x11A89}, // Zanabazar Square sign gvang/etc.
    {0x11D46, 0x11D46}, // Masaram Gondi repha
    {0x11F02, 0x11F02}, // Kawi sign repha (Unicode 15.0)
};

static constexpr int k_prepend_n =
    static_cast<int>(sizeof(k_prepend) / sizeof(k_prepend[0]));

// ---------------------------------------------------------------------------
// Unicode property predicates

static bool is_combining(uint32_t cp) noexcept {
    return in_ranges(k_combining, k_combining_n, cp);
}

static bool is_spacing_mark(uint32_t cp) noexcept {
    return in_ranges(k_spacing_mark, k_spacing_mark_n, cp);
}

static bool is_extended_pictographic(uint32_t cp) noexcept {
    return in_ranges(k_extpic, k_extpic_n, cp);
}

static bool is_prepend(uint32_t cp) noexcept {
    return in_ranges(k_prepend, k_prepend_n, cp);
}

static bool is_wide(uint32_t cp) noexcept {
    return in_ranges(k_wide, k_wide_n, cp);
}

// Regional Indicator Symbols: U+1F1E0–U+1F1FF.
// Two consecutive RIs form one flag emoji cluster (GB12/GB13).
static bool is_regional_indicator(uint32_t cp) noexcept {
    return cp >= 0x1F1E0u && cp <= 0x1F1FFu;
}

// ---------------------------------------------------------------------------
// Hangul Jamo and Syllable classification (GB6–GB8)
//
// Hangul L (leading consonant jamo): U+1100–U+115F, U+A960–U+A97C
// Hangul V (vowel jamo):             U+1160–U+11A7, U+D7B0–U+D7C6
// Hangul T (trailing consonant):     U+11A8–U+11FF, U+D7CB–U+D7FB
// Hangul LV syllable:  U+AC00–U+D7A3 where (cp-AC00)%28==0
// Hangul LVT syllable: U+AC00–U+D7A3 where (cp-AC00)%28!=0

static bool is_hangul_l(uint32_t cp) noexcept {
    return (cp >= 0x1100u && cp <= 0x115Fu) ||
           (cp >= 0xA960u && cp <= 0xA97Cu);
}
static bool is_hangul_v(uint32_t cp) noexcept {
    return (cp >= 0x1160u && cp <= 0x11A7u) ||
           (cp >= 0xD7B0u && cp <= 0xD7C6u);
}
static bool is_hangul_t(uint32_t cp) noexcept {
    return (cp >= 0x11A8u && cp <= 0x11FFu) ||
           (cp >= 0xD7CBu && cp <= 0xD7FBu);
}
static bool is_hangul_lv(uint32_t cp) noexcept {
    return (cp >= 0xAC00u && cp <= 0xD7A3u) &&
           ((cp - 0xAC00u) % 28u == 0u);
}
static bool is_hangul_lvt(uint32_t cp) noexcept {
    return (cp >= 0xAC00u && cp <= 0xD7A3u) &&
           ((cp - 0xAC00u) % 28u != 0u);
}

// GCB type for Hangul cluster-extension state machine.
// None: not a Hangul code point (or context cleared by intervening non-Hangul).
enum class HangulGCB : uint8_t { None, L, V, T, LV, LVT };

static HangulGCB hangul_gcb_of(uint32_t cp) noexcept {
    if (is_hangul_l(cp))   return HangulGCB::L;
    if (is_hangul_lv(cp))  return HangulGCB::LV;
    if (is_hangul_lvt(cp)) return HangulGCB::LVT;
    if (is_hangul_v(cp))   return HangulGCB::V;
    if (is_hangul_t(cp))   return HangulGCB::T;
    return HangulGCB::None;
}

// Returns true when a Hangul code point of type `next` may extend a cluster
// whose last non-Extend code point had type `last`.
static bool hangul_extends(HangulGCB last, HangulGCB next) noexcept {
    switch (last) {
    case HangulGCB::L:
        // GB6: L × (L | V | LV | LVT)
        return next == HangulGCB::L   || next == HangulGCB::V  ||
               next == HangulGCB::LV  || next == HangulGCB::LVT;
    case HangulGCB::LV:
    case HangulGCB::V:
        // GB7: (LV | V) × (V | T)
        return next == HangulGCB::V || next == HangulGCB::T;
    case HangulGCB::LVT:
    case HangulGCB::T:
        // GB8: (LVT | T) × T
        return next == HangulGCB::T;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// UTF-8 decoder

struct DecodeResult {
    uint32_t codepoint; // Decoded code point (0 when invalid)
    uint32_t byte_len;  // Bytes this result consumed (always >= 1)
    bool     valid;     // True when a well-formed sequence was decoded
};

// Decode one UTF-8 sequence starting at data[pos].  end is the exclusive
// limit.  On invalid input, returns {0, 1, false} — the caller must advance
// by exactly 1 byte and emit one invalid_utf8 span per call.
//
// Rejects: continuation bytes at sequence start, overlong encodings (0xC0/C1
// leads, 0xE0 requiring second byte < 0xA0, 0xF0 requiring second byte <
// 0x90), surrogates (0xED second byte >= 0xA0), out-of-range leads (> 0xF4),
// and truncated sequences (not enough continuation bytes before end).
static DecodeResult decode_utf8(const uint8_t* data,
                                size_t          pos,
                                size_t          end) noexcept {
    const uint8_t b0 = data[pos];

    // ASCII fast path
    if (b0 < 0x80)
        return {b0, 1, true};

    // Continuation byte at sequence start, or invalid byte 0xFF
    if (b0 < 0xC2 || b0 > 0xF4)
        return {0, 1, false};

    // 2-byte: 0xC2–0xDF
    if (b0 < 0xE0) {
        if (pos + 2 > end)          return {0, 1, false}; // truncated
        const uint8_t b1 = data[pos + 1];
        if (b1 < 0x80 || b1 > 0xBF) return {0, 1, false}; // bad continuation
        return {static_cast<uint32_t>((b0 & 0x1Fu) << 6 | (b1 & 0x3Fu)), 2, true};
    }

    // 3-byte: 0xE0–0xEF
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

    // 4-byte: 0xF0–0xF4
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

// ---------------------------------------------------------------------------
// Public API

CellRun compute_cell_run(std::string_view line_utf8, int tab_width) {
    CellRun result;
    result.total_cells = 0;

    const auto* data = reinterpret_cast<const uint8_t*>(line_utf8.data());
    const size_t end  = line_utf8.size();
    size_t pos        = 0;
    uint32_t cur_cell = 0; // Running column count, used for tab expansion

    while (pos < end) {
        const size_t cluster_start = pos;

        // Decode base code point
        const DecodeResult base = decode_utf8(data, pos, end);

        if (!base.valid) {
            // Each invalid byte becomes its own 1-cell invalid span
            result.spans.push_back({
                static_cast<uint32_t>(cluster_start), 1u, 1u,
                CellKind::invalid_utf8
            });
            result.total_cells += 1;
            cur_cell            += 1;
            pos                 += 1;
            continue;
        }

        const uint32_t cp = base.codepoint;
        pos += base.byte_len;

        // Tab: advance to next tab stop, minimum 1 column
        if (cp == 0x09u) {
            const uint32_t tw      = static_cast<uint32_t>(tab_width);
            const uint32_t advance = tw - (cur_cell % tw);
            result.spans.push_back({
                static_cast<uint32_t>(cluster_start), 1u, advance, CellKind::tab
            });
            result.total_cells += advance;
            cur_cell            += advance;
            continue; // Tab is never extended
        }

        // C0 controls (except tab), DEL, C1 controls → 1-cell replacement glyph
        if (cp < 0x20u || cp == 0x7Fu || (cp >= 0x80u && cp <= 0x9Fu)) {
            result.spans.push_back({
                static_cast<uint32_t>(cluster_start),
                static_cast<uint32_t>(base.byte_len),
                1u, CellKind::control
            });
            result.total_cells += 1;
            cur_cell            += 1;
            continue; // Control chars are never extended
        }

        // Determine the base cluster kind, cell width, and Hangul GCB state.
        // Prepend bases start at width 0 and are updated when they absorb
        // their following character (GB9b).
        uint32_t  base_width;
        CellKind  kind;
        HangulGCB last_hgcb;
        bool      is_prepend_base;

        if (is_combining(cp) || is_spacing_mark(cp)) {
            // Lone combining or spacing mark at line start (no preceding base)
            base_width      = 0u;
            kind            = CellKind::combining;
            last_hgcb       = HangulGCB::None;
            is_prepend_base = false;
        } else if (is_prepend(cp)) {
            // GB9b: Prepend character; width is set when following char absorbed
            base_width      = 0u;
            kind            = CellKind::text;
            last_hgcb       = HangulGCB::None;
            is_prepend_base = true;
        } else if (is_wide(cp)) {
            base_width      = 2u;
            kind            = CellKind::text;
            last_hgcb       = hangul_gcb_of(cp); // handles LV/LVT syllables
            is_prepend_base = false;
        } else {
            base_width      = 1u;
            kind            = CellKind::text;
            last_hgcb       = hangul_gcb_of(cp); // handles L/V/T jamo
            is_prepend_base = false;
        }

        uint32_t cluster_len = base.byte_len;
        bool     after_zwj   = (cp == 0x200Du);
        bool     base_is_ri  = is_regional_indicator(cp);
        bool     ri_paired   = false;

        // Absorb extending code points into this grapheme cluster.
        // Rules applied in priority order (UAX #29, Unicode 15.0.0):
        //   GB9   — × (Extend | ZWJ): combining marks, modifiers, ZWJ
        //   GB9a  — × SpacingMark:    Indic/script spacing vowel signs
        //   GB11  — ExtPic Extend* ZWJ × ExtPic: emoji ZWJ sequences
        //   GB12/13 — RI × RI: regional indicator flag pairs
        //   GB6–8 — Hangul jamo/syllable composition
        //   GB9b  — Prepend × [^Control]: absorb following char
        while (pos < end) {
            const DecodeResult ext = decode_utf8(data, pos, end);
            if (!ext.valid) break;  // Invalid byte starts its own cluster

            const uint32_t ext_cp   = ext.codepoint;
            bool           extends  = false;
            bool           upd_hgcb = false;
            HangulGCB      ext_hgcb = HangulGCB::None;

            if (is_combining(ext_cp) || is_spacing_mark(ext_cp)) {
                // GB9: × (Extend | ZWJ)
                // GB9a: × SpacingMark
                // GB6–GB8 lack the Extend* qualifier: Extend/SpacingMark
                // absorptions sever Hangul composition (UAX #29 test data:
                // L × Extend ÷ V).  Reset Hangul context here so a following
                // jamo cannot compose across this Extend.
                extends   = true;
                last_hgcb = HangulGCB::None;
                after_zwj = (ext_cp == 0x200Du); // re-arm ZWJ tracking
            } else if (after_zwj && is_extended_pictographic(ext_cp)) {
                // GB11: Extended_Pictographic Extend* ZWJ × Extended_Pictographic.
                // Only ExtPic chars continue an emoji ZWJ sequence; wide CJK or
                // fullwidth Latin chars do NOT qualify (is_wide ≠ is_extpic).
                extends   = true;
                after_zwj = false;
            } else if (base_is_ri && !ri_paired && is_regional_indicator(ext_cp)) {
                // GB12/GB13: second Regional Indicator completes a flag pair
                extends   = true;
                ri_paired = true;
                after_zwj = false;
            } else {
                // GB6–GB8: Hangul jamo/syllable composition.
                // These rules are checked before GB9b so that a Hangul sequence
                // starting after a Prepend is correctly composed.
                ext_hgcb = hangul_gcb_of(ext_cp);
                if (last_hgcb != HangulGCB::None &&
                    ext_hgcb  != HangulGCB::None &&
                    hangul_extends(last_hgcb, ext_hgcb)) {
                    extends   = true;
                    upd_hgcb  = true;
                    after_zwj = false;
                } else if (is_prepend_base) {
                    // GB9b: Prepend × [^Control].
                    // GCB-Control: C0 (< 0x20), DEL (0x7F), C1 (0x80–0x9F).
                    // CR/LF absent by precondition.
                    const bool is_gcb_ctrl =
                        (ext_cp < 0x20u) ||
                        (ext_cp == 0x7Fu) ||
                        (ext_cp >= 0x80u && ext_cp <= 0x9Fu);
                    if (!is_gcb_ctrl) {
                        extends   = true;
                        after_zwj = false;
                        // If absorbed char is not itself a Prepend: finalise width.
                        if (!is_prepend(ext_cp)) {
                            is_prepend_base = false;
                            if (is_wide(ext_cp))       base_width = 2u;
                            else if (!is_combining(ext_cp) &&
                                     !is_spacing_mark(ext_cp)) base_width = 1u;
                            // Update Hangul state for the newly absorbed base
                            ext_hgcb = hangul_gcb_of(ext_cp);
                            upd_hgcb = (ext_hgcb != HangulGCB::None);
                        }
                        // If it is another Prepend: is_prepend_base stays true
                    }
                }
            }

            if (!extends) break;

            cluster_len += ext.byte_len;
            pos         += ext.byte_len;
            if (upd_hgcb) last_hgcb = ext_hgcb;
        }

        result.spans.push_back({
            static_cast<uint32_t>(cluster_start),
            cluster_len,
            base_width,
            kind
        });
        result.total_cells += base_width;
        cur_cell            += base_width;
    }

    return result;
}

}  // namespace ssg
