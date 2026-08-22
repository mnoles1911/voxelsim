#pragma once
// Minimal zero-dependency test harness ("boring, testable" — plan §7).

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace vxctest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int failures = 0;
inline const char* currentTest = "";

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <typename A, typename B>
void checkEqFail(const char* file, int line, const char* ae, const char* be,
                 const A& a, const B& b) {
    std::ostringstream os;
    os << "  FAIL " << file << ":" << line << " in " << currentTest << ": " << ae
       << " == " << be << " (" << a << " vs " << b << ")";
    std::fprintf(stderr, "%s\n", os.str().c_str());
    ++failures;
}

// `patterns` is a substring filter: a test runs if its name contains any of
// them, or if there are none. Skipped tests are counted and reported, so a
// filter that matches nothing says so instead of printing a clean zero-test
// pass -- which is the way a filter silently turns a suite green.
inline int runAll(int patternCount = 0, char** patterns = nullptr) {
    int ran = 0;
    for (const Case& c : registry()) {
        bool wanted = patternCount == 0;
        for (int i = 0; i < patternCount && !wanted; ++i) {
            wanted = std::string(c.name).find(patterns[i]) != std::string::npos;
        }
        if (!wanted) continue;
        ++ran;
        currentTest = c.name;
        const int before = failures;
        c.fn();
        std::printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", c.name);
    }
    if (patternCount != 0) {
        std::printf("%d of %d test(s) matched the filter\n", ran,
                    static_cast<int>(registry().size()));
        if (ran == 0) {
            std::fprintf(stderr, "no test matched the filter\n");
            return 1;
        }
    }
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}

} // namespace vxctest

#define VXC_TEST(name)                                        \
    static void vxc_test_##name();                            \
    static ::vxctest::Registrar vxc_reg_##name(#name, &vxc_test_##name); \
    static void vxc_test_##name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, \
                         ::vxctest::currentTest, #cond);                        \
            ++::vxctest::failures;                                              \
        }                                                                       \
    } while (0)

// CHECK with a sentence attached. The condition names the mechanism; the
// message names the CONSEQUENCE, which is what a failure three months from now
// needs in order to be actionable rather than merely red.
#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n    %s\n", __FILE__,   \
                         __LINE__, ::vxctest::currentTest, #cond, (msg));        \
            ++::vxctest::failures;                                               \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        const auto va = (a);                                                  \
        const auto vb = (b);                                                  \
        if (!(va == vb))                                                      \
            ::vxctest::checkEqFail(__FILE__, __LINE__, #a, #b, +va, +vb);     \
    } while (0)
