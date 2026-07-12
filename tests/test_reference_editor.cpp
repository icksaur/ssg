// Oracle: tests/test_reference_editor.cpp
//
// Hand-computed command scripts for the reference editor.
//
// Every expected value in this file was computed by hand before the
// implementation was written and verified against the spec contracts.
// These cases constitute the oracle for SSG's piece-tree and command
// implementations (spec invariant I13).
//
// Structure: one TEST() per logical scenario; tests are grouped by command
// category; the main() at the bottom runs them all.

#include "reference_editor.h"
#include "test_helpers.h"

#include <string>
#include <string_view>
#include <vector>

using namespace ref;

// ── UTF-8 helpers ──────────────────────────────────────────────────────────

TEST(utf8_next_ascii) {
    // ASCII: each codepoint is one byte
    ASSERT_EQ(utf8_next("hello", 0), size_t{1});
    ASSERT_EQ(utf8_next("hello", 4), size_t{5});
}

TEST(utf8_next_multibyte) {
    // U+00E9 (é) encodes as 0xC3 0xA9 — 2 bytes
    std::string s = "\xC3\xA9!";
    ASSERT_EQ(utf8_next(s, 0), size_t{2}); // skip the 2-byte é
    ASSERT_EQ(utf8_next(s, 2), size_t{3}); // '!'
}

TEST(utf8_prev_ascii) {
    ASSERT_EQ(utf8_prev("hello", 5), size_t{4});
    ASSERT_EQ(utf8_prev("hello", 1), size_t{0});
}

TEST(utf8_prev_multibyte) {
    // "\xC3\xA9!" — position 2 (after é) should back up to 0
    std::string s = "\xC3\xA9!";
    ASSERT_EQ(utf8_prev(s, 2), size_t{0});
    ASSERT_EQ(utf8_prev(s, 3), size_t{2}); // back over '!'
}

// ── Line navigation ────────────────────────────────────────────────────────

TEST(line_start_first_line) {
    // "hello\nworld" — position anywhere on "hello" returns 0
    ASSERT_EQ(line_start("hello\nworld", 0), size_t{0});
    ASSERT_EQ(line_start("hello\nworld", 4), size_t{0});
}

TEST(line_start_second_line) {
    // "hello\nworld" — position anywhere on "world" returns 6
    ASSERT_EQ(line_start("hello\nworld", 6), size_t{6});
    ASSERT_EQ(line_start("hello\nworld", 9), size_t{6});
}

TEST(line_start_at_newline) {
    // Position at '\n' (index 5) is still on the first line
    ASSERT_EQ(line_start("hello\nworld", 5), size_t{0});
}

TEST(line_end_first_line) {
    // line_end returns position of '\n' (index 5) for "hello"
    ASSERT_EQ(line_end("hello\nworld", 0), size_t{5});
    ASSERT_EQ(line_end("hello\nworld", 3), size_t{5});
}

TEST(line_end_last_line) {
    // Last line has no '\n'; line_end returns text.size()
    ASSERT_EQ(line_end("hello\nworld", 6), size_t{11});
    ASSERT_EQ(line_end("hello\nworld", 11), size_t{11});
}

TEST(next_line_start_basic) {
    // next_line_start from anywhere on "hello" jumps to 6
    ASSERT_EQ(next_line_start("hello\nworld", 0), size_t{6});
    ASSERT_EQ(next_line_start("hello\nworld", 4), size_t{6});
}

TEST(next_line_start_last_line) {
    // On last line: returns text.size()
    ASSERT_EQ(next_line_start("hello\nworld", 8), size_t{11});
}

// ── Selection normalization ────────────────────────────────────────────────

TEST(normalize_sorts_ascending) {
    std::vector<Sel> sels = {{5, 5}, {2, 2}, {0, 0}};
    normalize_selections(sels);
    ASSERT_EQ(sels.size(), size_t{3});
    ASSERT_EQ(sels[0], (Sel{0, 0}));
    ASSERT_EQ(sels[1], (Sel{2, 2}));
    ASSERT_EQ(sels[2], (Sel{5, 5}));
}

TEST(normalize_removes_duplicates) {
    std::vector<Sel> sels = {{3, 3}, {3, 3}};
    normalize_selections(sels);
    ASSERT_EQ(sels.size(), size_t{1});
    ASSERT_EQ(sels[0], (Sel{3, 3}));
}

TEST(normalize_merges_overlapping) {
    // [1,5) and [3,7) overlap → merged [1,7)
    std::vector<Sel> sels = {{1, 5}, {3, 7}};
    normalize_selections(sels);
    ASSERT_EQ(sels.size(), size_t{1});
    ASSERT_EQ(sels[0].lo(), size_t{1});
    ASSERT_EQ(sels[0].hi(), size_t{7});
}

TEST(normalize_keeps_adjacent_separate) {
    // [0,3) and [3,6) are adjacent but non-overlapping
    std::vector<Sel> sels = {{0, 3}, {3, 6}};
    normalize_selections(sels);
    ASSERT_EQ(sels.size(), size_t{2});
}

// ── Text mutation: text_insert ─────────────────────────────────────────────

