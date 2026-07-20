// Reference editor — independent string-based oracle for SSG.
//
// Implements the ref:: API declared in reference_editor.h.
// No SSG headers are included (I13 compliance).

#include "reference_editor.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace ref {

// ── UTF-8 navigation ───────────────────────────────────────────────────────

size_t utf8_next(std::string_view text, size_t pos) {
    assert(pos < text.size());
    ++pos;
    while (pos < text.size() && (static_cast<uint8_t>(text[pos]) & 0xC0u) == 0x80u)
        ++pos;
    return pos;
}

size_t utf8_prev(std::string_view text, size_t pos) {
    assert(pos > 0);
    --pos;
    while (pos > 0 && (static_cast<uint8_t>(text[pos]) & 0xC0u) == 0x80u)
        --pos;
    return pos;
}

// ── Line navigation ────────────────────────────────────────────────────────

size_t line_start(std::string_view text, size_t pos) {
    // pos may equal text.size() (caret past end is valid)
    if (pos > text.size()) pos = text.size();
    while (pos > 0) {
        if (text[pos - 1] == '\n') return pos;
        --pos;
    }
    return 0;
}

size_t line_end(std::string_view text, size_t pos) {
    while (pos < text.size() && text[pos] != '\n')
        ++pos;
    return pos;
}

size_t next_line_start(std::string_view text, size_t pos) {
    size_t e = line_end(text, pos);
    return (e < text.size()) ? e + 1 : e;
}

// ── Word boundary helpers ──────────────────────────────────────────────────

