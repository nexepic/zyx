#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

class TerseSuitePrinter : public ::testing::TestEventListener {
private:
    std::stringstream captured_stdout_;
    std::stringstream captured_stderr_;
    std::streambuf* old_stdout_buf_{nullptr};
    std::streambuf* old_stderr_buf_{nullptr};
    int suite_failed_tests_{0};
    std::vector<std::string> current_test_failures_;
    
    int total_suites_{0};
    int completed_suites_{0};

    std::string getProgressPrefix() {
        char buf[64];
        int percentage = total_suites_ > 0 ? (completed_suites_ * 100 / total_suites_) : 0;
        // Using Bright Blue (\033[1;34m) for the progress bracket
        snprintf(buf, sizeof(buf), "\033[1;34m[%3d%% | %3d/%3d]\033[0m ", percentage, completed_suites_, total_suites_);
        return std::string(buf);
    }

public:
    void OnTestProgramStart(const ::testing::UnitTest& unit_test) override {
        total_suites_ = unit_test.test_suite_to_run_count();
        std::cout << "\n\033[1;34m================================================================================\033[0m\n"
                  << "\033[1m ZYX Graph Database - Unit Test Suite\033[0m\n"
                  << "\033[1;34m================================================================================\033[0m\n"
                  << " Target: " << unit_test.test_to_run_count() << " tests across " << total_suites_ << " suites\n\n";
    }

    void OnTestIterationStart(const ::testing::UnitTest&, int) override {}
    void OnEnvironmentsSetUpStart(const ::testing::UnitTest&) override {}
    void OnEnvironmentsSetUpEnd(const ::testing::UnitTest&) override {}

    void OnTestSuiteStart(const ::testing::TestSuite& test_suite) override {
        suite_failed_tests_ = 0;
    }

    void OnTestStart(const ::testing::TestInfo&) override {
        captured_stdout_.str(""); captured_stdout_.clear();
        captured_stderr_.str(""); captured_stderr_.clear();
        current_test_failures_.clear();
        
        old_stdout_buf_ = std::cout.rdbuf(captured_stdout_.rdbuf());
        old_stderr_buf_ = std::cerr.rdbuf(captured_stderr_.rdbuf());
    }

    void OnTestPartResult(const ::testing::TestPartResult& result) override {
        if (result.failed()) {
            std::stringstream ss;
            // Bold default color
            ss << "\033[1m" << result.file_name() << ":" << result.line_number() << "\033[0m\n"
               << result.message() << "\n";
            current_test_failures_.push_back(ss.str());
        }
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override {
        std::cout.rdbuf(old_stdout_buf_);
        std::cerr.rdbuf(old_stderr_buf_);

        if (test_info.result()->Failed()) {
            suite_failed_tests_++;
            std::cout << "\n" << getProgressPrefix() << "\033[1;31m✖ " << test_info.name() << " FAILED\033[0m\n";
            
            for (const auto& msg : current_test_failures_) {
                std::stringstream ss(msg);
                std::string line;
                while (std::getline(ss, line)) std::cout << "              " << line << "\n";
            }
            
            std::string out = captured_stdout_.str();
            std::string err = captured_stderr_.str();
            if (!out.empty()) {
                std::cout << "\033[1;33m              [STDOUT]\033[0m\n";
                std::stringstream ss(out);
                std::string line;
                while (std::getline(ss, line)) std::cout << "              " << line << "\n";
            }
            if (!err.empty()) {
                std::cerr << "\033[1;33m              [STDERR]\033[0m\n";
                std::stringstream ss(err);
                std::string line;
                while (std::getline(ss, line)) std::cerr << "              " << line << "\n";
            }
            std::cout << "\n";
        }
    }

    void OnTestSuiteEnd(const ::testing::TestSuite& test_suite) override {
        completed_suites_++;
        std::cout << getProgressPrefix();
        
        if (suite_failed_tests_ == 0) {
            std::cout << "\033[1;32m✔ PASS\033[0m \033[1m" << test_suite.name() << "\033[0m \033[1;34m(" << test_suite.elapsed_time() << " ms)\033[0m\n";
        } else {
            std::cout << "\033[1;31m✖ FAIL\033[0m \033[1m" << test_suite.name() << "\033[0m \033[1;31m(" << suite_failed_tests_ << " failures, " << test_suite.elapsed_time() << " ms)\033[0m\n";
        }
    }

    void OnEnvironmentsTearDownStart(const ::testing::UnitTest&) override {}
    void OnEnvironmentsTearDownEnd(const ::testing::UnitTest&) override {}
    void OnTestIterationEnd(const ::testing::UnitTest&, int) override {}

    void OnTestProgramEnd(const ::testing::UnitTest& unit_test) override {
        std::cout << "\n\033[1;34m================================================================================\033[0m\n";
        if (unit_test.failed_test_count() == 0) {
            std::cout << "\033[1;32m  🎉 ALL TESTS PASSED!\033[0m\n";
        } else {
            std::cout << "\033[1;31m  💥 TESTS FAILED!\033[0m\n";
        }
        std::cout << "\n";
        std::cout << "  Passed: \033[1;32m" << unit_test.successful_test_count() << "\033[0m\n"
                  << "  Failed: \033[1;31m" << unit_test.failed_test_count() << "\033[0m\n"
                  << "  Time  : " << unit_test.elapsed_time() << " ms\n"
                  << "\033[1;34m================================================================================\033[0m\n\n";
    }
};

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    ::testing::UnitTest* unit_test = ::testing::UnitTest::GetInstance();
    ::testing::TestEventListeners& listeners = unit_test->listeners();

    // Remove the default GTest printer
    delete listeners.Release(listeners.default_result_printer());
    
    // Add our modern progress printer
    listeners.Append(new TerseSuitePrinter);

    return RUN_ALL_TESTS();
}
