#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// A custom GTest event listener that acts as a "Suite-Level" Terse Printer.
// It groups output by TestSuite (which typically corresponds to a single test file)
// and suppresses the thousands of individual test case [ RUN ] / [ OK ] lines.
class TerseSuitePrinter : public ::testing::TestEventListener {
private:
    std::stringstream captured_stdout_;
    std::stringstream captured_stderr_;
    std::streambuf* old_stdout_buf_{nullptr};
    std::streambuf* old_stderr_buf_{nullptr};
    int suite_failed_tests_{0};
    std::vector<std::string> current_test_failures_;

public:
    void OnTestProgramStart(const ::testing::UnitTest& unit_test) override {
        std::cout << "\033[0;32m[==========]\033[0m Running " << unit_test.test_to_run_count()
                  << " tests from " << unit_test.test_suite_to_run_count()
                  << " test suites.\n";
        std::cout << "\033[0;32m[----------]\033[0m Global test environment set-up.\n";
    }

    void OnTestIterationStart(const ::testing::UnitTest&, int) override {}
    void OnEnvironmentsSetUpStart(const ::testing::UnitTest&) override {}
    void OnEnvironmentsSetUpEnd(const ::testing::UnitTest&) override {}

    void OnTestSuiteStart(const ::testing::TestSuite& test_suite) override {
        std::cout << "\033[0;32m[----------]\033[0m " << test_suite.test_to_run_count() << " tests from " << test_suite.name() << "\n";
        std::cout.flush();
        suite_failed_tests_ = 0;
    }

    void OnTestStart(const ::testing::TestInfo&) override {
        // Clear captures and redirect
        captured_stdout_.str(""); captured_stdout_.clear();
        captured_stderr_.str(""); captured_stderr_.clear();
        current_test_failures_.clear();
        
        old_stdout_buf_ = std::cout.rdbuf(captured_stdout_.rdbuf());
        old_stderr_buf_ = std::cerr.rdbuf(captured_stderr_.rdbuf());
    }

    void OnTestPartResult(const ::testing::TestPartResult& result) override {
        if (result.failed()) {
            std::stringstream ss;
            ss << result.file_name() << ":" << result.line_number() << "\n"
               << result.message() << "\n";
            current_test_failures_.push_back(ss.str());
        }
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override {
        // Restore std::cout and std::cerr
        std::cout.rdbuf(old_stdout_buf_);
        std::cerr.rdbuf(old_stderr_buf_);

        if (test_info.result()->Failed()) {
            suite_failed_tests_++;
            std::cout << "\033[1;31m[  FAILED  ]\033[0m " << test_info.test_suite_name() << "." << test_info.name() << "\n";
            
            for (const auto& msg : current_test_failures_) {
                std::cout << msg;
            }
            
            std::string out = captured_stdout_.str();
            std::string err = captured_stderr_.str();
            if (!out.empty()) std::cout << "\033[1;33m--- Captured STDOUT ---\033[0m\n" << out << "-----------------------\n";
            if (!err.empty()) std::cerr << "\033[1;33m--- Captured STDERR ---\033[0m\n" << err << "-----------------------\n";
        }
    }

    void OnTestSuiteEnd(const ::testing::TestSuite& test_suite) override {
        if (suite_failed_tests_ == 0) {
            std::cout << "\033[0;32m[       OK ]\033[0m " << test_suite.name() 
                      << " (" << test_suite.elapsed_time() << " ms)\n";
        } else {
            std::cout << "\033[0;31m[  FAILED  ]\033[0m " << test_suite.name() 
                      << " (" << suite_failed_tests_ << " failures, " << test_suite.elapsed_time() << " ms)\n";
        }
        std::cout << "\033[0;32m[----------]\033[0m " << test_suite.test_to_run_count() << " tests from " << test_suite.name() << " (" << test_suite.elapsed_time() << " ms total)\n\n";
    }

    void OnEnvironmentsTearDownStart(const ::testing::UnitTest&) override {
        std::cout << "\033[0;32m[----------]\033[0m Global test environment tear-down\n";
    }
    void OnEnvironmentsTearDownEnd(const ::testing::UnitTest&) override {}
    void OnTestIterationEnd(const ::testing::UnitTest&, int) override {}

    void OnTestProgramEnd(const ::testing::UnitTest& unit_test) override {
        std::cout << "\033[0;32m[==========]\033[0m " << unit_test.successful_test_count() + unit_test.failed_test_count()
                  << " tests from " << unit_test.test_suite_to_run_count() << " test suites ran. (" << unit_test.elapsed_time() << " ms total)\n";
        std::cout << "\033[0;32m[  PASSED  ]\033[0m " << unit_test.successful_test_count() << " tests.\n";
        if (unit_test.failed_test_count() > 0) {
            std::cout << "\033[0;31m[  FAILED  ]\033[0m " << unit_test.failed_test_count() << " tests.\n";
        }
    }
};

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    ::testing::UnitTest* unit_test = ::testing::UnitTest::GetInstance();
    ::testing::TestEventListeners& listeners = unit_test->listeners();

    // Remove the default GTest printer
    delete listeners.Release(listeners.default_result_printer());
    
    // Add our highly compressed Suite-Level printer
    listeners.Append(new TerseSuitePrinter);

    return RUN_ALL_TESTS();
}
