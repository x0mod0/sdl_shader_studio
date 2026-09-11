// Tiny test harness: no dependency, readable failures, one binary.
#ifndef SSSTUDIO_TEST_H
#define SSSTUDIO_TEST_H

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
int run_all();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

void fail(const char* file, int line, const std::string& message);

}  // namespace test

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::test::Registrar registrar_##name(#name, name);               \
    static void name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        auto va_ = (a);                                                   \
        auto vb_ = (b);                                                   \
        if (!(va_ == vb_)) {                                              \
            ::test::fail(__FILE__, __LINE__,                              \
                         std::string(#a " == " #b " -> ") +               \
                             std::to_string(va_) + " vs " +               \
                             std::to_string(vb_));                        \
        }                                                                 \
    } while (0)

#define CHECK_STREQ(a, b)                                                 \
    do {                                                                  \
        std::string va_ = (a);                                            \
        std::string vb_ = (b);                                            \
        if (va_ != vb_) {                                                 \
            ::test::fail(__FILE__, __LINE__,                              \
                         std::string(#a " == " #b " -> '") + va_ +        \
                             "' vs '" + vb_ + "'");                       \
        }                                                                 \
    } while (0)

#endif  // SSSTUDIO_TEST_H
