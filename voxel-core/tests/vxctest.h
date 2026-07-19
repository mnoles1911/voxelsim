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

inline int runAll() {
    for (const Case& c : registry()) {
        currentTest = c.name;
        const int before = failures;
        c.fn();
        std::printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", c.name);
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

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        const auto va = (a);                                                  \
        const auto vb = (b);                                                  \
        if (!(va == vb))                                                      \
            ::vxctest::checkEqFail(__FILE__, __LINE__, #a, #b, +va, +vb);     \
    } while (0)