TEST(text_insert_into_empty) {
    // "" + insert("hello") → "hello", caret at 5
    auto ed = make_editor("");
    ASSERT_TRUE(text_insert(ed, "hello"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(text_insert_at_caret_middle) {
    // "hello", caret at 2, insert("X") → "heXllo", caret at 3
    auto ed = make_editor("hello", 2, 2);
    ASSERT_TRUE(text_insert(ed, "X"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"heXllo"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
}

TEST(text_insert_replaces_selection) {
    // "hello world", select "world" [6,11), insert("there") → "hello there", caret at 11
    auto ed = make_editor("hello world", 6, 11);
    ASSERT_TRUE(text_insert(ed, "there"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello there"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{11, 11}));
}

TEST(text_insert_two_carets) {
    // "ac", carets at [1,1] and [2,2], insert("X"):
    //   high→low: insert at 2 → "acX" cursor=3; insert at 1 → "aXcX" cursor=2, adjust 3→4
    //   sorted: [{2,2},{4,4}]
    auto ed = make_editor("ac");
    ed.selections = {{1, 1}, {2, 2}};
    ASSERT_TRUE(text_insert(ed, "X"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"aXcX"});
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
    ASSERT_EQ(snapshot_selections(ed)[1], (Sel{4, 4}));
}

TEST(text_insert_read_only_rejected) {
    auto ed = make_editor("hello");
    ed.mode = Mode::read_only;
    ASSERT_FALSE(text_insert(ed, "x"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(text_insert_diff_rejected) {
    auto ed = make_editor("hello");
    ed.mode = Mode::diff;
    ASSERT_FALSE(text_insert(ed, "x"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

// ── Text mutation: text_delete_backward ───────────────────────────────────

TEST(text_delete_backward_caret) {
    // "hello", caret at 5, delete_backward → "hell", caret at 4
    auto ed = make_editor("hello", 5, 5);
    ASSERT_TRUE(text_delete_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hell"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{4, 4}));
}

TEST(text_delete_backward_at_zero) {
    // "hello", caret at 0, delete_backward → no text change
    auto ed = make_editor("hello", 0, 0);
    ASSERT_TRUE(text_delete_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(text_delete_backward_selection) {
    // "hello", selection [1,4) ("ell"), delete → "ho", caret at 1
    auto ed = make_editor("hello", 1, 4);
    ASSERT_TRUE(text_delete_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ho"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{1, 1}));
}

TEST(text_delete_backward_read_only_rejected) {
    auto ed = make_editor("hello", 3, 3);
    ed.mode = Mode::read_only;
    ASSERT_FALSE(text_delete_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

// ── Text mutation: text_delete_forward ────────────────────────────────────

TEST(text_delete_forward_caret) {
    // "hello", caret at 0, delete_forward → "ello", caret at 0
    auto ed = make_editor("hello", 0, 0);
    ASSERT_TRUE(text_delete_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(text_delete_forward_at_end) {
    // "hello", caret at 5 (end), delete_forward → no change
    auto ed = make_editor("hello", 5, 5);
    ASSERT_TRUE(text_delete_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(text_delete_forward_selection) {
    // "hello", selection [1,4), delete_forward → "ho", caret at 1
    auto ed = make_editor("hello", 1, 4);
    ASSERT_TRUE(text_delete_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ho"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{1, 1}));
}

// ── Text mutation: text_delete_word_backward / forward ────────────────────

TEST(text_delete_word_backward_basic) {
    // "hello world", caret at 11 (end), delete_word_backward
    // word_left("hello world", 11): back over 'd','l','r','o','w' → pos 6
    // delete [6,11) → "hello ", caret at 6
    auto ed = make_editor("hello world", 11, 11);
    ASSERT_TRUE(text_delete_word_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello "});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(text_delete_word_forward_basic) {
    // "hello world", caret at 0, delete_word_forward
    // word_right("hello world", 0): skip word chars "hello" → pos 5
    // delete [0,5) → " world", caret at 0
    auto ed = make_editor("hello world", 0, 0);
    ASSERT_TRUE(text_delete_word_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{" world"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

// ── Cursor movement ────────────────────────────────────────────────────────

TEST(cursor_left_basic) {
    auto ed = make_editor("hello", 3, 3);
    cursor_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(cursor_left_at_zero_noop) {
    auto ed = make_editor("hello", 0, 0);
    cursor_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(cursor_left_collapses_selection_to_lo) {
    // Selection [2,4): cursor_left collapses to lo=2
    auto ed = make_editor("hello", 2, 4);
    cursor_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(cursor_right_basic) {
    auto ed = make_editor("hello", 2, 2);
    cursor_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
}

TEST(cursor_right_at_end_noop) {
    auto ed = make_editor("hello", 5, 5);
    cursor_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(cursor_right_collapses_selection_to_hi) {
    // Selection anchor=2, active=4 (forward): cursor_right collapses to hi=4
    auto ed = make_editor("hello", 2, 4);
    cursor_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{4, 4}));
}

TEST(cursor_word_right_from_word) {
    // "hello world", caret at 0, cursor_word_right → skip "hello" → pos 5
    auto ed = make_editor("hello world", 0, 0);
    cursor_word_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(cursor_word_right_from_space) {
    // "hello world", caret at 5 (space), cursor_word_right → skip ' ' → pos 6
    auto ed = make_editor("hello world", 5, 5);
    cursor_word_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(cursor_word_left_from_end) {
    // "hello world", caret at 11, cursor_word_left → skip "world" → pos 6
    auto ed = make_editor("hello world", 11, 11);
    cursor_word_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(cursor_line_start_basic) {
    // "hello\nworld", caret at 8 (in "world") → line_start = 6
    auto ed = make_editor("hello\nworld", 8, 8);
    cursor_line_start(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(cursor_line_end_basic) {
    // "hello\nworld", caret at 8 → line_end = 11
    auto ed = make_editor("hello\nworld", 8, 8);
    cursor_line_end(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{11, 11}));
}

TEST(cursor_line_end_first_line) {
    // "hello\nworld", caret at 2 → line_end = 5 (position of '\n')
    auto ed = make_editor("hello\nworld", 2, 2);
    cursor_line_end(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(cursor_doc_start) {
    auto ed = make_editor("hello", 5, 5);
    cursor_doc_start(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(cursor_doc_end) {
    auto ed = make_editor("hello", 0, 0);
    cursor_doc_end(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(cursor_line_up_basic) {
    // "hello\nworld", caret at 8 (col=2 on "world")
    //   line_start(8)=6, col=8-6=2
    //   prev_line_start: line_start(5)=0, line_end(0)=5, prev_len=5
    //   new_pos = 0 + min(2,5) = 2
    auto ed = make_editor("hello\nworld", 8, 8);
    cursor_line_up(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(cursor_line_up_on_first_line) {
    // Already on first line: moves to position 0
    auto ed = make_editor("hello\nworld", 3, 3);
    cursor_line_up(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(cursor_line_up_col_clamp) {
    // "hi\nhello", caret at 7 (col=5 on "hello"), up to "hi" (len=2)
    //   new_pos = 0 + min(5,2) = 2
    auto ed = make_editor("hi\nhello", 7, 7);
    cursor_line_up(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(cursor_line_down_basic) {
    // "hello\nworld", caret at 2 (col=2 on "hello")
    //   next_line_start(2)=6, col=2, line_end("world")=11, len=5
    //   new_pos = 6 + min(2,5) = 8
    auto ed = make_editor("hello\nworld", 2, 2);
    cursor_line_down(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{8, 8}));
}

TEST(cursor_line_down_on_last_line) {
    // On last line: moves to line end
    auto ed = make_editor("hello\nworld", 8, 8);
    cursor_line_down(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{11, 11}));
}

TEST(cursor_set_position) {
    auto ed = make_editor("hello world", 0, 0);
    cursor_set_position(ed, 6);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

// ── Selection extension ────────────────────────────────────────────────────

TEST(select_left_from_caret) {
    // "hello", caret at 3, select_left → anchor=3, active=2
    auto ed = make_editor("hello", 3, 3);
    select_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 2}));
}

TEST(select_right_from_caret) {
    // "hello", caret at 3, select_right → anchor=3, active=4
    auto ed = make_editor("hello", 3, 3);
    select_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 4}));
}

TEST(select_right_then_left_restores_caret) {
    auto ed = make_editor("hello", 3, 3);
    select_right(ed); // {3,4}
    select_left(ed);  // {3,3}
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
}

TEST(select_all) {
    // "hello\nworld" (length 11): select_all → anchor=0, active=11
    auto ed = make_editor("hello\nworld", 3, 3);
    select_all(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 11}));
}

TEST(select_doc_start) {
    auto ed = make_editor("hello", 3, 5);
    select_doc_start(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 0}));
}

TEST(select_doc_end) {
    auto ed = make_editor("hello", 0, 3);
    select_doc_end(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 5}));
}

TEST(select_line_start) {
    // "hello\nworld", selection active at 8, select_line_start → active=6
    auto ed = make_editor("hello\nworld", 8, 8);
    select_line_start(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{8, 6}));
}

TEST(select_line_end) {
    auto ed = make_editor("hello\nworld", 6, 6);
    select_line_end(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 11}));
}

TEST(select_word_right_basic) {
    // "hello world", caret at 0, select_word_right → anchor=0, active=5
    auto ed = make_editor("hello world", 0, 0);
    select_word_right(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 5}));
}

TEST(select_word_left_basic) {
    // "hello world", caret at 11, select_word_left → anchor=11, active=6
    auto ed = make_editor("hello world", 11, 11);
    select_word_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{11, 6}));
}

TEST(select_add_next_occurrence) {
    // "foo bar foo": select first "foo" [0,3), add_next_occurrence → [0,3) + [8,11)
    // "foo bar foo": f(0)o(1)o(2) (3)b(4)a(5)r(6) (7)f(8)o(9)o(10) — length 11
    auto ed = make_editor("foo bar foo", 0, 3);
    select_add_next_occurrence(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0].lo(), size_t{0});
    ASSERT_EQ(snapshot_selections(ed)[0].hi(), size_t{3});
    ASSERT_EQ(snapshot_selections(ed)[1].lo(), size_t{8});
    ASSERT_EQ(snapshot_selections(ed)[1].hi(), size_t{11});
}

TEST(select_add_next_occurrence_no_match) {
    // Only one occurrence: nothing added
    auto ed = make_editor("hello world", 0, 5);
    select_add_next_occurrence(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
}

TEST(select_add_next_occurrence_caret_noop) {
    auto ed = make_editor("hello", 2, 2);
    select_add_next_occurrence(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
}

TEST(select_add_cursor_up) {
    // "hello\nworld", caret at 9 (col=3 on "world"), add_cursor_up
    //   prev line "hello" len=5, col=3 → new caret at 0+3=3
    auto ed = make_editor("hello\nworld", 9, 9);
    select_add_cursor_up(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
    ASSERT_EQ(snapshot_selections(ed)[1], (Sel{9, 9}));
}

TEST(select_add_cursor_down) {
    // "hello\nworld", caret at 2 (col=2 on "hello"), add_cursor_down
    //   next_line "world" len=5, col=2 → new caret at 6+2=8
    auto ed = make_editor("hello\nworld", 2, 2);
    select_add_cursor_down(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
    ASSERT_EQ(snapshot_selections(ed)[1], (Sel{8, 8}));
}

TEST(select_split_into_lines) {
    // "hello\nworld\nfoo": h(0)e(1)l(2)l(3)o(4)\n(5)w(6)o(7)r(8)l(9)d(10)\n(11)f(12)o(13)o(14)
    // selection [2,14) spans 3 lines
    //   line 0: [max(2,0), min(14,5)] = [2,5]
    //   line 1: [max(2,6), min(14,11)] = [6,11]
    //   line 2: [max(2,12), min(14,14)] = [12,14]
    auto ed = make_editor("hello\nworld\nfoo", 2, 14);
    select_split_into_lines(ed);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{3});
    ASSERT_EQ(snapshot_selections(ed)[0].lo(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0].hi(), size_t{5});
    ASSERT_EQ(snapshot_selections(ed)[1].lo(), size_t{6});
    ASSERT_EQ(snapshot_selections(ed)[1].hi(), size_t{11});
    ASSERT_EQ(snapshot_selections(ed)[2].lo(), size_t{12});
    ASSERT_EQ(snapshot_selections(ed)[2].hi(), size_t{14});
}

TEST(select_line_up_extends_active) {
    // "hello\nworld", active at 8 (col=2 on "world"), select_line_up
    //   active moves to 0+2=2; anchor stays at 8
    auto ed = make_editor("hello\nworld", 8, 8);
    select_line_up(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{8, 2}));
}

TEST(select_line_down_extends_active) {
    // "hello\nworld", active at 2 (col=2 on "hello"), select_line_down
    //   active moves to 6+2=8; anchor stays at 2
    auto ed = make_editor("hello\nworld", 2, 2);
    select_line_down(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 8}));
}

TEST(select_set_range) {
    auto ed = make_editor("hello world", 0, 0);
    select_set_range(ed, 6, 11);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 11}));
}

TEST(select_add_range) {
    auto ed = make_editor("hello world", 0, 5);
    select_add_range(ed, 6, 11);
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0].lo(), size_t{0});
    ASSERT_EQ(snapshot_selections(ed)[1].lo(), size_t{6});
}

// ── Clipboard ─────────────────────────────────────────────────────────────

TEST(clipboard_copy_selection) {
    // "hello", selection [1,4) → clipboard = ["ell"]
    auto ed = make_editor("hello", 1, 4);
    clipboard_copy(ed);
    ASSERT_EQ(ed.clipboard.size(), size_t{1});
    ASSERT_EQ(ed.clipboard[0], std::string{"ell"});
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"}); // text unchanged
}

TEST(clipboard_copy_caret_captures_line) {
    // "hello\nworld", caret at 2 → captures "hello\n" (line including '\n')
    auto ed = make_editor("hello\nworld", 2, 2);
    clipboard_copy(ed);
    ASSERT_EQ(ed.clipboard.size(), size_t{1});
    ASSERT_EQ(ed.clipboard[0], std::string{"hello\n"});
}

TEST(clipboard_copy_caret_last_line) {
    // "hello\nworld", caret at 8 (in "world") → captures "world" (no '\n')
    auto ed = make_editor("hello\nworld", 8, 8);
    clipboard_copy(ed);
    ASSERT_EQ(ed.clipboard.size(), size_t{1});
    ASSERT_EQ(ed.clipboard[0], std::string{"world"});
}

TEST(clipboard_paste_at_caret) {
    // text="world", caret at 0, clipboard=["hello "]
    // insert "hello " at 0 → "hello world", caret at 6
    auto ed = make_editor("world", 0, 0);
    ed.clipboard = {"hello "};
    ASSERT_TRUE(clipboard_paste(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello world"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(clipboard_paste_replaces_selection) {
    // "hello world", select [6,11), clipboard=["there"]
    // replace [6,11) with "there" → "hello there", caret at 11
    auto ed = make_editor("hello world", 6, 11);
    ed.clipboard = {"there"};
    ASSERT_TRUE(clipboard_paste(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello there"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{11, 11}));
}

TEST(clipboard_paste_one_to_one) {
    // "ac", carets [1,1] and [2,2], clipboard=["X","Y"] (1:1 paste)
    // Edit at 2: "acY" cursor=3; Edit at 1: "aXcY" cursor=2, adjust 3→4
    // Result: [{2,2},{4,4}]
    auto ed = make_editor("ac");
    ed.selections = {{1, 1}, {2, 2}};
    ed.clipboard  = {"X", "Y"};
    ASSERT_TRUE(clipboard_paste(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"aXcY"});
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
    ASSERT_EQ(snapshot_selections(ed)[1], (Sel{4, 4}));
}

TEST(clipboard_paste_full_text_on_mismatch) {
    // "ac", carets [1,1] and [2,2], clipboard=["XY"] (1 frag, 2 cursors)
    // Each caret gets "XY":
    // Edit at 2: "acXY" cursor=4; Edit at 1: "aXYcXY" cursor=3, adjust 4→6
    // Result: [{3,3},{6,6}]
    auto ed = make_editor("ac");
    ed.selections = {{1, 1}, {2, 2}};
    ed.clipboard  = {"XY"};
    ASSERT_TRUE(clipboard_paste(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"aXYcXY"});
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
    ASSERT_EQ(snapshot_selections(ed)[1], (Sel{6, 6}));
}

TEST(clipboard_cut_selection) {
    // "hello", selection [1,4) → clipboard=["ell"], text="ho", caret at 1
    auto ed = make_editor("hello", 1, 4);
    ASSERT_TRUE(clipboard_cut(ed));
    ASSERT_EQ(ed.clipboard.size(), size_t{1});
    ASSERT_EQ(ed.clipboard[0], std::string{"ell"});
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ho"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{1, 1}));
}

TEST(clipboard_cut_read_only_rejected) {
    auto ed = make_editor("hello", 1, 4);
    ed.mode = Mode::read_only;
    ASSERT_FALSE(clipboard_cut(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(clipboard_paste_read_only_rejected) {
    auto ed = make_editor("hello", 0, 0);
    ed.clipboard = {"x"};
    ed.mode = Mode::read_only;
    ASSERT_FALSE(clipboard_paste(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

// ── Undo / redo ───────────────────────────────────────────────────────────

TEST(undo_after_insert) {
    auto ed = make_editor("");
    text_insert(ed, "hello");
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_TRUE(edit_undo(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{""});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(redo_after_undo) {
    auto ed = make_editor("");
    text_insert(ed, "hello");
    edit_undo(ed);
    ASSERT_TRUE(edit_redo(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(undo_empty_stack_returns_false) {
    auto ed = make_editor("hello");
    ASSERT_FALSE(edit_undo(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(redo_empty_stack_returns_false) {
    auto ed = make_editor("hello");
    ASSERT_FALSE(edit_redo(ed));
}

TEST(new_edit_clears_redo) {
    // insert "a", insert "b", undo → redo has "ab"; insert "c" → redo cleared
    auto ed = make_editor("");
    text_insert(ed, "a");
    text_insert(ed, "b");
    edit_undo(ed);
    ASSERT_FALSE(ed.redo_stack.empty()); // redo has "ab" state
    text_insert(ed, "c");               // new edit clears redo
    ASSERT_TRUE(ed.redo_stack.empty());
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ac"});
}

TEST(undo_redo_restores_selections) {
    // Undo/redo must restore the selection state, not just the text.
    auto ed = make_editor("hello", 2, 4); // selection covers "ll"
    text_insert(ed, "X");                 // replaces "ll" → "heXo", caret at 3
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
    edit_undo(ed);
    // Selections restored to pre-insert state: [2,4)
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0].lo(), size_t{2});
    ASSERT_EQ(snapshot_selections(ed)[0].hi(), size_t{4});
}

TEST(undo_stack_grows_each_mutation) {
    auto ed = make_editor("");
    ASSERT_EQ(ed.undo_stack.size(), size_t{0});
    text_insert(ed, "a");
    ASSERT_EQ(ed.undo_stack.size(), size_t{1});
    text_insert(ed, "b");
    ASSERT_EQ(ed.undo_stack.size(), size_t{2});
    edit_undo(ed);
    ASSERT_EQ(ed.undo_stack.size(), size_t{1});
    ASSERT_EQ(ed.redo_stack.size(), size_t{1});
}

// ── Edit transforms ───────────────────────────────────────────────────────

TEST(edit_uppercase_selection) {
    // "hello", select all [0,5), uppercase → "HELLO", caret at 5
    auto ed = make_editor("hello", 0, 5);
    ASSERT_TRUE(edit_uppercase(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"HELLO"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(edit_lowercase_selection) {
    auto ed = make_editor("WORLD", 0, 5);
    ASSERT_TRUE(edit_lowercase(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"world"});
}

TEST(edit_swap_case_selection) {
    // "Hello" → 'H'→'h', 'e'→'E', 'l'→'L', 'l'→'L', 'o'→'O' → "hELLO"
    auto ed = make_editor("Hello", 0, 5);
    ASSERT_TRUE(edit_swap_case(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hELLO"});
}

TEST(edit_uppercase_read_only_rejected) {
    auto ed = make_editor("hello", 0, 5);
    ed.mode = Mode::read_only;
    ASSERT_FALSE(edit_uppercase(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(edit_indent_single_line) {
    // "hello", caret at 0 → indent adds 4 spaces → "    hello", caret at 4
    auto ed = make_editor("hello", 0, 0);
    ASSERT_TRUE(edit_indent(ed, 4, true));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"    hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{4, 4}));
}

TEST(edit_indent_two_lines) {
    // "a\nb", select all [0,3):
    //   line 0 start=0: insert "    " → delta=+4
    //   line 1 start=2 (adjusted to 6 after line 0 insertion): insert "    "
    // text = "    a\n    b" (11 chars)
    // Initial sel [0,3):
    //   after indent at line 1 (pos=2): anchor 0 < 2 unchanged, active 3 >= 2 → 3+4=7
    //   after indent at line 0 (pos=0): anchor 0 >= 0 → 0+4=4, active 7 >= 0 → 7+4=11
    // sel = [{4,11}]
    auto ed = make_editor("a\nb", 0, 3);
    ASSERT_TRUE(edit_indent(ed, 4, true));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"    a\n    b"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{4, 11}));
}

TEST(edit_outdent_single_line) {
    // "    hello", caret at 4 → outdent 4 spaces → "hello", caret at 0
    auto ed = make_editor("    hello", 4, 4);
    ASSERT_TRUE(edit_outdent(ed, 4, true));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(edit_outdent_partial) {
    // "  hello" (2 spaces), caret at 2, outdent 4 → removes 2 (all leading spaces)
    auto ed = make_editor("  hello", 2, 2);
    ASSERT_TRUE(edit_outdent(ed, 4, true));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(edit_outdent_no_leading_space) {
    // "hello" no leading spaces → no change
    auto ed = make_editor("hello", 0, 0);
    ASSERT_TRUE(edit_outdent(ed, 4, true));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(edit_duplicate_line_non_last) {
    // "hello\nworld", caret at 2 (on "hello"):
    //   line_end(2)=5, insert "\nhello" at 5 → "hello\nhello\nworld"
    //   cursor at 5+1=6
    auto ed = make_editor("hello\nworld", 2, 2);
    ASSERT_TRUE(edit_duplicate_line(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello\nhello\nworld"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(edit_duplicate_line_last_line) {
    // "hello\nworld", caret at 8 (on "world"):
    //   line_end(8)=11, insert "\nworld" at 11 → "hello\nworld\nworld"
    //   cursor at 11+1=12
    auto ed = make_editor("hello\nworld", 8, 8);
    ASSERT_TRUE(edit_duplicate_line(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello\nworld\nworld"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{12, 12}));
}

TEST(edit_delete_line_middle) {
    // "line1\nline2\nline3", caret at 7 (on "line2"):
    //   ls=6, le=line_end(text,7)=11 (pos of '\n'), le < 17 → has newline
    //   delete [6,12) = "line2\n" → "line1\nline3", cursor at 6
    auto ed = make_editor("line1\nline2\nline3", 7, 7);
    ASSERT_TRUE(edit_delete_line(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"line1\nline3"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(edit_delete_line_last_line) {
    // "aaa\nbbb\nccc", caret at 9 (on "ccc"):
    //   ls=8, le=line_end(text,9)=11=text.size() → last line (no '\n')
    //   del_start = 8-1=7, delete [7,11) = "\nccc" → "aaa\nbbb", cursor at 7
    auto ed = make_editor("aaa\nbbb\nccc", 9, 9);
    ASSERT_TRUE(edit_delete_line(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"aaa\nbbb"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{7, 7}));
}

TEST(edit_delete_line_only_line) {
    // "hello" (single line, no '\n'), caret at 2:
    //   ls=0, le=5=text.size() → last line, ls==0 → del_start=0
    //   delete [0,5) → "", cursor at 0
    auto ed = make_editor("hello", 2, 2);
    ASSERT_TRUE(edit_delete_line(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{""});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(edit_join_lines_basic) {
    // "hello\nworld", caret at 4 (on "hello"):
    //   line_end(4)=5, replace [5,6) with ' ' → "hello world", caret at 5
    //   (cursor lands at 5 = the replaced '\n' position)
    // Note: apply_ops places cursor at lo+replacement.size() = 5+1=6? No:
    //   op: lo=5, hi=6, replacement=" " (len=1), cursor=5+1=6
    // Wait let me recompute: lo=5, hi=6, repl=" " → cursor at lo+1=6
    auto ed = make_editor("hello\nworld", 4, 4);
    ASSERT_TRUE(edit_join_lines(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello world"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{6, 6}));
}

TEST(edit_join_lines_last_line_noop) {
    // On last line (no '\n'): no change
    auto ed = make_editor("hello\nworld", 8, 8);
    ASSERT_TRUE(edit_join_lines(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello\nworld"});
}

TEST(edit_transpose_basic) {
    // "hello", caret at 2 (between 'e' and 'l'):
    //   swap text[1]='e' with text[2]='l' → "hlelo", cursor at 3
    //   Wait: char_a_start=utf8_prev("hello",2)=1, char_b_end=utf8_next("hello",2)=3
    //   ca = text[1..2) = "e", cb = text[2..3) = "l"
    //   replacement = cb+ca = "le", cursor_at = 2 (le.size()+ca.size())
    //   → text becomes "h" + "le" + "lo" = "hlelo", cursor at 1+2=3
    auto ed = make_editor("hello", 2, 2);
    ASSERT_TRUE(edit_transpose(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hlelo"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{3, 3}));
}

TEST(edit_sort_lines_basic) {
    // "banana\napple\ncherry", select all:
    //   length = 6+1+5+1+6 = 19
    //   sorted: "apple\nbanana\ncherry" (length = 5+1+6+1+6 = 19)
    auto ed = make_editor("banana\napple\ncherry", 0, 19);
    ASSERT_TRUE(edit_sort_lines(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"apple\nbanana\ncherry"});
}

TEST(edit_sort_lines_disjoint_selections) {
    // "ccc\nbbb\naaa", carets on first and last line only (not middle):
    //   disjoint selections → each group has only 1 line → no sort, no change
    auto ed = make_editor("ccc\nbbb\naaa");
    ed.selections = {{0, 0}, {8, 8}};
    ASSERT_TRUE(edit_sort_lines(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ccc\nbbb\naaa"});
}

TEST(edit_toggle_comment_add) {
    // "hello", caret at 0, token="//" → "//hello", caret at 2
    auto ed = make_editor("hello", 0, 0);
    ASSERT_TRUE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"//hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(edit_toggle_comment_remove) {
    // "//hello", caret at 0, token="//" → "hello", caret at 0
    auto ed = make_editor("//hello", 0, 0);
    ASSERT_TRUE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(edit_toggle_comment_two_lines_add) {
    // "foo\nbar", select all: both lines uncommented → add "//" to both
    auto ed = make_editor("foo\nbar", 0, 7);
    ASSERT_TRUE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"//foo\n//bar"});
}

TEST(edit_toggle_comment_two_lines_remove) {
    // "//foo\n//bar", select all: both commented → remove "//" from both
    auto ed = make_editor("//foo\n//bar", 0, 11);
    ASSERT_TRUE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"foo\nbar"});
}

TEST(edit_toggle_comment_mixed_adds_to_all) {
    // "//foo\nbar": first line commented, second not → add to all (not all commented)
    auto ed = make_editor("//foo\nbar", 0, 9);
    ASSERT_TRUE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"////foo\n//bar"});
}

TEST(edit_toggle_comment_read_only_rejected) {
    auto ed = make_editor("hello", 0, 0);
    ed.mode = Mode::read_only;
    ASSERT_FALSE(edit_toggle_comment(ed, "//"));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(edit_move_line_up_basic) {
    // "first\nsecond\nthird", caret at 7 (on "second"):
    //   swap "second" with "first" → "second\nfirst\nthird"
    //   cursor moves from line 1 to line 0 at same column
    auto ed = make_editor("first\nsecond\nthird", 7, 7);
    ASSERT_TRUE(edit_move_line_up(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"second\nfirst\nthird"});
}

TEST(edit_move_line_up_first_line_noop) {
    // On first line: no change
    auto ed = make_editor("first\nsecond", 2, 2);
    ASSERT_TRUE(edit_move_line_up(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"first\nsecond"});
}

TEST(edit_move_line_down_basic) {
    // "first\nsecond\nthird", caret at 2 (on "first"):
    //   swap "first" with "second" → "second\nfirst\nthird"
    auto ed = make_editor("first\nsecond\nthird", 2, 2);
    ASSERT_TRUE(edit_move_line_down(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"second\nfirst\nthird"});
}

TEST(edit_move_line_down_last_line_noop) {
    auto ed = make_editor("first\nsecond", 8, 8);
    ASSERT_TRUE(edit_move_line_down(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"first\nsecond"});
}

// ── Snapshot accessors ────────────────────────────────────────────────────

TEST(snapshot_text_returns_string_view) {
    auto ed = make_editor("hello");
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(snapshot_selections_returns_ref) {
    auto ed = make_editor("hello", 2, 4);
    auto const& sels = snapshot_selections(ed);
    ASSERT_EQ(sels.size(), size_t{1});
    ASSERT_EQ(sels[0], (Sel{2, 4}));
}

// ── UTF-8 multibyte cursor movement ───────────────────────────────────────

TEST(cursor_left_multibyte) {
    // "\xC3\xA9!" (é then !) — caret at 3 (after '!'), cursor_left → at 2 (before '!')
    std::string s = "\xC3\xA9!";
    auto ed = make_editor(s, 3, 3);
    cursor_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{2, 2}));
}

TEST(cursor_left_over_multibyte) {
    // "\xC3\xA9!" caret at 2 (after é), cursor_left → at 0 (start of é)
    std::string s = "\xC3\xA9!";
    auto ed = make_editor(s, 2, 2);
    cursor_left(ed);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(delete_backward_multibyte) {
    // "\xC3\xA9!" caret at 2, delete_backward → "!" caret at 0
    std::string s = "\xC3\xA9!";
    auto ed = make_editor(s, 2, 2);
    ASSERT_TRUE(text_delete_backward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"!"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

// ── make_editor helpers ───────────────────────────────────────────────────

TEST(make_editor_default_caret_at_end) {
    auto ed = make_editor("hello");
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{5, 5}));
}

TEST(make_editor_empty_text) {
    auto ed = make_editor();
    ASSERT_EQ(snapshot_text(ed), std::string_view{""});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

TEST(make_editor_with_range) {
    auto ed = make_editor("hello", 1, 3);
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{1, 3}));
}

// ── Mode: diff mode behaves like read_only ─────────────────────────────────

TEST(diff_mode_rejects_delete) {
    auto ed = make_editor("hello", 3, 3);
    ed.mode = Mode::diff;
    ASSERT_FALSE(text_delete_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello"});
}

TEST(diff_mode_rejects_paste) {
    auto ed = make_editor("hello", 0, 0);
    ed.mode = Mode::diff;
    ed.clipboard = {"x"};
    ASSERT_FALSE(clipboard_paste(ed));
}

// ── Integration: multi-step scripts ───────────────────────────────────────

TEST(script_type_and_undo) {
    // Empty document → type "ab" char by char → undo twice → empty again
    auto ed = make_editor("");
    text_insert(ed, "a");
    text_insert(ed, "b");
    ASSERT_EQ(snapshot_text(ed), std::string_view{"ab"});
    edit_undo(ed);
    ASSERT_EQ(snapshot_text(ed), std::string_view{"a"});
    edit_undo(ed);
    ASSERT_EQ(snapshot_text(ed), std::string_view{""});
    ASSERT_FALSE(edit_undo(ed)); // stack exhausted
}

TEST(script_copy_paste_round_trip) {
    // Select "hello", copy, move to end, paste → "hellohello"
    auto ed = make_editor("hello");
    select_all(ed);
    clipboard_copy(ed);
    cursor_doc_end(ed);
    clipboard_paste(ed);
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hellohello"});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{10, 10}));
}

TEST(script_indent_outdent_round_trip) {
    // "hello\nworld", select all, indent, outdent → back to original
    auto ed = make_editor("hello\nworld");
    select_all(ed);
    edit_indent(ed, 4, true);
    ASSERT_EQ(snapshot_text(ed), std::string_view{"    hello\n    world"});
    select_all(ed);
    edit_outdent(ed, 4, true);
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello\nworld"});
}

TEST(script_comment_toggle_round_trip) {
    auto ed = make_editor("hello\nworld");
    select_all(ed);
    edit_toggle_comment(ed, "//");
    ASSERT_EQ(snapshot_text(ed), std::string_view{"//hello\n//world"});
    select_all(ed);
    edit_toggle_comment(ed, "//");
    ASSERT_EQ(snapshot_text(ed), std::string_view{"hello\nworld"});
}

TEST(script_multicursor_type_delete) {
    // "aa", carets at [0,0] and [1,1] (before each 'a')
    // delete_forward: delete 'a' at 1 first (high→low)
    //   op at 1: "a_" → "a", cursor=1; adjust []
    //   op at 0: "a" → "", cursor=0; adjust [1]→[1] (delta=-1 so 1→0? no: delta=-1, adjust)
    //   Wait: text="aa", carets at [0,0] and [1,1]
    //   delete_forward at 1: delete [1,2) → "a", cursor=1, delta=-1
    //   delete_forward at 0: delete [0,1) → "", cursor=0, delta=-1; adjust [1]→[1-1=0]
    //   New cursors (sorted): [0, 0] → normalize → [{0,0}]
    auto ed = make_editor("aa");
    ed.selections = {{0, 0}, {1, 1}};
    ASSERT_TRUE(text_delete_forward(ed));
    ASSERT_EQ(snapshot_text(ed), std::string_view{""});
    ASSERT_EQ(snapshot_selections(ed).size(), size_t{1});
    ASSERT_EQ(snapshot_selections(ed)[0], (Sel{0, 0}));
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Reference editor oracle tests ===\n";

    // UTF-8 helpers
    RUN(utf8_next_ascii);
    RUN(utf8_next_multibyte);
    RUN(utf8_prev_ascii);
    RUN(utf8_prev_multibyte);

    // Line navigation
    RUN(line_start_first_line);
    RUN(line_start_second_line);
    RUN(line_start_at_newline);
    RUN(line_end_first_line);
    RUN(line_end_last_line);
    RUN(next_line_start_basic);
    RUN(next_line_start_last_line);

    // Normalization
    RUN(normalize_sorts_ascending);
    RUN(normalize_removes_duplicates);
    RUN(normalize_merges_overlapping);
    RUN(normalize_keeps_adjacent_separate);

    // text_insert
    RUN(text_insert_into_empty);
    RUN(text_insert_at_caret_middle);
    RUN(text_insert_replaces_selection);
    RUN(text_insert_two_carets);
    RUN(text_insert_read_only_rejected);
    RUN(text_insert_diff_rejected);

    // text_delete_backward
    RUN(text_delete_backward_caret);
    RUN(text_delete_backward_at_zero);
    RUN(text_delete_backward_selection);
    RUN(text_delete_backward_read_only_rejected);

    // text_delete_forward
    RUN(text_delete_forward_caret);
    RUN(text_delete_forward_at_end);
    RUN(text_delete_forward_selection);

    // delete word
    RUN(text_delete_word_backward_basic);
    RUN(text_delete_word_forward_basic);

    // cursor movement
    RUN(cursor_left_basic);
    RUN(cursor_left_at_zero_noop);
    RUN(cursor_left_collapses_selection_to_lo);
    RUN(cursor_right_basic);
    RUN(cursor_right_at_end_noop);
    RUN(cursor_right_collapses_selection_to_hi);
    RUN(cursor_word_right_from_word);
    RUN(cursor_word_right_from_space);
    RUN(cursor_word_left_from_end);
    RUN(cursor_line_start_basic);
    RUN(cursor_line_end_basic);
    RUN(cursor_line_end_first_line);
    RUN(cursor_doc_start);
    RUN(cursor_doc_end);
    RUN(cursor_line_up_basic);
    RUN(cursor_line_up_on_first_line);
    RUN(cursor_line_up_col_clamp);
    RUN(cursor_line_down_basic);
    RUN(cursor_line_down_on_last_line);
    RUN(cursor_set_position);

    // selection extension
    RUN(select_left_from_caret);
    RUN(select_right_from_caret);
    RUN(select_right_then_left_restores_caret);
    RUN(select_all);
    RUN(select_doc_start);
    RUN(select_doc_end);
    RUN(select_line_start);
    RUN(select_line_end);
    RUN(select_word_right_basic);
    RUN(select_word_left_basic);
    RUN(select_add_next_occurrence);
    RUN(select_add_next_occurrence_no_match);
    RUN(select_add_next_occurrence_caret_noop);
    RUN(select_add_cursor_up);
    RUN(select_add_cursor_down);
    RUN(select_split_into_lines);
    RUN(select_line_up_extends_active);
    RUN(select_line_down_extends_active);
    RUN(select_set_range);
    RUN(select_add_range);

    // clipboard
    RUN(clipboard_copy_selection);
    RUN(clipboard_copy_caret_captures_line);
    RUN(clipboard_copy_caret_last_line);
    RUN(clipboard_paste_at_caret);
    RUN(clipboard_paste_replaces_selection);
    RUN(clipboard_paste_one_to_one);
    RUN(clipboard_paste_full_text_on_mismatch);
    RUN(clipboard_cut_selection);
    RUN(clipboard_cut_read_only_rejected);
    RUN(clipboard_paste_read_only_rejected);

    // undo/redo
    RUN(undo_after_insert);
    RUN(redo_after_undo);
    RUN(undo_empty_stack_returns_false);
    RUN(redo_empty_stack_returns_false);
    RUN(new_edit_clears_redo);
    RUN(undo_redo_restores_selections);
    RUN(undo_stack_grows_each_mutation);

    // edit transforms
    RUN(edit_uppercase_selection);
    RUN(edit_lowercase_selection);
    RUN(edit_swap_case_selection);
    RUN(edit_uppercase_read_only_rejected);
    RUN(edit_indent_single_line);
    RUN(edit_indent_two_lines);
    RUN(edit_outdent_single_line);
    RUN(edit_outdent_partial);
    RUN(edit_outdent_no_leading_space);
    RUN(edit_duplicate_line_non_last);
    RUN(edit_duplicate_line_last_line);
    RUN(edit_delete_line_middle);
    RUN(edit_delete_line_last_line);
    RUN(edit_delete_line_only_line);
    RUN(edit_join_lines_basic);
    RUN(edit_join_lines_last_line_noop);
    RUN(edit_transpose_basic);
    RUN(edit_sort_lines_basic);
    RUN(edit_sort_lines_disjoint_selections);
    RUN(edit_toggle_comment_add);
    RUN(edit_toggle_comment_remove);
    RUN(edit_toggle_comment_two_lines_add);
    RUN(edit_toggle_comment_two_lines_remove);
    RUN(edit_toggle_comment_mixed_adds_to_all);
    RUN(edit_toggle_comment_read_only_rejected);
    RUN(edit_move_line_up_basic);
    RUN(edit_move_line_up_first_line_noop);
    RUN(edit_move_line_down_basic);
    RUN(edit_move_line_down_last_line_noop);

    // snapshot accessors
    RUN(snapshot_text_returns_string_view);
    RUN(snapshot_selections_returns_ref);

    // multibyte
    RUN(cursor_left_multibyte);
    RUN(cursor_left_over_multibyte);
    RUN(delete_backward_multibyte);

    // make_editor
    RUN(make_editor_default_caret_at_end);
    RUN(make_editor_empty_text);
    RUN(make_editor_with_range);

    // mode enforcement
    RUN(diff_mode_rejects_delete);
    RUN(diff_mode_rejects_paste);

    // integration scripts
    RUN(script_type_and_undo);
    RUN(script_copy_paste_round_trip);
    RUN(script_indent_outdent_round_trip);
    RUN(script_comment_toggle_round_trip);
    RUN(script_multicursor_type_delete);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
