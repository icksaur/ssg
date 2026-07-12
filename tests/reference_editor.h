#pragma once

// Reference editor — independent string-based oracle for SSG.
//
// This file and its companion reference_editor.cpp share NO editing,
// selection, or history code with the SSG library (spec invariant I13).
// The reference editor is a minimal std::string-based implementation used
// as the correctness oracle for SSG's piece-tree document and command
// implementations.  SSG's piece-tree tests run the same command scripts
// through both implementations and compare canonical state.
//
// All positions are zero-based UTF-8 byte offsets.  A Sel is an
// (anchor, active) pair: anchor is fixed, active is the cursor.
// Mutations apply from highest to lowest offset so lower positions remain
// valid throughout a multi-cursor transaction.
//
// This oracle deliberately does not include <ssg/...> headers; it defines
// its own Mode enum and Sel type to satisfy I13.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ref {

// ── Core types ─────────────────────────────────────────────────────────────

/// A single selection or caret within a UTF-8 document.
/// anchor: the fixed end.  active: the moving cursor end.
/// When anchor == active this is a zero-width caret.
struct Sel {
    size_t anchor = 0;
    size_t active = 0;

    [[nodiscard]] size_t lo() const noexcept {
        return anchor < active ? anchor : active;
    }
    [[nodiscard]] size_t hi() const noexcept {
        return anchor < active ? active : anchor;
    }
    [[nodiscard]] bool is_caret() const noexcept { return anchor == active; }

    bool operator==(Sel const&) const noexcept = default;
};

/// Document editing mode — mirrors ssg::DocumentMode but is defined
/// independently so the reference editor compiles without SSG headers.
enum class Mode : uint8_t { edit, read_only, diff };

/// Complete editor state.
struct Editor {
    std::string      text;
    std::vector<Sel> selections;  // sorted by lo(), non-overlapping

    Mode mode = Mode::edit;

    // Structured clipboard: one fragment per selection at copy time.
    // Paste distributes 1:1 when fragment count == cursor count; otherwise
    // every cursor receives the full concatenated payload.
    std::vector<std::string> clipboard;

    struct Snapshot {
        std::string      text;
        std::vector<Sel> selections;
    };
    std::vector<Snapshot> undo_stack;
    std::vector<Snapshot> redo_stack;
};

// ── Constructor helpers ────────────────────────────────────────────────────

/// Editor with the given text and a single caret at end.
[[nodiscard]] Editor make_editor(std::string text = {});

/// Editor with the given text and a single selection [anchor, active).
/// If anchor == active the result is a caret.
[[nodiscard]] Editor make_editor(std::string text, size_t anchor, size_t active);

// ── UTF-8 byte navigation ──────────────────────────────────────────────────

/// Byte offset just after the UTF-8 codepoint starting at pos.
/// Requires pos < text.size().
[[nodiscard]] size_t utf8_next(std::string_view text, size_t pos);

/// Byte offset of the start of the UTF-8 codepoint whose sequence ends just
/// before pos.  Requires pos > 0.
[[nodiscard]] size_t utf8_prev(std::string_view text, size_t pos);

// ── Line navigation ────────────────────────────────────────────────────────

/// Byte offset of the start of the line containing pos.
[[nodiscard]] size_t line_start(std::string_view text, size_t pos);

/// Byte offset of the end of the line containing pos: position of '\n' or
/// text.size() (the caret-valid position after the last character).
[[nodiscard]] size_t line_end(std::string_view text, size_t pos);

/// Byte offset of the start of the next line, or text.size() when on the
/// last line.
[[nodiscard]] size_t next_line_start(std::string_view text, size_t pos);

// ── Selection normalization ────────────────────────────────────────────────

/// Sort by lo(), merge overlapping ranges (forward direction for merged
/// results), deduplicate exact duplicates.  Adjacent non-overlapping
/// selections are preserved as distinct.
void normalize_selections(std::vector<Sel>& sels);

// ── Text mutation commands ─────────────────────────────────────────────────
// All return false without modifying state when mode != edit.

bool text_insert(Editor& ed, std::string_view s);
bool text_newline(Editor& ed);
bool text_delete_backward(Editor& ed);
bool text_delete_forward(Editor& ed);
bool text_delete_word_backward(Editor& ed);
bool text_delete_word_forward(Editor& ed);

// ── Cursor movement ────────────────────────────────────────────────────────
// Collapse all selections to carets; never push to the undo stack.

