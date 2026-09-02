#pragma once

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

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <utility>

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
            auto root = source.parent_path() / ".test-runtime";
            std::filesystem::create_directories(root);
            return root / name;
        }
        auto parent = source.parent_path();
        if (parent == source) break;
        source = std::move(parent);
    }
    throw std::runtime_error{"test source is outside the repository test tree"};
}

inline int passed = 0;
inline int failed = 0;

#define TEST(name) static void name()

#define RUN(name)                                             \
    do {                                                      \
        std::cout << "  " << #name << "\n";                  \
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
