#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

class Failure : public std::runtime_error {
public:
    Failure(const char* file, int line, const std::string& message)
        : std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": " + message) {}
};

struct Case {
    std::string name;
    std::function<void()> function;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> function) {
        registry().push_back({std::move(name), std::move(function)});
    }
};

inline int run_all() {
    int failed = 0;
    for (const Case& test : registry()) {
        try {
            test.function();
            std::cout << "ok   " << test.name << "\n";
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << "\n     " << error.what()
                      << "\n";
            ++failed;
        }
    }
    std::cout << registry().size() - static_cast<std::size_t>(failed) << "/"
              << registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace test

#define TEST(name)                                                  \
    static void name();                                             \
    static test::Registrar name##_registrar(#name, name);           \
    static void name()

#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            throw test::Failure(__FILE__, __LINE__,                 \
                                "CHECK failed: " #condition);       \
        }                                                           \
    } while (false)

#define CHECK_EQ(a, b)                                              \
    do {                                                            \
        if (!((a) == (b))) {                                        \
            throw test::Failure(__FILE__, __LINE__,                 \
                                "CHECK_EQ failed: " #a " == " #b);  \
        }                                                           \
    } while (false)

#define CHECK_THROWS(expression)                                    \
    do {                                                            \
        bool threw = false;                                         \
        try {                                                       \
            (void)(expression);                                     \
        } catch (...) {                                             \
            threw = true;                                           \
        }                                                           \
        if (!threw) {                                               \
            throw test::Failure(__FILE__, __LINE__,                 \
                                "CHECK_THROWS failed: " #expression); \
        }                                                           \
    } while (false)