static bool isWordChar(char c) {
    auto b = static_cast<uint8_t>(c);
    return b > 127 ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Advance pos one "word group" to the right.
static size_t wordRightPos(std::string_view text, size_t pos) {
    if (pos >= text.size()) return pos;
    if (isWordChar(text[pos])) {
        while (pos < text.size() && isWordChar(text[pos]))
            pos = utf8_next(text, pos);
    } else if (isSpaceChar(text[pos])) {
        while (pos < text.size() && isSpaceChar(text[pos]))
            pos = utf8_next(text, pos);
    } else {
        pos = utf8_next(text, pos);
        while (pos < text.size() && !isWordChar(text[pos]) && !isSpaceChar(text[pos]))
            pos = utf8_next(text, pos);
    }
    return pos;
}

// Retreat pos one "word group" to the left.
static size_t wordLeftPos(std::string_view text, size_t pos) {
    if (pos == 0) return 0;
    size_t p = utf8_prev(text, pos);
    if (isWordChar(text[p])) {
        while (p > 0) {
            size_t prev = utf8_prev(text, p);
            if (!isWordChar(text[prev])) break;
            p = prev;
        }
    } else if (isSpaceChar(text[p])) {
        while (p > 0) {
            size_t prev = utf8_prev(text, p);
            if (!isSpaceChar(text[prev])) break;
            p = prev;
        }
    }
    // else: one non-word, non-space char was already stepped over
    return p;
}

// ── Selection normalization ────────────────────────────────────────────────

void normalize_selections(std::vector<Sel>& sels) {
    if (sels.empty()) return;

    std::sort(sels.begin(), sels.end(), [](Sel const& a, Sel const& b) {
        return a.lo() != b.lo() ? a.lo() < b.lo() : a.hi() < b.hi();
    });

    std::vector<Sel> out;
    out.push_back(sels[0]);

    for (size_t i = 1; i < sels.size(); ++i) {
        Sel& last = out.back();
        Sel const& cur = sels[i];

        if (cur == last) {
            // Exact duplicate: discard
        } else if (cur.lo() < last.hi()) {
            // Overlapping: extend to cover both (forward direction)
            size_t new_hi = std::max(last.hi(), cur.hi());
            last = Sel{last.lo(), new_hi};
        } else {
            out.push_back(cur);
        }
    }

    sels = std::move(out);
}

// ── Undo / redo helpers ────────────────────────────────────────────────────

// Save current state to undo stack and clear redo stack.
// Must be called before any mutation that should be undoable.
static void pushUndo(Editor& ed) {
    ed.undo_stack.push_back({ed.text, ed.selections});
    ed.redo_stack.clear();
}

// ── Core mutation engine ───────────────────────────────────────────────────

// An edit operation in pre-transaction byte coordinates.
struct EditOp {
    size_t      lo;
    size_t      hi;
    std::string replacement;
    // Where the cursor lands after this op.  SIZE_MAX means lo+replacement.size().
    size_t      cursor_at = ~size_t{0};
};

// Apply ops to ed.text (does NOT push undo — caller must do that first).
// Ops are sorted descending by lo internally so lower positions remain valid.
// ed.selections is rebuilt as carets at the computed cursor positions, then
// normalized.
static void doApplyOps(Editor& ed, std::vector<EditOp> ops) {
    if (ops.empty()) return;

    std::sort(ops.begin(), ops.end(), [](EditOp const& a, EditOp const& b) {
        return a.lo > b.lo;
    });

    // Track new cursor positions for each op (in processing order, high→low).
    std::vector<size_t> cursors;
    cursors.reserve(ops.size());

    for (auto const& op : ops) {
        ed.text.replace(op.lo, op.hi - op.lo, op.replacement);

        size_t raw = (op.cursor_at == ~size_t{0})
                         ? op.lo + op.replacement.size()
                         : op.lo + op.cursor_at;

        ptrdiff_t delta = static_cast<ptrdiff_t>(op.replacement.size()) -
                          static_cast<ptrdiff_t>(op.hi - op.lo);

        // All previously recorded cursors are at positions above op.hi
        // (guaranteed by non-overlapping ops sorted high→low); shift them.
        for (auto& c : cursors)
            c = static_cast<size_t>(static_cast<ptrdiff_t>(c) + delta);

        cursors.push_back(raw);
    }

    ed.selections.clear();
    for (size_t c : cursors)
        ed.selections.push_back(Sel{c, c});
    normalize_selections(ed.selections);
}

// push_undo then do_apply_ops.
static void applyOps(Editor& ed, std::vector<EditOp> ops) {
    if (ops.empty()) return;
    pushUndo(ed);
    doApplyOps(ed, std::move(ops));
}

// ── Adjust-in-place helper for indent/outdent and similar structural ops ───

// Apply a set of insertions/deletions and adjust all selection endpoints
// according to where the text changed.  Does NOT push undo.
// Ops are sorted descending internally.
struct AdjOp {
    size_t      pos;       // byte position
    size_t      del_len;   // bytes to delete at pos
    std::string ins_text;  // text to insert at pos
};

static void adjustApply(Editor& ed, std::vector<AdjOp> ops) {
    if (ops.empty()) return;

    std::sort(ops.begin(), ops.end(), [](AdjOp const& a, AdjOp const& b) {
        return a.pos > b.pos;
    });

    for (auto const& op : ops) {
        ed.text.replace(op.pos, op.del_len, op.ins_text);
        ptrdiff_t delta = static_cast<ptrdiff_t>(op.ins_text.size()) -
                          static_cast<ptrdiff_t>(op.del_len);
        if (delta == 0) continue;

        for (auto& s : ed.selections) {
            auto adjust = [&](size_t& ep) {
                size_t end_of_del = op.pos + op.del_len;
                if (ep >= end_of_del) {
                    // After the deleted region: shift
                    ep = static_cast<size_t>(static_cast<ptrdiff_t>(ep) + delta);
                } else if (ep > op.pos) {
                    // Inside deleted region: clamp to insertion end
                    ep = op.pos + op.ins_text.size();
                }
                // ep <= op.pos: unchanged
            };
            adjust(s.anchor);
            adjust(s.active);
        }
    }
}

// ── Line collection helper ─────────────────────────────────────────────────

// Return the sorted, deduplicated set of line-start byte positions for every
// line that intersects at least one selection.
static std::vector<size_t> touchedLineStarts(Editor const& ed) {
    std::vector<size_t> ls;
    for (auto const& s : ed.selections) {
        size_t lo = s.lo();
        size_t hi = s.hi();
        // For a caret (lo==hi) the effective end is lo+1 so the line is included.
        size_t effective_hi = (lo == hi) ? lo + 1 : hi;
        size_t cur = line_start(ed.text, lo);
        while (true) {
            if (cur >= effective_hi) break; // line starts at or after the selection end
            ls.push_back(cur);
            size_t ns = next_line_start(ed.text, cur);
            if (ns == cur) break; // last line: no further lines
            cur = ns;
        }
    }
    std::sort(ls.begin(), ls.end());
    ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
    return ls;
}

// ── Constructor helpers ────────────────────────────────────────────────────

Editor make_editor(std::string text) {
    size_t end = text.size();
    Editor ed;
    ed.text = std::move(text);
    ed.selections.push_back(Sel{end, end});
    return ed;
}

Editor make_editor(std::string text, size_t anchor, size_t active) {
    Editor ed;
    ed.text = std::move(text);
    ed.selections.push_back(Sel{anchor, active});
    return ed;
}

// ── Text mutation commands ─────────────────────────────────────────────────

bool text_insert(Editor& ed, std::string_view s) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& sel : ed.selections)
        ops.push_back({sel.lo(), sel.hi(), std::string(s)});
    applyOps(ed, std::move(ops));
    return true;
}

bool text_newline(Editor& ed) {
    return text_insert(ed, "\n");
}

