#pragma once
// Minimal dependency-free test harness: DDRTEST registers a test function via
// a static initializer; test_main.cpp runs everything in the registry.
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ddrtest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

} // namespace ddrtest

struct TestFailure : std::exception {
    std::string msg;
    explicit TestFailure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

#define DDRTEST(name)                                                                 \
    static void ddrtest_fn_##name();                                                  \
    static ddrtest::Registrar ddrtest_reg_##name(#name, ddrtest_fn_##name);            \
    static void ddrtest_fn_##name()

#define DDR_CHECK(cond)                                                               \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::ostringstream _os;                                                   \
            _os << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond;           \
            throw TestFailure(_os.str());                                             \
        }                                                                             \
    } while (0)

#define DDR_CHECK_EQ(a, b)                                                            \
    do {                                                                              \
        auto _a = (a);                                                                \
        auto _b = (b);                                                                \
        if (!(_a == _b)) {                                                            \
            std::ostringstream _os;                                                   \
            _os << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " (" << _a  \
                << ") != " #b " (" << _b << ")";                                      \
            throw TestFailure(_os.str());                                             \
        }                                                                             \
    } while (0)
