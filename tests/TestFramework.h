#pragma once
// Tiny test framework: TEST(name) { CHECK(cond); }
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
}

#define TEST(name) \
    static void test_##name(); \
    static testing::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { if (!(cond)) { \
    std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++testing::failures(); } } while (0)

#define CHECK_MSG(cond, ...) do { if (!(cond)) { \
    std::printf("    FAILED %s:%d: %s -- ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); \
    std::printf("\n"); ++testing::failures(); } } while (0)