bool text_delete_backward(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& sel : ed.selections) {
        if (!sel.is_caret()) {
            ops.push_back({sel.lo(), sel.hi(), ""});
        } else if (sel.active > 0) {
            size_t prev = utf8_prev(ed.text, sel.active);
            ops.push_back({prev, sel.active, ""});
        } else {
            // caret at position 0: no-op (insert nothing at 0)
            ops.push_back({0, 0, ""});
        }
    }
    applyOps(ed, std::move(ops));
    return true;
}

bool text_delete_forward(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& sel : ed.selections) {
        if (!sel.is_caret()) {
            ops.push_back({sel.lo(), sel.hi(), ""});
        } else if (sel.active < ed.text.size()) {
            size_t next = utf8_next(ed.text, sel.active);
            ops.push_back({sel.active, next, ""});
        } else {
            ops.push_back({ed.text.size(), ed.text.size(), ""});
        }
    }
    applyOps(ed, std::move(ops));
    return true;
}

bool text_delete_word_backward(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& sel : ed.selections) {
        if (!sel.is_caret()) {
            ops.push_back({sel.lo(), sel.hi(), ""});
        } else {
            size_t prev = wordLeftPos(ed.text, sel.active);
            ops.push_back({prev, sel.active, ""});
        }
    }
    applyOps(ed, std::move(ops));
    return true;
}

bool text_delete_word_forward(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& sel : ed.selections) {
        if (!sel.is_caret()) {
            ops.push_back({sel.lo(), sel.hi(), ""});
        } else {
            size_t next = wordRightPos(ed.text, sel.active);
            ops.push_back({sel.active, next, ""});
        }
    }
    applyOps(ed, std::move(ops));
    return true;
}

// ── Cursor movement ────────────────────────────────────────────────────────

void cursor_set_position(Editor& ed, size_t byte_offset) {
    size_t clamped = std::min(byte_offset, ed.text.size());
    ed.selections = {Sel{clamped, clamped}};
}

void cursor_left(Editor& ed) {
    for (auto& s : ed.selections) {
        if (!s.is_caret()) {
            s = Sel{s.lo(), s.lo()};
        } else if (s.active > 0) {
            size_t p = utf8_prev(ed.text, s.active);
            s = Sel{p, p};
        }
    }
    normalize_selections(ed.selections);
}

void cursor_right(Editor& ed) {
    for (auto& s : ed.selections) {
        if (!s.is_caret()) {
            s = Sel{s.hi(), s.hi()};
        } else if (s.active < ed.text.size()) {
            size_t p = utf8_next(ed.text, s.active);
            s = Sel{p, p};
        }
    }
    normalize_selections(ed.selections);
}

void cursor_word_left(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t p = wordLeftPos(ed.text, s.lo());
        s = Sel{p, p};
    }
    normalize_selections(ed.selections);
}

void cursor_word_right(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t p = wordRightPos(ed.text, s.hi());
        s = Sel{p, p};
    }
    normalize_selections(ed.selections);
}

void cursor_line_start(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t p = line_start(ed.text, s.lo());
        s = Sel{p, p};
    }
    normalize_selections(ed.selections);
}

void cursor_line_end(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t p = line_end(ed.text, s.hi());
        s = Sel{p, p};
    }
    normalize_selections(ed.selections);
}

void cursor_line_up(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t pos = s.lo();
        size_t cur_ls = line_start(ed.text, pos);
        if (cur_ls == 0) {
            // Already on first line: move to start of line
            s = Sel{0, 0};
        } else {
            size_t prev_ls = line_start(ed.text, cur_ls - 1);
            size_t col = pos - cur_ls;
            size_t prev_le = line_end(ed.text, prev_ls);
            size_t prev_len = prev_le - prev_ls;
            size_t new_pos = prev_ls + std::min(col, prev_len);
            s = Sel{new_pos, new_pos};
        }
    }
    normalize_selections(ed.selections);
}

void cursor_line_down(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t pos = s.hi();
        size_t cur_ls = line_start(ed.text, pos);
        size_t ns = next_line_start(ed.text, pos);
        if (ns == line_end(ed.text, pos)) {
            // On last line (no '\n' found): move to line end
            size_t le = line_end(ed.text, pos);
            s = Sel{le, le};
        } else {
            size_t col = pos - cur_ls;
            size_t next_le = line_end(ed.text, ns);
            size_t next_len = next_le - ns;
            size_t new_pos = ns + std::min(col, next_len);
            s = Sel{new_pos, new_pos};
        }
    }
    normalize_selections(ed.selections);
}

void cursor_doc_start(Editor& ed) {
    ed.selections = {Sel{0, 0}};
}

