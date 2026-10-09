// Minimal test framework: TEST(name) { CHECK(...); REQUIRE(...); }
// Run all tests with `engine_tests`, or only those whose name contains a filter: `engine_tests scene`.
#pragma once

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/vecmath.h"

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& what);   // records a failure in the current test
struct Abort {};                                                    // thrown by REQUIRE to end the test

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline Vec3 v3(float x, float y, float z) { return {x, y, z}; }   // braces would split macro arguments

// A fresh directory per test run, removed afterwards
std::filesystem::path tempPath(const std::string& file);
void writeFile(const std::filesystem::path& path, const std::string& text);

// A Dear ImGui context without a renderer, so input actions can be driven from tests
struct ImGuiContextGuard {
    ImGuiContextGuard();
    ~ImGuiContextGuard();
};

}  // namespace test

#define TEST(name)                                      \
    static void name();                                 \
    static test::Register test_reg_##name(#name, name); \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define REQUIRE(cond)                                                   \
    do {                                                                \
        if (!(cond)) {                                                  \
            test::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");       \
            throw test::Abort{};                                        \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        double test_a_ = (a), test_b_ = (b);                                                               \
        if (!(std::fabs(test_a_ - test_b_) <= (eps)))                                                      \
            test::fail(__FILE__, __LINE__,                                                                 \
                       "CHECK_NEAR(" #a ", " #b ") " + std::to_string(test_a_) + " vs " + std::to_string(test_b_)); \
    } while (0)

#define CHECK_VEC(a, b, eps)                                                                                  \
    do {                                                                                                      \
        Vec3 test_a_ = (a), test_b_ = (b);                                                                    \
        if (!(length(test_a_ - test_b_) <= (eps)))                                                            \
            test::fail(__FILE__, __LINE__,                                                                    \
                       "CHECK_VEC(" #a ", " #b ") (" + std::to_string(test_a_.x) + " " + std::to_string(test_a_.y) + \
                           " " + std::to_string(test_a_.z) + ") vs (" + std::to_string(test_b_.x) + " " +    \
                           std::to_string(test_b_.y) + " " + std::to_string(test_b_.z) + ")");               \
    } while (0)
