#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <iomanip>

namespace ulv_test {

class AssertionException : public std::runtime_error {
public:
    AssertionException(const std::string& msg, const char* file, int line)
        : std::runtime_error(msg), file_(file), line_(line) {}

    const char* file() const { return file_; }
    int line() const { return line_; }

private:
    const char* file_;
    int line_;
};

struct TestCase {
    std::string name;
    std::string tier;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void register_test(const std::string& tier, const std::string& name, std::function<void()> func) {
        tests_.push_back({name, tier, std::move(func)});
    }

    const std::vector<TestCase>& tests() const { return tests_; }

private:
    std::vector<TestCase> tests_;
};

struct TestRegistrar {
    TestRegistrar(const std::string& tier, const std::string& name, std::function<void()> func) {
        TestRegistry::instance().register_test(tier, name, std::move(func));
    }
};

#define ULV_CONCAT_INTERNAL(a, b) a##b
#define ULV_CONCAT(a, b) ULV_CONCAT_INTERNAL(a, b)

#define TEST_CASE_TIER(tier, name) \
    static void ULV_CONCAT(test_func_, __LINE__)(); \
    static ::ulv_test::TestRegistrar ULV_CONCAT(test_reg_, __LINE__)(tier, name, ULV_CONCAT(test_func_, __LINE__)); \
    static void ULV_CONCAT(test_func_, __LINE__)()

#define TEST_CASE(name) TEST_CASE_TIER("General", name)

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream oss; \
            oss << "ASSERT_TRUE(" #cond ") failed"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            std::ostringstream oss; \
            oss << "ASSERT_FALSE(" #cond ") failed"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_EQ(actual, expected) \
    do { \
        auto act_val = (actual); \
        auto exp_val = (expected); \
        if (!(act_val == exp_val)) { \
            std::ostringstream oss; \
            oss << "ASSERT_EQ(" #actual ", " #expected ") failed: actual [" \
                << act_val << "] != expected [" << exp_val << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_NE(actual, expected) \
    do { \
        auto act_val = (actual); \
        auto exp_val = (expected); \
        if (act_val == exp_val) { \
            std::ostringstream oss; \
            oss << "ASSERT_NE(" #actual ", " #expected ") failed: values are both [" \
                << act_val << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GT(val, bound) \
    do { \
        if (!((val) > (bound))) { \
            std::ostringstream oss; \
            oss << "ASSERT_GT(" #val ", " #bound ") failed: [" << (val) << "] <= [" << (bound) << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GE(val, bound) \
    do { \
        if (!((val) >= (bound))) { \
            std::ostringstream oss; \
            oss << "ASSERT_GE(" #val ", " #bound ") failed: [" << (val) << "] < [" << (bound) << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LT(val, bound) \
    do { \
        if (!((val) < (bound))) { \
            std::ostringstream oss; \
            oss << "ASSERT_LT(" #val ", " #bound ") failed: [" << (val) << "] >= [" << (bound) << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LE(val, bound) \
    do { \
        if (!((val) <= (bound))) { \
            std::ostringstream oss; \
            oss << "ASSERT_LE(" #val ", " #bound ") failed: [" << (val) << "] > [" << (bound) << "]"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_CONTAINS(haystack_str, needle_str) \
    do { \
        std::string s_haystack = (haystack_str); \
        std::string s_needle = (needle_str); \
        if (s_haystack.find(s_needle) == std::string::npos) { \
            std::ostringstream oss; \
            oss << "ASSERT_CONTAINS failed: '" << s_needle << "' not found in string (len=" << s_haystack.length() << ")"; \
            throw ::ulv_test::AssertionException(oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

inline int run_all_tests() {
    const auto& tests = TestRegistry::instance().tests();
    int passed = 0;
    int failed = 0;

    std::cout << "======================================================================\n";
    std::cout << "  ULV Native C++ Test Suite (Running " << tests.size() << " registered tests)\n";
    std::cout << "======================================================================\n";

    std::string current_tier = "";
    auto suite_start = std::chrono::high_resolution_clock::now();

    for (const auto& test : tests) {
        if (test.tier != current_tier) {
            current_tier = test.tier;
            std::cout << "\n--- [" << current_tier << "] ---\n";
        }

        std::cout << "  [TEST] " << std::left << std::setw(50) << test.name << " ... " << std::flush;
        auto t0 = std::chrono::high_resolution_clock::now();
        try {
            test.func();
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cout << "\033[32mPASS\033[0m (" << std::fixed << std::setprecision(2) << ms << " ms)\n";
            passed++;
        } catch (const AssertionException& e) {
            std::cout << "\033[31mFAIL\033[0m\n";
            std::cout << "         " << e.file() << ":" << e.line() << " -> " << e.what() << "\n";
            failed++;
        } catch (const std::exception& e) {
            std::cout << "\033[31mERROR\033[0m\n";
            std::cout << "         Unexpected std::exception: " << e.what() << "\n";
            failed++;
        } catch (...) {
            std::cout << "\033[31mCRASH\033[0m\n";
            std::cout << "         Unknown exception caught\n";
            failed++;
        }
    }

    auto suite_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(suite_end - suite_start).count();

    std::cout << "\n======================================================================\n";
    std::cout << "  Test Summary: " << passed << " passed, " << failed << " failed, "
              << tests.size() << " total (" << std::fixed << std::setprecision(1) << total_ms << " ms)\n";
    std::cout << "======================================================================\n";

    return (failed == 0) ? 0 : 1;
}

} // namespace ulv_test