void cursor_doc_end(Editor& ed) {
    size_t e = ed.text.size();
    ed.selections = {Sel{e, e}};
}

// ── Selection extension ────────────────────────────────────────────────────

void select_set_range(Editor& ed, size_t anchor, size_t active) {
    anchor = std::min(anchor, ed.text.size());
    active = std::min(active, ed.text.size());
    ed.selections = {Sel{anchor, active}};
}

void select_add_range(Editor& ed, size_t anchor, size_t active) {
    anchor = std::min(anchor, ed.text.size());
    active = std::min(active, ed.text.size());
    ed.selections.push_back(Sel{anchor, active});
    normalize_selections(ed.selections);
}

void select_left(Editor& ed) {
    for (auto& s : ed.selections) {
        if (s.active > 0)
            s.active = utf8_prev(ed.text, s.active);
    }
    normalize_selections(ed.selections);
}

void select_right(Editor& ed) {
    for (auto& s : ed.selections) {
        if (s.active < ed.text.size())
            s.active = utf8_next(ed.text, s.active);
    }
    normalize_selections(ed.selections);
}

void select_word_left(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = wordLeftPos(ed.text, s.active);
    normalize_selections(ed.selections);
}

void select_word_right(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = wordRightPos(ed.text, s.active);
    normalize_selections(ed.selections);
}

void select_line_start(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = line_start(ed.text, s.active);
    normalize_selections(ed.selections);
}

void select_line_end(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = line_end(ed.text, s.active);
    normalize_selections(ed.selections);
}

void select_line_up(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t pos = s.active;
        size_t cur_ls = line_start(ed.text, pos);
        if (cur_ls == 0) {
            s.active = 0;
        } else {
            size_t prev_ls = line_start(ed.text, cur_ls - 1);
            size_t col = pos - cur_ls;
            size_t prev_le = line_end(ed.text, prev_ls);
            size_t prev_len = prev_le - prev_ls;
            s.active = prev_ls + std::min(col, prev_len);
        }
    }
    normalize_selections(ed.selections);
}

void select_line_down(Editor& ed) {
    for (auto& s : ed.selections) {
        size_t pos = s.active;
        size_t cur_ls = line_start(ed.text, pos);
        size_t ns = next_line_start(ed.text, pos);
        if (ns == line_end(ed.text, pos)) {
            s.active = ed.text.size();
        } else {
            size_t col = pos - cur_ls;
            size_t next_le = line_end(ed.text, ns);
            size_t next_len = next_le - ns;
            s.active = ns + std::min(col, next_len);
        }
    }
    normalize_selections(ed.selections);
}

void select_doc_start(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = 0;
    normalize_selections(ed.selections);
}

void select_doc_end(Editor& ed) {
    for (auto& s : ed.selections)
        s.active = ed.text.size();
    normalize_selections(ed.selections);
}

void select_all(Editor& ed) {
    ed.selections = {Sel{0, ed.text.size()}};
}

void select_add_next_occurrence(Editor& ed) {
    if (ed.selections.empty()) return;

    // Use the last selection as the search source
    Sel const& src = ed.selections.back();
    if (src.is_caret()) return; // caret: no text to search

    std::string needle = ed.text.substr(src.lo(), src.hi() - src.lo());
    if (needle.empty()) return;

    // Search after the last selection's hi
    size_t start = src.hi();
    size_t found = ed.text.find(needle, start);

    if (found == std::string::npos) {
        // Wrap around from the beginning
        found = ed.text.find(needle, 0);
        if (found == std::string::npos || found >= src.lo()) return;
    }

    size_t anchor = found;
    size_t active = found + needle.size();
    ed.selections.push_back(Sel{anchor, active});
    normalize_selections(ed.selections);
}

void select_add_cursor_up(Editor& ed) {
    std::vector<Sel> new_sels;
    for (auto const& s : ed.selections) {
        size_t pos = s.active;
        size_t cur_ls = line_start(ed.text, pos);
        if (cur_ls == 0) continue; // no line above
        size_t prev_ls = line_start(ed.text, cur_ls - 1);
        size_t col = pos - cur_ls;
        size_t prev_le = line_end(ed.text, prev_ls);
        size_t prev_len = prev_le - prev_ls;
        size_t new_pos = prev_ls + std::min(col, prev_len);
        new_sels.push_back(Sel{new_pos, new_pos});
    }
    for (auto& s : new_sels)
        ed.selections.push_back(s);
    normalize_selections(ed.selections);
}

