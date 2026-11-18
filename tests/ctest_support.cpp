#include "CTestSupport.h"

#include <sstream>
#include <stdexcept>
#include <string>

int main()
{
    std::ostringstream errors;
    std::ostringstream output;
    auto* previous = std::cerr.rdbuf(errors.rdbuf());
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    society_ctest::CTestRunner empty;
    const int emptyResult = empty.result();
    society_ctest::CTestRunner missing("absent");
    missing.run("other", [] { CTEST_CHECK(false); });
    const int missingResult = missing.result();
    society_ctest::CTestRunner failure;
    bool continued = false;
    failure.run("bad-row", [] { CTEST_CHECK(false); });
    failure.run("next-row", [&] { continued = true; });
    const int failureResult = failure.result();
    society_ctest::CTestRunner allSkipped;
    allSkipped.run("not-supported", [] { society_ctest::skip("fixture"); });
    const int allSkippedResult = allSkipped.result();
    society_ctest::CTestRunner mixed;
    mixed.run("not-supported", [] { society_ctest::skip("fixture"); });
    mixed.run("supported", [] {});
    const int mixedResult = mixed.result();
    std::cerr.rdbuf(previous);
    std::cout.rdbuf(previousOutput);
    if (emptyResult == 0 || missingResult == 0 || failureResult == 0 || allSkippedResult != 77 || mixedResult != 0 || !continued ||
        errors.str().find("No test cases") == std::string::npos ||
        errors.str().find("bad-row") == std::string::npos ||
        errors.str().find("ctest_support.cpp:") == std::string::npos)
        return 1;

    society_ctest::CTestRunner success;
    bool selected = false;
    bool unexpected = false;
    society_ctest::CTestRunner filtered("selected");
    filtered.run("other", [&] { unexpected = true; });
    filtered.run("selected", [&] { selected = true; });
    if (!selected || unexpected || filtered.result() != 0)
        return 1;
    int groupRows = 0;
    society_ctest::CTestRunner group("batch");
    group.run("batch:first", [&] { ++groupRows; });
    group.run("batch:second", [&] { ++groupRows; });
    group.run("batch-other:first", [&] { unexpected = true; });
    if (group.result() != 0 || groupRows != 2 || unexpected) return 1;
    int exactRows = 0;
    society_ctest::CTestRunner exact("batch:second");
    exact.run("batch:first", [&] { ++exactRows; });
    exact.run("batch:second", [&] { ++exactRows; });
    if (exact.result() != 0 || exactRows != 1) return 1;
    success.run("checks", [] {
        int evaluations = 0;
        CTEST_CHECK(true);
        CTEST_COMPARE(++evaluations, 1);
        CTEST_COMPARE(evaluations, 1);
        CTEST_THROWS(std::runtime_error, throw std::runtime_error("expected"));
        society_ctest::CallbackChecks callback;
        bool continuedCallback = false;
        callback.capture([] { CTEST_CHECK(false); });
        callback.capture([&] { continuedCallback = true; });
        CTEST_CHECK(!continuedCallback);
        CTEST_THROWS(std::runtime_error, callback.rethrow());
    });
    return success.result();
}
