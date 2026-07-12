# Unicode data for SSG cell layout

Pinned Unicode version: **15.0.0** (released 2022-09-13).

## Files

| File | Source URL | Used by |
|---|---|---|
| `east_asian_width.txt` | `https://unicode.org/Public/15.0.0/ucd/EastAsianWidth.txt` | `src/layout.cpp:k_wide[]` via `tools/gen_eaw_table.py` |
| `GraphemeBreakProperty.txt` | `https://unicode.org/Public/15.0.0/ucd/auxiliary/GraphemeBreakProperty.txt` | `src/layout.cpp:k_gcb[]` via `tools/gen_gcb_table.py` |
| `emoji-data.txt` | `https://unicode.org/Public/15.0.0/ucd/emoji/emoji-data.txt` | `src/layout.cpp:k_extpic[]`, `k_emoji_pres[]`, `k_emoji[]` via `tools/gen_extpic_table.py`, `tools/gen_emoji_props_table.py` |
| `GraphemeBreakTest.txt` | `https://unicode.org/Public/15.0.0/ucd/auxiliary/GraphemeBreakTest.txt` | `tests/test_gcb_oracle.cpp` (segmentation oracle) |

Official-file SHA-256 pins:

- `east_asian_width.txt`: `743e7bc435c04ab1a8459710b1c3cad56eedced5b806b4659b6e69b85d0adf2a`
- `GraphemeBreakProperty.txt`: `5a0f8748575432f8ff95e1dd5bfaa27bda1a844809e17d6939ee912bba6568a1`
- `emoji-data.txt`: `29071dba22c72c27783a73016afb8ffaeb025866740791f9c2d0b55cc45a3470`
- `GraphemeBreakTest.txt`: `0d2080d0def294a4b7660801cc03ddfe5866ff300c789c2cc1b50fd7802b2d97`

## Pinning contract

The C++ arrays `k_wide`, `k_emoji_pres`, `k_emoji`, `k_extpic`, and `k_gcb` in
`src/layout.cpp` are the authoritative data for the running binary.  The text
files here are the pinned official Unicode 15.0.0 source files so that a future
Unicode-version upgrade can diff the new files against these and update the C++
arrays and fixture goldens together.

`k_gcb` is generated from `GraphemeBreakProperty.txt` using `tools/gen_gcb_table.py`.
LV and LVT Hangul syllable entries (AC00–D7A3) are excluded from the table and
computed arithmetically in `gcb_prop_of()`.

`k_extpic` is generated from `emoji-data.txt` using `tools/gen_extpic_table.py`,
which merges all `Extended_Pictographic` ranges into a minimal non-overlapping sorted
`URange` array.  Running the script reproduces the table currently embedded in
`layout.cpp`.

`k_wide` is generated from `EastAsianWidth.txt` using `tools/gen_eaw_table.py`,
which extracts only EAW=W and EAW=F ranges (121 merged ranges).  It deliberately
excludes any Emoji-only or other-property ranges — width 2 for emoji characters
is determined by `k_emoji_pres[]` and VS-16 tracking, not `k_wide[]`.

`k_emoji_pres` and `k_emoji` are generated from `emoji-data.txt` using
`tools/gen_emoji_props_table.py`, which extracts `Emoji_Presentation` (81 ranges)
and `Emoji` (151 ranges) properties respectively.

## Algorithms

- **Grapheme clusters:** UAX #29 extended grapheme clusters (Unicode 15.0.0).
  Full GCB state machine using `k_gcb[]` table sourced from `GraphemeBreakProperty.txt`.
  Implemented rules: GB6–GB8 (Hangul), GB9 (× Extend/ZWJ), GB9a (× SpacingMark),
  GB9b (Prepend ×), GB11 (ExtPic Extend* ZWJ × ExtPic), GB12/GB13 (RI × RI).
  GB11 uses a three-state enum `{ None, ExtPic, Zwj }` that correctly handles
  double-ZWJ sequences and SpacingMark interruptions.

- **Display width:** UAX #11 East Asian Width plus Emoji properties.
  - EAW = W or F (`is_eaw_wide`) → 2 cells.
  - `Emoji_Presentation=Yes` (`is_emoji_pres`) → 2 cells (e.g. Regional Indicators,
    most emoji sequences).
  - `Emoji=Yes` + VS-16 (U+FE0F) absorbed into cluster → 2 cells (emoji presentation
    sequence for text-default emoji such as `#`, `*`, `0`–`9`, U+2702, etc.).
  - All other printable code points → 1 cell.
  - These three rules are orthogonal to segmentation.

- **GCB=Control outside C0/C1:** Code points with GCB=Control that are not in
  the C0 (U+0000–U+001F) or C1 (U+0080–U+009F) ranges — such as U+00AD (Soft
  Hyphen), U+200B (Zero Width Space), U+202A–202E (bidi controls), U+2060–206F
  (Word Joiners), and U+FEFF (BOM) — produce their own cluster with
  `kind=control, width=0`.  They are NOT absorbed into a preceding cluster, but
  they occupy no terminal column.  C0/DEL/C1 keep `width=1`.

## Update procedure

1. Download the new `EastAsianWidth.txt`, `GraphemeBreakProperty.txt`,
   `emoji-data.txt`, and `GraphemeBreakTest.txt` from
   `https://unicode.org/Public/<version>/ucd/`.
2. Run `tools/gen_eaw_table.py data/unicode/east_asian_width.txt` and
   update `k_wide[]` in `src/layout.cpp` with the output.
3. Run `tools/gen_gcb_table.py data/unicode/GraphemeBreakProperty.txt` and
   update `k_gcb[]` in `src/layout.cpp` with the output.
4. Run `tools/gen_extpic_table.py data/unicode/emoji-data.txt` and
   update `k_extpic[]` in `src/layout.cpp` with the output.
5. Run `tools/gen_emoji_props_table.py data/unicode/emoji-data.txt` and
   update `k_emoji_pres[]` and `k_emoji[]` in `src/layout.cpp` with the output.
6. Update the pinned SHA-256 hashes in this file and the version comment in
   `src/layout.cpp`.
7. Re-run all test gates: `cmake --build build && ctest --test-dir build`.
8. The `test_gcb_oracle` test validates against `GraphemeBreakTest.txt`
   automatically; check for any new failures.
9. Re-author any fixture goldens in `tests/test_cell_layout.cpp` whose
   expected values changed.
