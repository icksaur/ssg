#include "test_helpers.h"

#include <ssg/document.h>
#include <ssg/recovery.h>
#include <ssg/text_encoding.h>
#include <ssg/workspace.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// LF-1 (doc/spec-large-files-loading.md): the IMMUTABLE open-equivalence golden.
//
// For a fixed encoding corpus this records, from the CURRENT open path, the
// fields that must stay byte-identical across LF-2..LF-4b: content
// classification, decode status (encoding/BOM/EOL/final-newline), initial dirty
// flag, document revision/mode, a hash of the opened text, the per-line
// line-terminator summary, and a hash of the re-encoded (save) bytes.  LF-2/3/4
// must not change this golden; SSG_REGEN_GOLDEN=1 rewrites it after an
// INTENTIONAL behavior change only.

namespace {

namespace fs = std::filesystem;

fs::path unique_root() {
    auto base = fs::temp_directory_path() /
                ("ssg-open-equiv-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// FNV-1a 64-bit: a deterministic content hash for the equivalence oracle (change
// detection, not a cryptographic guarantee).
std::string hash_hex(std::string_view bytes) {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << h;
    return out.str();
}

std::string encoding_name(ssg::TextEncoding e) {
    switch (e) {
    case ssg::TextEncoding::utf8: return "utf8";
    case ssg::TextEncoding::utf8_bom: return "utf8_bom";
    case ssg::TextEncoding::utf16le: return "utf16le";
    case ssg::TextEncoding::utf16be: return "utf16be";
    case ssg::TextEncoding::windows1252: return "windows1252";
    case ssg::TextEncoding::iso88591: return "iso88591";
    }
    return "?";
}

std::string ending_name(ssg::LineEnding e) {
    switch (e) {
    case ssg::LineEnding::lf: return "lf";
    case ssg::LineEnding::crlf: return "crlf";
    case ssg::LineEnding::cr: return "cr";
    case ssg::LineEnding::mixed: return "mixed";
    }
    return "?";
}

std::string kind_name(ssg::FileContentKind k) {
    switch (k) {
    case ssg::FileContentKind::text: return "text";
    case ssg::FileContentKind::binary: return "binary";
    case ssg::FileContentKind::decode_failure: return "decode_failure";
    }
    return "?";
}

std::string mode_name(ssg::DocumentMode m) {
    switch (m) {
    case ssg::DocumentMode::edit: return "edit";
    case ssg::DocumentMode::read_only: return "read_only";
    case ssg::DocumentMode::diff: return "diff";
    }
    return "?";
}

// The per-line terminator summary, computed independently of the workspace via
// the public decoder, so the record captures mixed-EOL fidelity directly.
std::string terminator_summary(std::string_view bytes) {
    auto decoded = ssg::decode_text(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    if (!decoded.accepted()) {
        std::ostringstream out;
        out << "err:" << static_cast<int>(decoded.error->code) << "@"
            << decoded.error->utf8_offset;
        return out.str();
    }
    int lf = 0, crlf = 0, cr = 0, none = 0;
    for (auto t : decoded.text->line_terminators) {
        switch (t) {
        case ssg::LineTerminator::lf: ++lf; break;
        case ssg::LineTerminator::crlf: ++crlf; break;
        case ssg::LineTerminator::cr: ++cr; break;
        case ssg::LineTerminator::none: ++none; break;
        }
    }
    std::ostringstream out;
    out << "lf:" << lf << ",crlf:" << crlf << ",cr:" << cr << ",none:" << none;
    return out.str();
}

// The bytes a save of a freshly opened (non-dirty) file would produce, via the
// public encode path, hashed — captures encode round-trip fidelity independent
// of the workspace's save machinery.
std::string save_hash(std::string_view bytes) {
    auto decoded = ssg::decode_text(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    if (!decoded.accepted()) return "n/a";
    auto encoded = ssg::encode_text(*decoded.text);
    if (!encoded.accepted()) return "encode_err";
    return hash_hex(std::string_view{
        reinterpret_cast<const char*>(encoded.bytes.data()),
        encoded.bytes.size()});
}

struct Entry {
    std::string name;
    std::string bytes;
};

std::vector<Entry> corpus() {
    std::string bom = "\xef\xbb\xbf";
    std::string utf16 = std::string("\xff\xfe", 2) +
                        std::string("h\0i\0", 4);  // FF FE + "hi" LE (has NUL)
    return {
        {"utf8_lf", "line one\nline two\nthird line\n"},
        {"utf8_no_final_nl", "alpha\nbeta\ngamma"},
        {"utf8_crlf", "a\r\nb\r\nc\r\n"},
        {"utf8_mixed_eol", "a\r\nb\nc\r\n"},
        {"utf8_bom", bom + "hello\nworld\n"},
        {"utf8_multibyte", "h\xc3\xa9llo\nw\xc3\xb6rld\n\xe2\x98\x83\n"},
        {"empty", ""},
        {"binary_nul", std::string("abc\0def", 7)},
        {"malformed", "abc\xc0\xc0" "def"},
        {"utf16le_bom", utf16},
        {"nul_before_malformed", std::string("\0\xc0", 2)},
        {"malformed_before_nul", std::string("\xc0\0", 2)},
    };
}

std::string capture() {
    auto root = unique_root();
    auto recovery = ssg::RecoveryActions::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    std::ostringstream out;
    for (const auto& entry : corpus()) {
        write_bytes(root / entry.name, entry.bytes);
        auto opened = workspace.open_file(entry.name);
        out << "name=" << entry.name;
        if (!opened.accepted()) {
            out << " open_error=" << static_cast<int>(opened.error) << "\n";
            continue;
        }
        auto id = *opened.document;
        auto state = workspace.state(id);
        auto snapshot = workspace.document(id).snapshot();
        out << " kind=" << kind_name(state->content_kind)
            << " encoding=" << encoding_name(state->encoding.encoding)
            << " bom=" << (state->encoding.had_bom ? 1 : 0)
            << " eol=" << ending_name(state->encoding.line_ending)
            << " final_nl=" << (state->encoding.final_newline ? 1 : 0)
            << " dirty=" << (state->dirty ? 1 : 0)
            << " revision=" << snapshot.revision.value()
            << " mode=" << mode_name(snapshot.mode)
            << " texthash=" << hash_hex(snapshot.text)
            << " terms=" << terminator_summary(entry.bytes)
            << " save=" << save_hash(entry.bytes) << "\n";
    }
    fs::remove_all(root);
    return out.str();
}

std::string read_file(const char* path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(open_equivalence_matches_golden) {
    const std::string actual = capture();
    const char* path = SSG_OPEN_GOLDEN;
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << actual;
        std::cout << "  regenerated " << path << "\n";
        ++passed;
        return;
    }
    const std::string expected = read_file(path);
    if (actual != expected) {
        std::cerr << "  open-equivalence golden mismatch\n--- actual ---\n"
                  << actual << "--- expected ---\n"
                  << expected << "--- end ---\n";
        ++failed;
    } else {
        ++passed;
    }
}

}  // namespace

int main() {
    RUN(open_equivalence_matches_golden);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
