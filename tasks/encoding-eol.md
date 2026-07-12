# encoding-eol

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 3
- Depends: `platform-file-io`
- Branch: `encoding-eol-task`

## Scope

Implement decode/encode and status metadata for UTF-8, BOM variants,
UTF-16LE/BE, Windows-1252, ISO-8859-1, and mixed line endings, and own
`file.reopen_with_encoding`, `file.set_encoding`, `file.set_line_ending`, and
`file.set_final_newline` command sets.

## Files

`include/ssg/text_encoding.h`, `src/text_encoding.cpp`,
`tests/fixtures/encoding/`, `tests/test_text_encoding.cpp`,
`cmake/components/encoding-eol.cmake`

## Oracle

Byte-exact decode/encode/BOM/EOL round trips and explicit failure fixtures.

## Done

The mandatory workflow is complete without file-tab commands.
