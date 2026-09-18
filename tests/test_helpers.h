#pragma once

#include <ssg/Editor.h>

// Minimal standalone test helpers for SSG test executables.
//
// Each test file is an independent executable with its own main(); these
// helpers are the only shared test infrastructure (no framework dependency).
//
// Assertion macros work with any type that satisfies the relevant operator
// (==, !=, etc.).  They intentionally do NOT stream operand values to avoid
// requiring operator<< on domain types such as scoped enums and strong-id
// types.  Failure messages include the expression text and file/line, which
// combined with descriptive test names is sufficient to locate failures.
//
// Usage:
//   TEST(my_test) { ASSERT_EQ(a, b); }
//   int main() { RUN(my_test); ... }

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <random>
#include <source_location>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

class TestRuntimeDirectory {
public:
    explicit TestRuntimeDirectory(std::filesystem::path path)
        : path_{std::move(path)} {
        std::filesystem::create_directories(path_);
    }

    ~TestRuntimeDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

inline std::filesystem::path testRuntimePath(
    std::filesystem::path const& name,
    std::source_location location = std::source_location::current()) {
    if (name.empty() || name.is_absolute() || name.has_parent_path()) {
        throw std::runtime_error{"test runtime name must be one relative component"};
    }
    auto source = std::filesystem::absolute(location.file_name()).parent_path();
    while (!source.empty()) {
        if (source.filename() == "tests" &&
            std::filesystem::exists(source.parent_path() / "CMakeLists.txt")) {
            static const auto processDirectory =
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()) +
                "_" + std::to_string(std::random_device{}());
            static const TestRuntimeDirectory runtime{
                source.parent_path() / ".test-runtime" / processDirectory};
            return runtime.path() / name;
        }
        auto parent = source.parent_path();
        if (parent == source) break;
        source = std::move(parent);
    }
    throw std::runtime_error{"test source is outside the repository test tree"};
}

inline int runGitStatus(const std::filesystem::path& root,
                        std::string_view command) {
#ifdef _WIN32
    constexpr std::string_view nullDevice = "NUL";
#else
    constexpr std::string_view nullDevice = "/dev/null";
#endif
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " >" + std::string{nullDevice} + " 2>&1";
    return std::system(full.c_str());
}

inline int passed = 0;
inline int failed = 0;

namespace ssg::test {

inline CommandResult requireCommand(ClientInputResult result) {
    if (!result.command) {
        throw std::runtime_error{"client input produced no command result"};
    }
    return std::move(*result.command);
}

inline PromptEditState promptText(std::string text) {
    return PromptEditState{std::move(text)};
}

inline CommandResult dispatchInput(Editor& editor, ClientInput input) {
    return requireCommand(editor.input(std::move(input)));
}

inline CommandResult openFile(Editor& editor, std::string_view path) {
    auto result = applyFilePathCompletion(editor, PromptCompletion::FileOpen, path);
    return {result.accepted ? CommandError::None : CommandError::HandlerFailed,
            std::move(result.message), std::move(result.viewAction)};
}

inline CommandResult typeText(Editor& editor, std::string text) {
    std::lock_guard operationLock{editor.operationMutex};
    auto const active = editor.activeDocumentId();
    auto const revisionBefore =
        active && editor.activeDocument()
            ? std::optional<std::uint64_t>{editor.activeDocument()->revision()}
            : std::nullopt;
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto result = applyEditorTextInput(
        editor, TextInputCommand::Insert,
        TextInputArguments{std::move(text)});
    editor.reconcileFindDocument();
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    if (result.accepted && active && revisionBefore &&
        editor.activeDocumentId() == active && editor.activeDocument() != nullptr &&
        editor.activeDocument()->revision() != *revisionBefore) {
        (void)editor.follow.notifyLocalEdit();
    }
    return {result.accepted ? CommandError::None : CommandError::HandlerFailed,
            std::move(result.message), std::move(result.viewAction)};
}

inline CommandResult setSelections(
    Editor& editor,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    auto const* tab = editor.activeTabState();
    auto const* document = editor.activeDocument();
    if (tab == nullptr || document == nullptr) {
        throw std::runtime_error{
            "selection transition requires an active document"};
    }
    auto revision = document->revision();
    if (tab->kind == TabKind::LiveDiff) {
        revision = editor.diff.viewState().revision;
    }
    std::vector<SelectionRangeTransition> selections;
    selections.reserve(ranges.size());
    for (auto const& [anchor, active] : ranges) {
        selections.push_back({ByteOffset{anchor}, ByteOffset{active}});
    }
    return dispatchInput(editor, ViewTransitionInput{
                                   SelectionTransition{
                                       tab->id, revision,
                                       std::move(selections)}});
}

inline CommandResult clickDocument(Editor& editor, std::uint64_t byteOffset,
                                   bool additive = false,
                                   bool selectWord = false) {
    auto result = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{byteOffset}, additive, selectWord});
    if (editor.documentPointerGesture.has_value()) {
        auto release = dispatchInput(
            editor,
            DocumentPointerInput{
                ByteOffset{byteOffset}, false, false,
                InputPointerButton::Primary, InputPointerPhase::Release});
        if (!release.accepted()) return release;
    }
    return result;
}

