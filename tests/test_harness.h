// SPDX-License-Identifier: GPL-2.0-or-later
//
// A deliberately tiny test harness: no dependencies, so the portable half of
// the driver can be tested anywhere a C++17 compiler exists.

#ifndef QCAM_TEST_HARNESS_H_
#define QCAM_TEST_HARNESS_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace qcam_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline const char*& CurrentTest() {
    static const char* name = "";
    return name;
}

// CHECK_EQ prints both sides, so it needs to stringify whatever it was
// handed: numbers, bools, strings, or anything else.
inline std::string ToStr(const std::string& v) { return v; }
inline std::string ToStr(const char* v) { return v ? v : "(null)"; }
inline std::string ToStr(bool v) { return v ? "true" : "false"; }

template <typename T>
inline std::string ToStr(const T& v) {
    if constexpr (std::is_enum_v<T>)
        return std::to_string(static_cast<long long>(v));
    else if constexpr (std::is_arithmetic_v<T>)
        return std::to_string(v);
    else
        return "<value>";
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void ReportFailure(const char* file, int line, const std::string& what) {
    FailureCount()++;
    std::fprintf(stderr, "  FAIL %s\n    at %s:%d\n    %s\n", CurrentTest(), file,
                 line, what.c_str());
}

inline int RunAll(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& test : Registry()) {
        if (filter && std::string(test.name).find(filter) == std::string::npos)
            continue;
        CurrentTest() = test.name;
        const int before = FailureCount();
        test.fn();
        ran++;
        if (FailureCount() == before)
            std::printf("  ok   %s\n", test.name);
    }
    std::printf("\n%d test(s) run, %d failure(s)\n", ran, FailureCount());
    return FailureCount() == 0 ? 0 : 1;
}

}  // namespace qcam_test

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::qcam_test::Registrar registrar_##name(#name, &name);        \
    static void name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond))                                                     \
            ::qcam_test::ReportFailure(__FILE__, __LINE__,               \
                                       "CHECK failed: " #cond);          \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        const auto qcam_a_ = (a);                                        \
        const auto qcam_b_ = (b);                                        \
        if (!(qcam_a_ == qcam_b_)) {                                     \
            ::qcam_test::ReportFailure(                                  \
                __FILE__, __LINE__,                                      \
                std::string("CHECK_EQ failed: " #a " == " #b "\n      ") \
                    + "lhs = " + ::qcam_test::ToStr(qcam_a_)             \
                    + ", rhs = " + ::qcam_test::ToStr(qcam_b_));         \
        }                                                                \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                            \
    do {                                                                 \
        const double qcam_a_ = static_cast<double>(a);                   \
        const double qcam_b_ = static_cast<double>(b);                   \
        const double qcam_d_ = qcam_a_ - qcam_b_;                        \
        if (!((qcam_d_ < 0 ? -qcam_d_ : qcam_d_) <= (tol))) {            \
            ::qcam_test::ReportFailure(                                  \
                __FILE__, __LINE__,                                      \
                std::string("CHECK_NEAR failed: " #a " ~= " #b "\n      ")\
                    + "lhs = " + std::to_string(qcam_a_)                 \
                    + ", rhs = " + std::to_string(qcam_b_));             \
        }                                                                \
    } while (0)

#define CHECK_OK(expr)                                                   \
    do {                                                                 \
        const ::qcam::Status qcam_st_ = (expr);                          \
        if (::qcam::Failed(qcam_st_))                                    \
            ::qcam_test::ReportFailure(                                  \
                __FILE__, __LINE__,                                      \
                std::string("expected Ok from " #expr ", got ")          \
                    + ::qcam::StatusName(qcam_st_));                     \
    } while (0)

#define CHECK_STATUS(expr, want)                                         \
    do {                                                                 \
        const ::qcam::Status qcam_st_ = (expr);                          \
        if (qcam_st_ != (want))                                          \
            ::qcam_test::ReportFailure(                                  \
                __FILE__, __LINE__,                                      \
                std::string("expected " #want " from " #expr ", got ")   \
                    + ::qcam::StatusName(qcam_st_));                     \
    } while (0)

#endif  // QCAM_TEST_HARNESS_H_
