# text-input-commands

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `document-transactions`, `selection-navigation`, `settings-model`,
  `unicode-cell-layout`
- Branch: `text-input-commands-task`

## Scope

Implement `text.insert`, newline, backward/forward character and word deletion,
selection replacement, multi-caret insertion, and automatic indentation as one
immutable feature `CommandSet`. The set owns descriptors and a pure apply
function; session assembly later binds it to live document and selection state.

## Files

`include/ssg/text_input_commands.h`, `src/text_input_commands.cpp`,
`tests/test_text_input_commands.cpp`,
`cmake/components/text-input-commands.cmake`

## Oracle

Independent hand fixtures cover single/multiple carets, selected replacement,
line endings, indentation settings, grapheme deletion, and overlapping edit
normalization.

Reference-editor parity is restricted to ASCII/single-codepoint input with LF
and automatic indentation disabled. Hand fixtures are authoritative for
grapheme clusters, CR/CRLF/mixed line endings, and automatic indentation.

## Done

The mandatory workflow is complete without history coalescing or line
transforms.