inline CommandResult dragDocument(Editor& editor, std::uint64_t anchor,
                                  std::uint64_t active,
                                  bool additive = false) {
    auto press = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{anchor}, additive});
    if (!press.accepted()) return press;
    auto move = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{active}, false, false,
            InputPointerButton::Primary, InputPointerPhase::Move});
    if (!move.accepted()) return move;
    auto release = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{active}, false, false,
            InputPointerButton::Primary, InputPointerPhase::Release});
    if (!release.accepted()) return release;
    return move;
}

}  // namespace ssg::test

#define TEST(name) static void name()

#define SSG_TEST_SUITE(name) int ssg_test_entry_##name()
#define SSG_TEST_SUITE_ARGS(name) int ssg_test_entry_##name(int argc, char** argv)

#define RUN(name)                                             \
    do {                                                      \
        std::cout << "  " << #name << std::endl;              \
        name();                                               \
    } while (0)

// Equality: requires operator== on the two operands.
#define ASSERT_EQ(a, b)                                                 \
    do {                                                                 \
        if (!((a) == (b))) {                                            \
            std::cerr << "  FAIL: " << #a << " == " << #b              \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            ++failed;                                                   \
        } else {                                                        \
            ++passed;                                                   \
        }                                                               \
    } while (0)

// Inequality: requires operator== (derives != from it).
#define ASSERT_NE(a, b)                                                       \
    do {                                                                       \
        if ((a) == (b)) {                                                     \
            std::cerr << "  FAIL: " << #a << " != " << #b                    \
                      << " (equal) at " << __FILE__ << ":" << __LINE__ << "\n"; \
            ++failed;                                                         \
        } else {                                                              \
            ++passed;                                                         \
        }                                                                     \
    } while (0)

// Boolean truth.
#define ASSERT_TRUE(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::cerr << "  FAIL: expected true: " << #expr                   \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n";       \
            ++failed;                                                          \
        } else {                                                               \
            ++passed;                                                          \
        }                                                                      \
    } while (0)

// Boolean falsity.
#define ASSERT_FALSE(expr)                                                     \
    do {                                                                       \
        if (expr) {                                                            \
            std::cerr << "  FAIL: expected false: " << #expr                  \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n";       \
            ++failed;                                                          \
        } else {                                                               \
            ++passed;                                                          \
        }                                                                      \
    } while (0)

// Asserts that evaluating expr throws an exception of exc_type.
// expr must be an expression (not a declaration).
#define ASSERT_THROWS(expr, exc_type)                                          \
    do {                                                                       \
        bool _threw = false;                                                   \
        try { (void)(expr); }                                                  \
        catch (exc_type const&) { _threw = true; }                            \
        catch (...) {}                                                         \
        if (!_threw) {                                                         \
            std::cerr << "  FAIL: " << #expr << " did not throw "             \
                      << #exc_type << " at " << __FILE__ << ":" << __LINE__   \
                      << "\n";                                                 \
            ++failed;                                                          \
        } else {                                                               \
            ++passed;                                                          \
        }                                                                      \
    } while (0)

// Asserts that evaluating expr does not throw.
// expr must be an expression (not a declaration).
#define ASSERT_NO_THROW(expr)                                                  \
    do {                                                                       \
        bool _threw = false;                                                   \
        try { (void)(expr); }                                                  \
        catch (...) { _threw = true; }                                        \
        if (_threw) {                                                          \
            std::cerr << "  FAIL: " << #expr << " threw unexpectedly at "     \
                      << __FILE__ << ":" << __LINE__ << "\n";                 \
            ++failed;                                                          \
        } else {                                                               \
            ++passed;                                                          \
        }                                                                      \
    } while (0)
