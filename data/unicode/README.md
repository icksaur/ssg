# Unicode data for SSG cell layout

Pinned Unicode version: **15.0.0** (released 2022-09-13).

## Files

| File | Description |
|---|---|
| `east_asian_width.txt` | Wide (EAW = W or F) code point ranges used in `src/layout.cpp:k_wide[]` |
| `combining_zero_width.txt` | Combining / zero-width code point ranges used in `src/layout.cpp:k_combining[]` |

## Pinning contract

The C++ arrays `k_wide` and `k_combining` in `src/layout.cpp` are the
authoritative data for the running binary.  The text files here document the
source ranges so that a future Unicode-version upgrade can diff the new
`EastAsianWidth.txt` and `DerivedCoreProperties.txt` against the current text
files and update the C++ arrays and fixture goldens together.

## Algorithms

- **Grapheme clusters:** UAX #29 extended grapheme clusters (Unicode 15.0.0).
  Implemented rules: GB9 (× Extend/ZWJ), GB11 (emoji ZWJ sequences),
  GB12/GB13 (Regional Indicator flag pairs).  Emoji modifier sequences use
  GB9 (modifier has Extend property).

- **Display width:** UAX #11 East Asian Width.  EAW = W or F → 2 cells.
  Emoji presentation sequences (base + VS-16) use the base character's width
  as the cluster width (VS-16 is in the zero-width/combining table and adds 0
  cells).

## Update procedure

1. Download the new `EastAsianWidth.txt` and `DerivedCoreProperties.txt` from
   `https://unicode.org/Public/<version>/ucd/`.
2. Diff against the ranges listed in `east_asian_width.txt` and
   `combining_zero_width.txt` below.
3. Update `src/layout.cpp:k_wide[]` and `src/layout.cpp:k_combining[]`.
4. Update the pinned version comment in `src/layout.cpp`.
5. Re-run the `test_cell_layout` oracle gate; update any affected golden
   values in `tests/fixtures/layout/cells/`.
6. Re-author any fixtures whose expected values changed.
