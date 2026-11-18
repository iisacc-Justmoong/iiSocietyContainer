#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace society_ctest {

struct SkippedCase final { std::string reason; };
[[noreturn]] inline void skip(const char* reason) { throw SkippedCase{reason}; }

class CTestRunner final {
public:
    explicit CTestRunner(std::string_view selection = {}) : selection_(selection) {}

    template <typename Function>
    void run(std::string_view name, Function&& function)
    {
        const bool group = name.size() > selection_.size() &&
                           name.substr(0, selection_.size()) == selection_ &&
                           name[selection_.size()] == ':';
        if (!selection_.empty() && selection_ != name && !group) return;
        ++cases_;
        try {
            std::forward<Function>(function)();
        } catch (const SkippedCase& skipped) {
            ++skipped_;
            std::cerr << name << ": skipped: " << skipped.reason << '\n';
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << name << ": unexpected non-standard exception\n";
        }
    }

    [[nodiscard]] int result() const
    {
        if (cases_ == 0) {
            std::cerr << "No test cases were executed\n";
            return 1;
        }
        std::cout << cases_ << " test cases, " << failures_ << " failures\n";
        if (failures_ != 0) return 1;
        return skipped_ == cases_ ? 77 : 0;
    }

private:
    std::string selection_;
    unsigned cases_ = 0;
    unsigned failures_ = 0;
    unsigned skipped_ = 0;
};

class CallbackChecks final {
public:
    template <typename Function> void capture(Function&& function) noexcept
    {
        if (failure_) return;
        try { std::forward<Function>(function)(); }
        catch (...) { failure_ = std::current_exception(); }
    }
    void rethrow() const { if (failure_) std::rethrow_exception(failure_); }
private:
    std::exception_ptr failure_;
};

[[noreturn]] inline void fail(const char* file, int line, const char* message)
{
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

template <typename Actual, typename Expected>
inline void compare(Actual&& actual, Expected&& expected,
                    const char* file, int line, const char* message)
{
    if (!(actual == expected))
        fail(file, line, message);
}

} // namespace society_ctest

#define CTEST_CHECK(expression) \
    do { if (!(expression)) ::society_ctest::fail(__FILE__, __LINE__, #expression); } while (false)

#define CTEST_CHECK_MESSAGE(expression, message) \
    do { \
        if (!(expression)) \
            ::society_ctest::fail(__FILE__, __LINE__, (message)); \
    } while (false)

#define CTEST_COMPARE(actual, expected) \
    ::society_ctest::compare((actual), (expected), __FILE__, __LINE__, #actual " == " #expected)

#define CTEST_THROWS(exception_type, expression) \
    do { \
        bool ctest_thrown = false; \
        try { (void)(expression); } \
        catch (const exception_type&) { ctest_thrown = true; } \
        if (!ctest_thrown) \
            ::society_ctest::fail(__FILE__, __LINE__, "expected " #exception_type " from " #expression); \
    } while (false)
