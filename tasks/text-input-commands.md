# text-input-commands

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `selection-navigation`, `settings-model`
- Branch: `text-input-commands-task`

## Scope

Implement `text.insert`, newline, backward/forward character and word deletion,
selection replacement, multi-caret insertion, and automatic indentation as one
`CommandSet`.

## Files

`include/ssg/text_input_commands.h`, `src/text_input_commands.cpp`,
`tests/test_text_input_commands.cpp`,
`cmake/components/text-input-commands.cmake`

## Oracle

Independent hand fixtures cover single/multiple carets, selected replacement,
line endings, indentation settings, grapheme deletion, and overlapping edit
normalization.

## Done

The mandatory workflow is complete without history coalescing or line
transforms.