void cursor_set_position(Editor& ed, size_t byte_offset);
void cursor_left(Editor& ed);
void cursor_right(Editor& ed);
void cursor_word_left(Editor& ed);
void cursor_word_right(Editor& ed);
void cursor_line_start(Editor& ed);
void cursor_line_end(Editor& ed);
void cursor_line_up(Editor& ed);
void cursor_line_down(Editor& ed);
void cursor_doc_start(Editor& ed);
void cursor_doc_end(Editor& ed);

// ── Selection extension ────────────────────────────────────────────────────
// Move only the active end; anchor stays fixed.  Never push to the undo stack.

void select_set_range(Editor& ed, size_t anchor, size_t active);
void select_add_range(Editor& ed, size_t anchor, size_t active);
void select_left(Editor& ed);
void select_right(Editor& ed);
void select_word_left(Editor& ed);
void select_word_right(Editor& ed);
void select_line_start(Editor& ed);
void select_line_end(Editor& ed);
void select_line_up(Editor& ed);
void select_line_down(Editor& ed);
void select_doc_start(Editor& ed);
void select_doc_end(Editor& ed);
void select_all(Editor& ed);

/// Add a selection covering the next occurrence of the text in the last
/// selection.  No-op if the last selection is a caret or there is no next
/// occurrence.
void select_add_next_occurrence(Editor& ed);

/// Add a caret at the same column on the line above each existing caret.
void select_add_cursor_up(Editor& ed);

/// Add a caret at the same column on the line below each existing caret.
void select_add_cursor_down(Editor& ed);

/// Split each multi-line selection into one forward selection per line.
/// Single-line selections and carets are unchanged.
void select_split_into_lines(Editor& ed);

// ── Clipboard ─────────────────────────────────────────────────────────────

/// Copy selected text into the clipboard register.  For a caret, captures
/// the whole line including its terminator (or without if on the last line).
/// Does not modify text; no mode restriction; never pushes undo.
void clipboard_copy(Editor& ed);

/// Copy then delete selections.  Returns false (no-op) if mode != edit.
bool clipboard_cut(Editor& ed);

/// Paste clipboard fragments at each cursor.  Returns false (no-op) if
/// mode != edit.
bool clipboard_paste(Editor& ed);

// ── History ───────────────────────────────────────────────────────────────

/// Restore previous state.  Returns false if the undo stack is empty.
bool edit_undo(Editor& ed);

/// Restore next state.  Returns false if the redo stack is empty.
bool edit_redo(Editor& ed);

// ── Edit transforms ───────────────────────────────────────────────────────
// All return false without modifying state when mode != edit.

/// Add tab_width spaces (or one tab) at the start of every line touched by
/// any selection.
bool edit_indent(Editor& ed, int tab_width = 4, bool use_spaces = true);

/// Remove up to tab_width leading spaces (or one leading tab) from every
/// line touched by any selection.
bool edit_outdent(Editor& ed, int tab_width = 4, bool use_spaces = true);

/// Insert a copy of each cursor's line immediately below it; move cursors
/// to the start of the duplicated lines.
bool edit_duplicate_line(Editor& ed);

/// Exchange each cursor's line with the line above it.
bool edit_move_line_up(Editor& ed);

/// Exchange each cursor's line with the line below it.
bool edit_move_line_down(Editor& ed);

/// Delete every line touched by any selection.  Cursor lands on the line
/// that takes the deleted line's position (or document end).
bool edit_delete_line(Editor& ed);

/// For each cursor, replace the newline at the end of its line with a single
/// space, joining it with the next line.  No-op on the last line.
bool edit_join_lines(Editor& ed);

/// Replace each selected character with its uppercase equivalent (ASCII).
bool edit_uppercase(Editor& ed);

/// Replace each selected character with its lowercase equivalent (ASCII).
bool edit_lowercase(Editor& ed);

/// Toggle uppercase/lowercase for each selected character (ASCII).
bool edit_swap_case(Editor& ed);

/// Sort all lines that fall within any selection lexicographically.
bool edit_sort_lines(Editor& ed);

/// Swap the character just before each caret with the character just after it
/// (or the last two characters when at document end).  Advances the caret.
bool edit_transpose(Editor& ed);

/// Toggle line comments for every line touched by any selection using the
/// given prefix token.  If every touched line already starts with the token,
/// remove it; otherwise add it.
bool edit_toggle_comment(Editor& ed, std::string_view line_comment_token);

// ── Snapshot accessors ────────────────────────────────────────────────────

[[nodiscard]] std::string_view        snapshot_text(Editor const& ed) noexcept;
[[nodiscard]] std::vector<Sel> const& snapshot_selections(Editor const& ed) noexcept;

} // namespace ref