void select_add_cursor_down(Editor& ed) {
    std::vector<Sel> new_sels;
    for (auto const& s : ed.selections) {
        size_t pos = s.active;
        size_t cur_ls = line_start(ed.text, pos);
        size_t ns = next_line_start(ed.text, pos);
        if (ns == line_end(ed.text, pos)) continue; // on last line
        size_t col = pos - cur_ls;
        size_t next_le = line_end(ed.text, ns);
        size_t next_len = next_le - ns;
        size_t new_pos = ns + std::min(col, next_len);
        new_sels.push_back(Sel{new_pos, new_pos});
    }
    for (auto& s : new_sels)
        ed.selections.push_back(s);
    normalize_selections(ed.selections);
}

void select_split_into_lines(Editor& ed) {
    std::vector<Sel> result;
    for (auto const& s : ed.selections) {
        if (s.is_caret() || s.lo() == s.hi()) {
            result.push_back(s);
            continue;
        }
        // Multi-char selection: split into one per line
        size_t lo = s.lo();
        size_t hi = s.hi();
        size_t cur = line_start(ed.text, lo);
        while (cur < hi) {
            size_t le = line_end(ed.text, cur);
            size_t seg_lo = std::max(cur, lo);
            size_t seg_hi = std::min(le, hi);
            if (seg_lo < seg_hi)
                result.push_back(Sel{seg_lo, seg_hi});
            size_t ns = next_line_start(ed.text, cur);
            if (ns == cur) break; // last line
            cur = ns;
        }
    }
    if (result.empty()) result = ed.selections;
    ed.selections = std::move(result);
    normalize_selections(ed.selections);
}

// ── Clipboard ─────────────────────────────────────────────────────────────

void clipboard_copy(Editor& ed) {
    ed.clipboard.clear();
    for (auto const& s : ed.selections) {
        if (!s.is_caret()) {
            ed.clipboard.push_back(ed.text.substr(s.lo(), s.hi() - s.lo()));
        } else {
            // Caret: capture whole line including '\n' if present
            size_t ls = line_start(ed.text, s.active);
            size_t ns = next_line_start(ed.text, s.active);
            ed.clipboard.push_back(ed.text.substr(ls, ns - ls));
        }
    }
}

bool clipboard_cut(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    clipboard_copy(ed);
    // Delete all selected text (selections that are carets delete the whole line)
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    for (auto const& s : ed.selections) {
        if (!s.is_caret()) {
            ops.push_back({s.lo(), s.hi(), ""});
        } else {
            // Cut whole line
            size_t ls = line_start(ed.text, s.active);
            size_t ns = next_line_start(ed.text, s.active);
            ops.push_back({ls, ns, ""});
        }
    }
    // apply_ops pushes undo; clipboard was already set above
    applyOps(ed, std::move(ops));
    return true;
}

bool clipboard_paste(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    if (ed.clipboard.empty()) return true;

    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());

    bool one_to_one = ed.clipboard.size() == ed.selections.size();
    std::string plain_text;
    if (!one_to_one) {
        for (auto const& fragment : ed.clipboard) {
            plain_text += fragment;
        }
    }

    for (size_t i = 0; i < ed.selections.size(); ++i) {
        auto const& s = ed.selections[i];
        std::string const& frag = one_to_one
                                      ? ed.clipboard[i]
                                      : plain_text;
        ops.push_back({s.lo(), s.hi(), frag});
    }
    applyOps(ed, std::move(ops));
    return true;
}

// ── History ───────────────────────────────────────────────────────────────

bool edit_undo(Editor& ed) {
    if (ed.undo_stack.empty()) return false;
    ed.redo_stack.push_back({ed.text, ed.selections});
    auto snap = std::move(ed.undo_stack.back());
    ed.undo_stack.pop_back();
    ed.text       = std::move(snap.text);
    ed.selections = std::move(snap.selections);
    return true;
}

bool edit_redo(Editor& ed) {
    if (ed.redo_stack.empty()) return false;
    ed.undo_stack.push_back({ed.text, ed.selections});
    auto snap = std::move(ed.redo_stack.back());
    ed.redo_stack.pop_back();
    ed.text       = std::move(snap.text);
    ed.selections = std::move(snap.selections);
    return true;
}

// ── Edit transforms ───────────────────────────────────────────────────────

bool edit_indent(Editor& ed, int tab_width, bool use_spaces) {
    if (ed.mode != Mode::edit) return false;
    auto lines = touchedLineStarts(ed);
    if (lines.empty()) return true;
    pushUndo(ed);
    std::string ins = use_spaces ? std::string(static_cast<size_t>(tab_width), ' ') : "\t";
    std::vector<AdjOp> ops;
    ops.reserve(lines.size());
    for (size_t ls : lines)
        ops.push_back({ls, 0, ins});
    adjustApply(ed, std::move(ops));
    return true;
}

