#include "harness.h"

#include <cstdio>
#include <vector>

namespace bybus_test {
namespace {

int g_failures = 0;

// Function-local static so registration order cannot depend on other
// translation units' dynamic initialisation.
std::vector<Case> &cases() {
    static std::vector<Case> instance;
    return instance;
}

} // namespace

void check(bool condition, const char *what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++g_failures;
}

void add_case(const char *name, void (*function)()) {
    if (!name || !function) return;
    cases().push_back(Case{name, function});
}

int run_all() {
    for (const Case &item : cases()) item.function();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all host checks passed\n");
    return 0;
}

} // namespace bybus_test
