#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <source_location>
#include <string>
#include <stdexcept>
#include <utility>

inline std::string testRuntimeToken(std::random_device::result_type value) {
    static constexpr std::string_view digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string token;
    do {
        token.push_back(digits[value % digits.size()]);
        value /= digits.size();
    } while (value != 0);
    std::ranges::reverse(token);
    return token;
}

class TestRuntimeDirectory {
public:
    explicit TestRuntimeDirectory(std::filesystem::path path)
        : path_{std::move(path)} {
        std::filesystem::create_directories(path_);
    }

    TestRuntimeDirectory() {
        std::random_device random;
        for (;;) {
            auto candidate = std::filesystem::temp_directory_path() /
                             ("s" + testRuntimeToken(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::filesystem::filesystem_error{
                    "create test runtime directory", candidate, error};
            }
        }
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

inline std::filesystem::path testSystemRuntimePath(
    std::filesystem::path const& name) {
    if (name.empty() || name.is_absolute() || name.has_parent_path()) {
        throw std::runtime_error{"test runtime name must be one relative component"};
    }
    static const TestRuntimeDirectory runtime;
    return runtime.path() / name;
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
inline int skipped = 0;

#define TEST(name) static void name()

#define SSG_TEST_SUITE(name) int ssg_test_entry_##name()
#define SSG_TEST_SUITE_ARGS(name) int ssg_test_entry_##name(int argc, char** argv)

#define RUN(name)                                             \
    do {                                                      \
        std::cout << "  " << #name << std::endl;              \
        name();                                               \
    } while (0)

#define SKIP(reason)                                                   \
    do {                                                               \
        std::cout << "  SKIP: " << reason << " at " << __FILE__ << ":" \
                  << __LINE__ << std::endl;                             \
        ++skipped;                                                     \
        return;                                                        \
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
