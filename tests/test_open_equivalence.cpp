#include "test_helpers.h"

#include <ssg/Document.h>
#include <ssg/RecoveryManager.h>
#include <ssg/TextCodec.h>
#include <ssg/Workspace.h>

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

// LF-1: the IMMUTABLE open-equivalence golden.
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

fs::path uniqueRoot() {
    auto base = fs::temp_directory_path() /
                ("ssg-open-equiv-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void writeBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// FNV-1a 64-bit: a deterministic content hash for the equivalence oracle (change
// detection, not a cryptographic guarantee).
std::string hashHex(std::string_view bytes) {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << h;
    return out.str();
}

std::string encodingName(ssg::TextEncoding e) {
    switch (e) {
    case ssg::TextEncoding::Utf8: return "utf8";
    case ssg::TextEncoding::Utf8Bom: return "utf8_bom";
    case ssg::TextEncoding::Utf16le: return "utf16le";
    case ssg::TextEncoding::Utf16be: return "utf16be";
    case ssg::TextEncoding::Windows1252: return "windows1252";
    case ssg::TextEncoding::Iso88591: return "iso88591";
    }
    return "?";
}

std::string endingName(ssg::LineEnding e) {
    switch (e) {
    case ssg::LineEnding::Lf: return "lf";
    case ssg::LineEnding::Crlf: return "crlf";
    case ssg::LineEnding::Cr: return "cr";
    case ssg::LineEnding::Mixed: return "mixed";
    }
    return "?";
}

std::string kindName(ssg::FileContentKind k) {
    switch (k) {
    case ssg::FileContentKind::Text: return "text";
    case ssg::FileContentKind::Binary: return "binary";
    case ssg::FileContentKind::DecodeFailure: return "decode_failure";
    }
    return "?";
}

std::string modeName(ssg::DocumentMode m) {
    switch (m) {
    case ssg::DocumentMode::Edit: return "edit";
    case ssg::DocumentMode::ReadOnly: return "read_only";
    case ssg::DocumentMode::Diff: return "diff";
    }
    return "?";
}

// The per-line terminator summary, computed independently of the workspace via
// the public decoder, so the record captures mixed-EOL fidelity directly.
std::string terminatorSummary(std::string_view bytes) {
    auto decoded = ssg::TextCodec{}.decode(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    if (!decoded.accepted()) {
        std::ostringstream out;
        out << "err:" << static_cast<int>(decoded.error->code) << "@"
            << decoded.error->utf8Offset;
        return out.str();
    }
    int lf = 0, crlf = 0, cr = 0, none = 0;
    for (auto t : decoded.text->lineTerminators) {
        switch (t) {
        case ssg::LineTerminator::Lf: ++lf; break;
        case ssg::LineTerminator::Crlf: ++crlf; break;
        case ssg::LineTerminator::Cr: ++cr; break;
        case ssg::LineTerminator::None: ++none; break;
        }
    }
    std::ostringstream out;
    out << "lf:" << lf << ",crlf:" << crlf << ",cr:" << cr << ",none:" << none;
    return out.str();
}

// The bytes a save of a freshly opened (non-dirty) file would produce, via the
// public encode path, hashed — captures encode round-trip fidelity independent
// of the workspace's save machinery.
std::string saveHash(std::string_view bytes) {
    auto decoded = ssg::TextCodec{}.decode(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    if (!decoded.accepted()) return "n/a";
    auto encoded = ssg::TextCodec{}.encode(*decoded.text);
    if (!encoded.accepted()) return "encode_err";
    return hashHex(std::string_view{
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
    auto root = uniqueRoot();
    auto recovery = ssg::RecoveryManager::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    std::ostringstream out;
    for (const auto& entry : corpus()) {
        writeBytes(root / entry.name, entry.bytes);
        auto opened = workspace.openFile(entry.name);
        out << "name=" << entry.name;
        if (!opened.accepted()) {
            out << " open_error=" << static_cast<int>(opened.error) << "\n";
            continue;
        }
        auto id = *opened.document;
        auto state = workspace.state(id);
        auto snapshot = workspace.document(id).snapshot();
        out << " kind=" << kindName(state->contentKind)
            << " encoding=" << encodingName(state->encoding.encoding)
            << " bom=" << (state->encoding.hadBom ? 1 : 0)
            << " eol=" << endingName(state->encoding.lineEnding)
            << " final_nl=" << (state->encoding.finalNewline ? 1 : 0)
            << " dirty=" << (state->dirty ? 1 : 0)
            << " revision=" << snapshot.revision.value()
            << " mode=" << modeName(snapshot.mode)
            << " texthash=" << hashHex(snapshot.text)
            << " terms=" << terminatorSummary(entry.bytes)
            << " save=" << saveHash(entry.bytes) << "\n";
    }
    fs::remove_all(root);
    return out.str();
}

std::string readFile(const char* path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(openEquivalenceMatchesGolden) {
    const std::string actual = capture();
    const char* path = SSG_OPEN_GOLDEN;
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << actual;
        std::cout << "  regenerated " << path << "\n";
        ++passed;
        return;
    }
    const std::string expected = readFile(path);
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
    RUN(openEquivalenceMatchesGolden);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
