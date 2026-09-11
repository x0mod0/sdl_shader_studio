#include "test.h"

#include <exception>
#include <iostream>

namespace test {
namespace {
struct Failure : std::exception {};
int g_failures = 0;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& message) {
    std::cout << "    " << file << ":" << line << ": " << message << "\n";
    ++g_failures;
    throw Failure{};
}

int run_all() {
    int failed = 0;
    for (auto& c : registry()) {
        const int before = g_failures;
        std::cout << "[ run  ] " << c.name << "\n";
        try {
            c.fn();
        } catch (const Failure&) {
            // already reported
        } catch (const std::exception& e) {
            std::cout << "    unexpected exception: " << e.what() << "\n";
            ++g_failures;
        }
        if (g_failures > before) {
            std::cout << "[ FAIL ] " << c.name << "\n";
            ++failed;
        } else {
            std::cout << "[  ok  ] " << c.name << "\n";
        }
    }
    std::cout << "\n" << registry().size() - failed << "/" << registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace test

int main() { return test::run_all(); }
