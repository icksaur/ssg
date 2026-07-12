#pragma once

#include <iostream>

inline int passed = 0;
inline int failed = 0;

#define TEST(name) static void name()

#define RUN(name) do { \
    std::cout << "  " << #name << std::endl; \
    name(); \
} while (0)

#define ASSERT_EQ(a, b) do { \
    const auto& actual = (a); \
    const auto& expected = (b); \
    if (actual != expected) { \
        std::cerr << "  FAIL: " << #a << " == " << #b \
                  << " (got '" << actual << "' vs '" << expected << "') at " \
                  << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { \
        passed++; \
    } \
} while (0)
