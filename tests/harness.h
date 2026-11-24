// Minimal host-test harness shared by every tests/*.cpp file.
//
// Each translation unit registers its cases at static-initialisation time:
//
//     BYBUS_TEST(pairing_blank_translation_is_rejected) {
//         check(!merged.changed, "blank translation must not edit the table");
//     }
//
// `build/test-host.ps1` compiles every tests/*.cpp into one binary; the single
// main() lives in tests/test_main.cpp and calls run_all().
#pragma once

namespace bybus_test {

struct Case {
    const char *name;
    void (*function)();
};

// Records a failed expectation. Never throws: the host target is /W4 /WX and
// must keep running after a failure so every case still reports.
void check(bool condition, const char *what);

void add_case(const char *name, void (*function)());

int run_all();

struct Registrar {
    Registrar(const char *name, void (*function)()) { add_case(name, function); }
};

} // namespace bybus_test

using bybus_test::check;

#define BYBUS_TEST(function_name)                                              \
    static void function_name();                                               \
    static ::bybus_test::Registrar function_name##_registration(               \
        #function_name, &function_name);                                       \
    static void function_name()