bool edit_outdent(Editor& ed, int tab_width, bool use_spaces) {
    if (ed.mode != Mode::edit) return false;
    auto lines = touchedLineStarts(ed);
    if (lines.empty()) return true;
    pushUndo(ed);
    std::vector<AdjOp> ops;
    ops.reserve(lines.size());
    for (size_t ls : lines) {
        size_t del = 0;
        if (use_spaces) {
            while (del < static_cast<size_t>(tab_width) &&
                   ls + del < ed.text.size() &&
                   ed.text[ls + del] == ' ')
                ++del;
        } else {
            if (ls < ed.text.size() && ed.text[ls] == '\t')
                del = 1;
        }
        if (del > 0)
            ops.push_back({ls, del, ""});
    }
    adjustApply(ed, std::move(ops));
    return true;
}

bool edit_duplicate_line(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    pushUndo(ed);

    // Collect unique line-end positions from cursor positions (high→low).
    std::vector<std::pair<size_t, size_t>> le_to_si; // (line_end, sel_index)
    for (size_t i = 0; i < ed.selections.size(); ++i) {
        size_t le = line_end(ed.text, ed.selections[i].active);
        le_to_si.push_back({le, i});
    }
    std::sort(le_to_si.begin(), le_to_si.end(), [](auto& a, auto& b) {
        return a.first > b.first;
    });
    // Deduplicate same line_end
    le_to_si.erase(
        std::unique(le_to_si.begin(), le_to_si.end(),
                    [](auto& a, auto& b) { return a.first == b.first; }),
        le_to_si.end());

    // For each unique line (high→low): insert "\n" + line_content at line_end.
    // Cursor goes to le + 1 (start of inserted copy).
    // Track adjusted cursor targets in processing order.
    std::vector<std::pair<size_t, size_t>> new_cursors; // (pos, sel_index)

    for (auto const& [le, si] : le_to_si) {
        size_t ls  = line_start(ed.text, le > 0 ? le - 1 : 0);
        // Recompute ls in current text (positions below le are unchanged by
        // higher-offset ops already applied).
        ls = line_start(ed.text, le);
        std::string content = ed.text.substr(ls, le - ls);
        std::string ins     = "\n" + content;
        ed.text.insert(le, ins);

        ptrdiff_t delta = static_cast<ptrdiff_t>(ins.size());
        size_t new_cursor = le + 1; // start of inserted copy

        // Adjust previously recorded cursors (they're all above le)
        for (auto& [nc, _] : new_cursors)
            nc = static_cast<size_t>(static_cast<ptrdiff_t>(nc) + delta);

        new_cursors.push_back({new_cursor, si});

        // Adjust all selection endpoints >= le
        for (auto& s : ed.selections) {
            if (s.anchor >= le)
                s.anchor = static_cast<size_t>(static_cast<ptrdiff_t>(s.anchor) + delta);
            if (s.active >= le)
                s.active = static_cast<size_t>(static_cast<ptrdiff_t>(s.active) + delta);
        }
    }

    // Move each involved selection to its new cursor
    for (auto const& [nc, si] : new_cursors)
        ed.selections[si] = Sel{nc, nc};

    normalize_selections(ed.selections);
    return true;
}

bool edit_move_line_up(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    // Collect unique line starts for each cursor's line; skip first line.
    std::vector<size_t> lines = touchedLineStarts(ed);
    // Remove lines that are already the first line
    lines.erase(std::remove(lines.begin(), lines.end(), size_t{0}), lines.end());
    if (lines.empty()) return true;
    pushUndo(ed);

    // For each line (process low→high to avoid invalidation issues):
    // swap line with the line above it.
    // Since we process in ascending order and use the current text,
    // we do swaps from top to bottom.
    std::sort(lines.begin(), lines.end());

    for (size_t ls : lines) {
        // prev line
        size_t prev_ls = line_start(ed.text, ls - 1);
        size_t prev_le = line_end(ed.text, prev_ls);
        size_t cur_le  = line_end(ed.text, ls);

        // Extract both lines (without trailing '\n')
        std::string prev_line = ed.text.substr(prev_ls, prev_le - prev_ls);
        std::string cur_line  = ed.text.substr(ls, cur_le - ls);

        // Replace: [prev_ls .. cur_le) with cur_line + "\n" + prev_line
        std::string swapped = cur_line + "\n" + prev_line;
        size_t range_len = cur_le - prev_ls;
        size_t delta = swapped.size(); // same as range_len (no length change)
        (void)delta;

        ed.text.replace(prev_ls, range_len, swapped);

        // Adjust selections: the cursor was on ls (now at prev_ls)
        // Lines are same length so we just shift by the difference
        ptrdiff_t shift = static_cast<ptrdiff_t>(prev_ls) - static_cast<ptrdiff_t>(ls);
        for (auto& s : ed.selections) {
            auto adjust_if_on_line = [&](size_t& ep) {
                // If endpoint was on cur_line, move it up by the line distance
                if (ep >= ls && ep <= cur_le) {
                    ep = static_cast<size_t>(static_cast<ptrdiff_t>(ep) + shift);
                }
            };
            adjust_if_on_line(s.anchor);
            adjust_if_on_line(s.active);
        }
    }

    normalize_selections(ed.selections);
    return true;
}

