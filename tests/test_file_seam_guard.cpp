#include "test_helpers.h"

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
    {"src/platform/platform_files.cpp", "implements shared seam operations"},
    {"src/platform/linux/linux_files.cpp", "implements the Linux seam"},
    {"src/platform/windows/windows_files.cpp", "implements the Windows seam"},
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

// Spellings that reach the filesystem directly. std::fopen is included because
// it is the C-shaped way to do the same thing.
//
// Filesystem mutations and traversal belong behind the same seam as byte I/O.
constexpr std::string_view kForbidden[] = {
    "std::ifstream",           "std::ofstream",
    "std::fstream",            "std::fopen",
    "std::filesystem::rename", "std::filesystem::copy_file",
    "std::filesystem::remove(", "::create_directories",
    "::create_directory(",     "::remove_all",
    "::directory_iterator",    "::recursive_directory_iterator",
    "::exists(",               "::is_regular_file(",
    "::is_directory(",         "::file_size(",
    "::last_write_time(",      "::symlink_status(",
    "::canonical(",            "::weakly_canonical(",
};

// A single line may opt out by carrying this marker plus a reason. Line-scoped
// exceptions keep the rest of the file guarded.
constexpr std::string_view kExemptionMarker = "seam-exempt:";

constexpr std::string_view kPortableTestFiles[] = {
    "tests/test_helpers.h",
    "tests/test_command_dispatch.cpp",
    "tests/test_script_host.cpp",
    "tests/test_theme.cpp",
    "tests/test_tui_contract.cpp",
    "tests/test_system_clipboard_reader.cpp",
};

constexpr std::string_view kPosixTestSpellings[] = {
    "<unistd.h>",
    "<sys/stat.h>",
    "::getpid(",
    "::chmod(",
};

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

// Every filesystem read and write must go through platform_files.h.
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
            // license the whole file. A file-level exemption for main.cpp,
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
                     "      Use the operation in ssg/platform_files.h,\n"
                     "      or add a justified entry to kExemptions.\n",
                     offender.c_str());
    }
    check(offenders.empty(), "no raw stream I/O outside the seam");
}

struct PlatformHeaderOwner {
    std::string_view spelling;
    std::string_view path;
    std::string_view reason;
};

constexpr PlatformHeaderOwner kPlatformHeaderOwners[] = {
    {"<windows.h>", "src/platform/windows/windows_files.cpp",
     "implements Windows file services"},
    {"<windows.h>", "src/platform/windows/WindowsFilesystemWatcher.cpp",
     "implements the Windows filesystem watcher"},
    {"<windows.h>", "src/platform/windows/WindowsGitMetadataWatcher.cpp",
     "implements the Windows Git metadata watcher"},
    {"<windows.h>", "src/platform/windows/WindowsPlatformRuntime.cpp",
     "implements Windows process runtime services"},
    {"<windows.h>", "src/platform/windows/WindowsConsoleInput.h",
     "defines Windows console input translation"},
    {"<windows.h>", "src/platform/windows/WindowsTerminal.h",
     "defines the Windows console terminal adapter"},
    {"<windows.h>", "src/platform/windows/WindowsTerminal.cpp",
     "implements Windows console terminal services"},
    {"<windows.h>",
     "src/platform/windows/WindowsSystemClipboardReader.cpp",
     "implements Windows clipboard services"},
    {"<sys/inotify.h>", "src/platform/linux/LinuxFilesystemWatcher.cpp",
     "implements the Linux filesystem watcher"},
    {"<sys/inotify.h>", "src/platform/linux/LinuxGitMetadataWatcher.cpp",
     "implements the Linux Git metadata watcher"},
    {"<termios.h>", "src/platform/linux/LinuxTerminal.cpp",
     "implements Linux terminal state"},
    {"<git2.h>", "src/GitRepository.cpp",
     "implements the core-owned Git repository adapter"},
    {"<lua.h>", "src/LuaCommandHost.cpp",
     "implements the Lua adapter"},
};

