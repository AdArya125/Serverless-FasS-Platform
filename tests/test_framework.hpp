#pragma once
// A deliberately small test framework: a registry of named test functions
// plus two assertion macros. No external dependency, so the same build
// toolchain that builds the platform also runs its tests.

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace faas_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const std::string& name, const std::function<void()>& fn) {
        registry().push_back({name, fn});
    }
};

inline int run_all() {
    int passed = 0;
    int failed = 0;
    for (auto& test : registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << passed << "/" << registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace faas_test

#define TEST_CASE(name)                                                              \
    static void name();                                                             \
    static faas_test::Registrar registrar_##name(#name, name);                      \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            throw std::runtime_error("CHECK failed: " #cond " at " __FILE__ ":" +    \
                                      std::to_string(__LINE__));                     \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        if (!((a) == (b))) {                                                         \
            throw std::runtime_error("CHECK_EQ failed: " #a " != " #b " at " __FILE__ \
                                      ":" + std::to_string(__LINE__));               \
        }                                                                            \
    } while (0)
