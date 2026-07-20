// GraphemeBreakTest oracle for compute_cell_run.
//
// Reads GraphemeBreakTest.txt (Unicode 15.0.0) and for each applicable line
// verifies that compute_cell_run's segmentation matches the official break
// positions.  Lines containing U+000D or U+000A are skipped; the compute_cell_run
// API forbids CR/LF in its input (layout.h contract: "single logical line").
//
// The oracle checks:
//   1. The number of grapheme clusters (spans) equals the number of ÷ boundaries
//      minus 1 (first and last ÷ are line start/end markers).
//   2. The byte_offset of every span matches the UTF-8 byte offset of the
//      corresponding break position in the encoded string.
//   3. The byte_len of every span (including the final span) is exactly the
//      distance from its byte_offset to the next break (or end of string).
//
// The oracle does NOT check CellKind or width; those are implementation details.

#include <ssg/layout.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UNICODE_DATA_DIR
#  error "UNICODE_DATA_DIR must be defined (path to data/unicode/)"
#endif

static std::string encodeUtf8(uint32_t cp) {
    std::string out;
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6)  & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return out;
}

// Parses one test line: "÷ HHHH × HHHH ÷ ..." into a list of (codepoint, is_break_after).
// Returns false if line should be skipped (blank, comment, or contains CR/LF).
struct Entry { uint32_t cp; bool break_after; };

static bool parseLine(const std::string& line,
                        std::vector<Entry>& out_entries,
                        std::vector<size_t>& out_break_bytes) {
    out_entries.clear();
    out_break_bytes.clear();

    const std::string stripped = [&]{
        auto pos = line.find('#');
        return (pos != std::string::npos) ? line.substr(0, pos) : line;
    }();

    // Tokenise on ÷ (UTF-8: C3 B7) and × (UTF-8: C3 97)
    // We look for ASCII-ish patterns: each token is either a hex code point or a break marker.
    std::string s = stripped;
    // Normalise: replace ÷ and × with ASCII sentinels
    std::string norm;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            if (c2 == 0xB7) { norm += 'D'; i += 2; continue; } // ÷
            if (c2 == 0x97) { norm += 'X'; i += 2; continue; } // ×
        }
        norm += static_cast<char>(c);
        ++i;
    }

    // Scan: alternating break/no-break markers and hex code points
    std::istringstream ss(norm);
    std::string tok;
    bool first = true;
    bool last_was_break_marker = false;
    while (ss >> tok) {
        if (tok == "D" || tok == "X") {
            if (first && tok == "D") { first = false; last_was_break_marker = true; continue; }
            last_was_break_marker = (tok == "D");
        } else {
            // hex code point
            uint32_t cp = std::stoul(tok, nullptr, 16);
            // Skip lines containing CR or LF
            if (cp == 0x000D || cp == 0x000A) return false;
            bool break_before = last_was_break_marker && !out_entries.empty();
            // We record break_after on the PREVIOUS entry
            if (!out_entries.empty()) {
                out_entries.back().break_after = last_was_break_marker;
            }
            out_entries.push_back({cp, false});
            last_was_break_marker = false;
            first = false;
        }
    }
    if (!out_entries.empty()) out_entries.back().break_after = true; // final ÷

    if (out_entries.empty()) return false;

    // Compute break byte positions (excluding position 0 and total_bytes)
    size_t byte_pos = 0;
    for (size_t i = 0; i < out_entries.size(); ++i) {
        if (i > 0 && out_entries[i - 1].break_after) {
            out_break_bytes.push_back(byte_pos);
        }
        byte_pos += encodeUtf8(out_entries[i].cp).size();
    }

    return !out_entries.empty();
}

int main() {
    const std::string path = std::string(UNICODE_DATA_DIR) + "/GraphemeBreakTest.txt";
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "Cannot open %s\n", path.c_str());
        return 1;
    }

    int passed = 0, failed = 0, skipped = 0;
    int line_num = 0;
    std::string line;

    while (std::getline(f, line)) {
        ++line_num;
        std::vector<Entry> entries;
        std::vector<size_t> break_bytes;

        if (!parseLine(line, entries, break_bytes)) {
            ++skipped;
            continue;
        }

        // Build UTF-8 string
        std::string utf8;
        for (const auto& e : entries)
            utf8 += encodeUtf8(e.cp);

        // Run compute_cell_run
        const auto run = ssg::computeCellRun(utf8);

        // Expected clusters = number of ÷-separated segments
        // break_bytes has the byte offsets where breaks occur (between spans)
        const size_t expected_spans = break_bytes.size() + 1;

        bool ok = true;
        if (run.spans.size() != expected_spans) {
            ok = false;
        } else {
            // Helpers: expected byte_offset and byte_len for span i.
            // break_bytes[i] is the byte offset where span i+1 begins.
            auto expected_offset = [&](size_t i) -> uint32_t {
                return static_cast<uint32_t>((i == 0) ? 0 : break_bytes[i - 1]);
            };
            auto expected_len = [&](size_t i) -> uint32_t {
                const size_t start = (i == 0) ? 0 : break_bytes[i - 1];
                const size_t end_pos = (i < break_bytes.size()) ? break_bytes[i] : utf8.size();
                return static_cast<uint32_t>(end_pos - start);
            };

            for (size_t i = 0; i < run.spans.size(); ++i) {
                if (run.spans[i].byte_offset != expected_offset(i) ||
                    run.spans[i].byte_len    != expected_len(i)) {
                    ok = false;
                    break;
                }
            }
        }

        if (ok) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr, "FAIL line %d: %s\n", line_num, line.c_str());
            std::fprintf(stderr, "  expected %zu spans, got %zu\n",
                         expected_spans, run.spans.size());
            for (size_t i = 0; i < run.spans.size(); ++i) {
                std::fprintf(stderr, "  span[%zu] offset=%u len=%u (expected offset=%zu",
                             i, run.spans[i].byte_offset, run.spans[i].byte_len,
                             (i == 0) ? 0u : break_bytes[i - 1]);
                if (i < run.spans.size()) {
                    const size_t exp_start = (i == 0) ? 0 : break_bytes[i - 1];
                    const size_t exp_end   = (i < break_bytes.size()) ? break_bytes[i] : utf8.size();
                    std::fprintf(stderr, " len=%zu", exp_end - exp_start);
                }
                std::fprintf(stderr, ")\n");
            }
            std::fprintf(stderr, "  expected break bytes:");
            for (auto b : break_bytes) std::fprintf(stderr, " %zu", b);
            std::fprintf(stderr, "\n");
        }
    }

    std::fprintf(stdout, "GraphemeBreakTest oracle: passed=%d failed=%d skipped=%d\n",
                 passed, failed, skipped);
    return failed > 0 ? 1 : 0;
}