constexpr std::string_view kNativeSpellings[] = {
    "#include <fcntl.h>",
    "#include <poll.h>",
    "#include <signal.h>",
    "#include <sys/",
    "#include <termios.h>",
    "#include <unistd.h>",
    "#include <windows.h>",
    " ::execv(",
    " ::fork(",
    " ::getpid(",
    " ::pipe(",
    " ::read(",
    " ::sigaction(",
    " ::waitpid(",
    " ::write(",
    "clock_gettime(",
    "CLOCK_MONOTONIC",
    "CompareStringOrdinal(",
};

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

void platformHeadersStayInTheirAdapters() {
    const auto root = repositoryRoot();
    if (root.empty()) return;

    std::vector<std::string> offenders;
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
            const auto source = readSource(it->path());
            for (const auto& owner : kPlatformHeaderOwners) {
                if (source.find("#include " + std::string{owner.spelling}) ==
                    std::string::npos) {
                    continue;
                }
                const bool allowed = std::any_of(
                    std::begin(kPlatformHeaderOwners),
                    std::end(kPlatformHeaderOwners),
                    [&](const PlatformHeaderOwner& candidate) {
                        return candidate.spelling == owner.spelling &&
                               candidate.path == relative;
                    });
                if (!allowed) {
                    offenders.push_back(relative + " includes " +
                                        std::string{owner.spelling});
                }
            }
        }
    }

    for (const auto& offender : offenders) {
        std::fprintf(stderr, "FAIL: platform header outside its adapter: %s\n",
                     offender.c_str());
    }
    check(offenders.empty(), "platform headers stay in named adapters");
}

void nativeCallsStayInPlatformFamilies() {
    const auto root = repositoryRoot();
    if (root.empty()) return;

    std::vector<std::string> offenders;
    for (const auto& directory : {"src", "include", "apps"}) {
        const auto base = root / directory;
        if (!fs::exists(base)) continue;
        for (fs::recursive_directory_iterator it{base}, end; it != end; ++it) {
            if (!it->is_regular_file()) continue;
            const auto extension = it->path().extension().string();
            if (extension != ".cpp" && extension != ".h" &&
                extension != ".hpp") {
                continue;
            }
            const auto relative =
                fs::relative(it->path(), root).generic_string();
            if (startsWith(relative, "src/platform/linux/") ||
                startsWith(relative, "src/platform/windows/")) {
                continue;
            }
            const auto source = readSource(it->path());
            for (const auto spelling : kNativeSpellings) {
                if (source.find(spelling) != std::string::npos) {
                    offenders.push_back(relative + " uses " +
                                        std::string{spelling});
                }
            }
        }
    }

    for (const auto& offender : offenders) {
        std::fprintf(stderr, "FAIL: native call outside platform family: %s\n",
                     offender.c_str());
    }
    check(offenders.empty(), "native calls stay in platform families");
}

void portableTestsAvoidPosixCalls() {
    const auto root = repositoryRoot();
    if (root.empty()) return;

    std::vector<std::string> offenders;
    for (const auto relative : kPortableTestFiles) {
        const auto source = readSource(root / relative);
        for (const auto spelling : kPosixTestSpellings) {
            if (source.find(spelling) != std::string::npos) {
                offenders.push_back(std::string{relative} + " uses " +
                                    std::string{spelling});
            }
        }
    }

    for (const auto& offender : offenders) {
        std::fprintf(stderr, "FAIL: POSIX call in portable test: %s\n",
                     offender.c_str());
    }
    check(offenders.empty(), "portable tests avoid POSIX calls");
}

}  // namespace

SSG_TEST_SUITE(test_file_seam_guard) {
    libraryAndApplicationCodeUseTheFileSeam();
    platformHeadersStayInTheirAdapters();
    nativeCallsStayInPlatformFamilies();
    portableTestsAvoidPosixCalls();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