bool edit_move_line_down(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<size_t> lines = touchedLineStarts(ed);
    if (lines.empty()) return true;
    pushUndo(ed);

    // Process in descending order to avoid invalidation
    std::sort(lines.begin(), lines.end(), std::greater<size_t>());

    for (size_t ls : lines) {
        size_t le = line_end(ed.text, ls);
        if (le >= ed.text.size()) continue; // last line, nothing below

        size_t next_ls = le + 1; // skip '\n'
        size_t next_le = line_end(ed.text, next_ls);

        std::string cur_line  = ed.text.substr(ls, le - ls);
        std::string next_line = ed.text.substr(next_ls, next_le - next_ls);

        std::string swapped = next_line + "\n" + cur_line;
        ed.text.replace(ls, next_le - ls, swapped);

        // Cursor was on cur_line, should now be on swapped-down position
        ptrdiff_t shift = static_cast<ptrdiff_t>(next_ls) - static_cast<ptrdiff_t>(ls);
        for (auto& s : ed.selections) {
            auto adjust_if_on_line = [&](size_t& ep) {
                if (ep >= ls && ep <= le) {
                    ep = static_cast<size_t>(static_cast<ptrdiff_t>(ep) + shift);
                }
            };
            adjust_if_on_line(s.anchor);
            adjust_if_on_line(s.active);
        }
    }

    normalize_selections(ed.selections);
    return true;
}

bool edit_delete_line(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    auto lines = touchedLineStarts(ed);
    if (lines.empty()) return true;

    // Build one delete op per touched line (high→low via apply_ops).
    // Non-last line: delete [ls, le+1) = line content + '\n'.
    // Last line (no trailing '\n'): consume the preceding '\n' too so the
    // document does not gain an empty trailing line.
    std::vector<EditOp> ops;
    ops.reserve(lines.size());
    for (size_t ls : lines) {
        size_t le = line_end(ed.text, ls);
        if (le < ed.text.size()) {
            // There is a '\n' at le: delete [ls, le+1)
            ops.push_back({ls, le + 1, ""});
        } else {
            // Last line (no trailing newline): also remove preceding '\n'
            size_t del_start = (ls > 0) ? ls - 1 : 0;
            ops.push_back({del_start, ed.text.size(), ""});
        }
    }
    applyOps(ed, std::move(ops));
    return true;
}

bool edit_join_lines(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    ops.reserve(ed.selections.size());
    // For each cursor, replace '\n' at end of its line with ' '
    for (auto const& s : ed.selections) {
        size_t le = line_end(ed.text, s.active);
        if (le < ed.text.size()) { // there is a '\n'
            ops.push_back({le, le + 1, " "});
        }
        // else on last line: no-op for this cursor
    }
    if (!ops.empty())
        applyOps(ed, std::move(ops));
    return true;
}

bool edit_uppercase(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    for (auto const& s : ed.selections) {
        if (s.is_caret()) continue;
        std::string upper = ed.text.substr(s.lo(), s.hi() - s.lo());
        for (char& c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        // cursor_at = hi - lo (stay at end of selection, i.e. keep selection)
        // We use explicit cursor to end so selection is preserved as-is.
        // For simplicity: cursor at lo + len (= hi). Use default.
        ops.push_back({s.lo(), s.hi(), std::move(upper)});
    }
    if (!ops.empty())
        applyOps(ed, std::move(ops));
    return true;
}

bool edit_lowercase(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    for (auto const& s : ed.selections) {
        if (s.is_caret()) continue;
        std::string lower = ed.text.substr(s.lo(), s.hi() - s.lo());
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        ops.push_back({s.lo(), s.hi(), std::move(lower)});
    }
    if (!ops.empty())
        applyOps(ed, std::move(ops));
    return true;
}

bool edit_swap_case(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    for (auto const& s : ed.selections) {
        if (s.is_caret()) continue;
        std::string swapped = ed.text.substr(s.lo(), s.hi() - s.lo());
        for (char& c : swapped) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isupper(uc))
                c = static_cast<char>(std::tolower(uc));
            else if (std::islower(uc))
                c = static_cast<char>(std::toupper(uc));
        }
        ops.push_back({s.lo(), s.hi(), std::move(swapped)});
    }
    if (!ops.empty())
        applyOps(ed, std::move(ops));
    return true;
}

