#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(what.size()),
                     what.data());
        ++failures;
    }
}

// Files permitted to open a raw stream, each with the reason. Keeping the list
// here rather than in prose means an exception is a reviewed edit to a test,
// not an unremarked line of code.
struct Exemption {
    std::string_view path;
    std::string_view reason;
};

constexpr Exemption kExemptions[] = {
    // The seam's own implementations ARE the filesystem access.
    {"src/platform/linux_files.cpp", "implements the seam"},
    {"src/platform/windows_files.cpp", "implements the seam"},
    // Tests read and write files out-of-band on purpose: a test that used the
    // seam to check the seam would be its own oracle.
    {"tests/", "test fixtures read out-of-band by design"},
};

bool exempt(const std::string& relative) {
    return std::any_of(std::begin(kExemptions), std::end(kExemptions),
                       [&](const Exemption& entry) {
                           return relative.find(entry.path) != std::string::npos;
                       });
}

// The raw-stream spellings that reach the filesystem directly. std::fopen is
// included because it is the C-shaped way to do the same thing.
constexpr std::string_view kForbidden[] = {
    "std::ifstream", "std::ofstream", "std::fstream", "std::fopen",
};

// A single line may opt out by carrying this marker plus a reason. Line-scoped
// exceptions keep the rest of the file guarded.
constexpr std::string_view kExemptionMarker = "seam-exempt:";

fs::path repositoryRoot() {
    auto current = fs::current_path();
    for (int depth = 0; depth < 6; ++depth) {
        if (fs::exists(current / "CMakeLists.txt") &&
            fs::exists(current / "src") && fs::exists(current / "include")) {
            return current;
        }
        if (!current.has_parent_path()) break;
        current = current.parent_path();
    }
    return {};
}

std::string readSource(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

// Every filesystem read and write must go through include/ssg/platform_files.h.
// Routing the existing call sites once is worthless without this: the next
// feature would simply add a fresh raw stream, and the seam would rot back into
// being optional.
void libraryAndApplicationCodeUseTheFileSeam() {
    const auto root = repositoryRoot();
    check(!root.empty(), "repository root is discoverable");
    if (root.empty()) return;

    std::vector<std::string> offenders;
    std::size_t scanned = 0;

    for (const auto& directory : {"src", "include", "apps"}) {
        const auto base = root / directory;
        if (!fs::exists(base)) continue;
        for (fs::recursive_directory_iterator it{base}, end; it != end; ++it) {
            if (!it->is_regular_file()) continue;
            const auto extension = it->path().extension().string();
            if (extension != ".cpp" && extension != ".h" && extension != ".hpp") {
                continue;
            }
            const auto relative =
                fs::relative(it->path(), root).generic_string();
            if (exempt(relative)) continue;
            ++scanned;

            const auto source = readSource(it->path());
            // Line-scoped so a single justified exception cannot silently
            // license the whole file. A file-level exemption for ssg_main.cpp,
            // say, would let a future raw init.lua read slip back in.
            std::size_t line = 1;
            std::size_t start = 0;
            std::string previous;
            while (start <= source.size()) {
                const auto end = source.find('\n', start);
                const auto text = source.substr(
                    start, end == std::string::npos ? std::string::npos
                                                    : end - start);
                // The marker may sit on the line itself or on the line above,
                // so a long call can carry its justification without being
                // wrapped awkwardly.
                const bool exempted =
                    text.find(kExemptionMarker) != std::string::npos ||
                    previous.find(kExemptionMarker) != std::string::npos;
                if (!exempted) {
                    for (const auto spelling : kForbidden) {
                        if (text.find(spelling) != std::string::npos) {
                            offenders.push_back(relative + ":" +
                                                std::to_string(line) +
                                                " uses " +
                                                std::string{spelling});
                        }
                    }
                }
                if (end == std::string::npos) break;
                previous = text;
                start = end + 1;
                ++line;
            }
        }
    }

    // Guards that scan nothing pass vacuously. Pin that real files were seen.
    check(scanned > 100, "the guard actually scanned the tree");

    for (const auto& offender : offenders) {
        std::fprintf(stderr,
                     "FAIL: raw stream I/O outside the seam: %s\n"
                     "      Use ssg::readFile / createFileExclusively /\n"
                     "      replaceFileAtomically from ssg/platform_files.h,\n"
                     "      or add a justified entry to kExemptions.\n",
                     offender.c_str());
    }
    check(offenders.empty(), "no raw stream I/O outside the seam");
}

}  // namespace

int main() {
    libraryAndApplicationCodeUseTheFileSeam();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