bool edit_sort_lines(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    auto line_starts = touchedLineStarts(ed);
    if (line_starts.size() < 2) return true;

    pushUndo(ed);

    // Group touched line starts into contiguous runs.  Two successive entries
    // are contiguous when the second equals next_line_start of the first.
    // Sort each contiguous group independently so disjoint selections do not
    // destroy untouched lines that lie between them.
    std::vector<std::vector<size_t>> groups;
    groups.push_back({line_starts[0]});
    for (size_t i = 1; i < line_starts.size(); ++i) {
        size_t prev = groups.back().back();
        if (next_line_start(ed.text, prev) == line_starts[i]) {
            groups.back().push_back(line_starts[i]);
        } else {
            groups.push_back({line_starts[i]});
        }
    }

    // Build one replacement op per group with >= 2 lines.
    // Compute all ops from the original text before mutating.
    std::vector<AdjOp> ops;
    for (auto const& group : groups) {
        if (group.size() < 2) continue;
        size_t first_ls = group.front();
        size_t last_le  = line_end(ed.text, group.back());

        std::vector<std::string> lns;
        lns.reserve(group.size());
        for (size_t ls : group)
            lns.push_back(ed.text.substr(ls, line_end(ed.text, ls) - ls));

        std::sort(lns.begin(), lns.end());

        std::string replacement;
        for (size_t i = 0; i < lns.size(); ++i) {
            if (i > 0) replacement += '\n';
            replacement += lns[i];
        }
        ops.push_back({first_ls, last_le - first_ls, replacement});
    }

    adjustApply(ed, std::move(ops));
    return true;
}

bool edit_transpose(Editor& ed) {
    if (ed.mode != Mode::edit) return false;
    std::vector<EditOp> ops;
    for (auto const& s : ed.selections) {
        if (!s.is_caret()) continue;
        size_t pos = s.active;
        if (ed.text.empty()) continue;

        size_t a, b_end;
        if (pos == ed.text.size()) {
            // At end: swap last two codepoints
            if (pos < 2) continue;
            a     = utf8_prev(ed.text, pos);
            b_end = pos;
            size_t a_prev = utf8_prev(ed.text, a);
            // swap [a_prev, a) with [a, b_end)
            std::string ca = ed.text.substr(a_prev, a - a_prev);
            std::string cb = ed.text.substr(a, b_end - a);
            // Replace [a_prev, b_end) with cb + ca; cursor stays at end
            ops.push_back({a_prev, b_end, cb + ca, b_end - a_prev});
        } else if (pos == 0) {
            continue; // nothing before cursor
        } else {
            // Swap char at [pos-1..pos) with char at [pos..utf8_next(pos))
            size_t char_a_start = utf8_prev(ed.text, pos);
            size_t char_b_end   = utf8_next(ed.text, pos);
            std::string ca = ed.text.substr(char_a_start, pos - char_a_start);
            std::string cb = ed.text.substr(pos, char_b_end - pos);
            // Cursor moves to char_b_end after the swap
            ops.push_back({char_a_start, char_b_end, cb + ca,
                           static_cast<size_t>(cb.size() + ca.size())});
        }
    }
    if (!ops.empty())
        applyOps(ed, std::move(ops));
    return true;
}

bool edit_toggle_comment(Editor& ed, std::string_view line_comment_token) {
    if (ed.mode != Mode::edit) return false;
    if (line_comment_token.empty()) return true;

    auto lines = touchedLineStarts(ed);
    if (lines.empty()) return true;

    // Determine whether ALL touched lines start with the token.
    bool all_commented = true;
    for (size_t ls : lines) {
        if (ed.text.compare(ls, line_comment_token.size(), line_comment_token) != 0) {
            all_commented = false;
            break;
        }
    }

    pushUndo(ed);

    if (all_commented) {
        // Remove token from each line start
        std::vector<AdjOp> ops;
        ops.reserve(lines.size());
        for (size_t ls : lines)
            ops.push_back({ls, line_comment_token.size(), ""});
        adjustApply(ed, std::move(ops));
    } else {
        // Add token to each line start
        std::vector<AdjOp> ops;
        ops.reserve(lines.size());
        std::string token_str(line_comment_token);
        for (size_t ls : lines)
            ops.push_back({ls, 0, token_str});
        adjustApply(ed, std::move(ops));
    }
    return true;
}

// ── Snapshot accessors ────────────────────────────────────────────────────

std::string_view snapshot_text(Editor const& ed) noexcept {
    return ed.text;
}

std::vector<Sel> const& snapshot_selections(Editor const& ed) noexcept {
    return ed.selections;
}

} // namespace ref
