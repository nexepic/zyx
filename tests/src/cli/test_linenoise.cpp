/**
 * @file test_linenoise.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

// Include the header under test
#include "graph/cli/linenoise.hpp"

// Platform specific includes for I/O redirection
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#else
#include <io.h>
#include <fcntl.h>
#define pipe(fds) _pipe(fds, 4096, _O_BINARY)
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif

namespace fs = std::filesystem;

class LinenoiseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for file tests
        tempDir = fs::temp_directory_path() / "linenoise_test";
        fs::create_directories(tempDir);
        historyPath = tempDir / "history.txt";
    }

    void TearDown() override {
        // Cleanup global state
        linenoise::linenoiseAtExit();

        // Remove temporary files
        fs::remove_all(tempDir);
    }

    fs::path tempDir;
    fs::path historyPath;
};

// =========================================================================
// 1. History Management Tests
// =========================================================================

TEST_F(LinenoiseTest, History_AddAndRetrieve) {
    linenoise::linenoiseState ls;

    // Default max len is 100
    EXPECT_TRUE(ls.AddHistory("command1"));
    EXPECT_TRUE(ls.AddHistory("command2"));

    auto history = ls.GetHistory();
    ASSERT_EQ(history.size(), 2UL);
    EXPECT_EQ(history[0], "command1");
    EXPECT_EQ(history[1], "command2");
}

TEST_F(LinenoiseTest, History_DuplicateHandling) {
    linenoise::linenoiseState ls;

    EXPECT_TRUE(ls.AddHistory("command1"));
    // Adding the exact same command immediately should return false (ignored)
    EXPECT_FALSE(ls.AddHistory("command1"));

    EXPECT_TRUE(ls.AddHistory("command2"));
    // Adding command1 again is allowed if it's not the *immediate* previous one
    EXPECT_TRUE(ls.AddHistory("command1"));

    auto history = ls.GetHistory();
    ASSERT_EQ(history.size(), 3UL);
    EXPECT_EQ(history[0], "command1");
    EXPECT_EQ(history[1], "command2");
    EXPECT_EQ(history[2], "command1");
}

TEST_F(LinenoiseTest, History_MaxLenEnforcement) {
    linenoise::linenoiseState ls;
    ls.SetHistoryMaxLen(2);

    ls.AddHistory("1");
    ls.AddHistory("2");
    ls.AddHistory("3"); // Should push out "1"

    auto history = ls.GetHistory();
    ASSERT_EQ(history.size(), 2UL);
    EXPECT_EQ(history[0], "2");
    EXPECT_EQ(history[1], "3");
}

TEST_F(LinenoiseTest, History_ResizeSmaller) {
    linenoise::linenoiseState ls;
    ls.AddHistory("1");
    ls.AddHistory("2");
    ls.AddHistory("3");

    // Resize to smaller than current size
	ls.SetHistoryMaxLen(2);

	auto history = ls.GetHistory();
	ASSERT_EQ(history.size(), 2UL);
	// [Expect Correct Behavior] Keep latest
	EXPECT_EQ(history[0], "2");
	EXPECT_EQ(history[1], "3");
}

// =========================================================================
// 2. File Persistence Tests
// =========================================================================

TEST_F(LinenoiseTest, SaveAndLoadHistory) {
    {
        linenoise::linenoiseState ls;
        ls.AddHistory("save_me_1");
        ls.AddHistory("save_me_2");
        EXPECT_TRUE(ls.SaveHistory(historyPath.string().c_str()));
    }

    // Verify file exists
    EXPECT_TRUE(fs::exists(historyPath));

    {
        linenoise::linenoiseState ls2;
        EXPECT_TRUE(ls2.LoadHistory(historyPath.string().c_str()));
        auto history = ls2.GetHistory();
        ASSERT_EQ(history.size(), 2UL);
        EXPECT_EQ(history[0], "save_me_1");
        EXPECT_EQ(history[1], "save_me_2");
    }
}

TEST_F(LinenoiseTest, LoadNonExistentFile) {
    linenoise::linenoiseState ls;
    // Should return false cleanly
    EXPECT_FALSE(ls.LoadHistory("/path/to/nowhere/ghost.txt"));
}

// =========================================================================
// 3. UTF-8 and internal utility Tests
// =========================================================================

// Note: These functions are in the linenoise namespace.
TEST_F(LinenoiseTest, Utf8_ColumnPosition) {
    // Simple ASCII
    std::string ascii = "hello";
    EXPECT_EQ(linenoise::unicodeColumnPos(ascii.c_str(), ascii.length()), 5);

    // UTF-8 (Euro sign € is 3 bytes, width 1)
    std::string utf8_1 = "\xE2\x82\xAC"; // €
    EXPECT_EQ(linenoise::unicodeColumnPos(utf8_1.c_str(), utf8_1.length()), 1);

    // CJK Wide Character (Ni '你' is 3 bytes, width 2)
    std::string utf8_wide = "\xE4\xBD\xA0";
    EXPECT_EQ(linenoise::unicodeColumnPos(utf8_wide.c_str(), utf8_wide.length()), 2);

    // Combined: "a€你" -> 1 + 1 + 2 = 4
    std::string combined = "a" + utf8_1 + utf8_wide;
    EXPECT_EQ(linenoise::unicodeColumnPos(combined.c_str(), combined.length()), 4);
}

// =========================================================================
// 4. Input Simulation Tests (Readline)
// =========================================================================

// Helper to redirect stdin for testing Readline
class StdinRedirect {
public:
    StdinRedirect(const std::string& input) {
        // Create pipe
        if (pipe(fds) < 0) {
            throw std::runtime_error("Failed to create pipe");
        }

        // Save original stdin
        original_stdin = dup(STDIN_FILENO);

        // Write input to pipe
        if (write(fds[1], input.c_str(), input.size()) < 0) {
             // Handle write error
        }
        close(fds[1]); // Close write end

        // Redirect read end to stdin
        dup2(fds[0], STDIN_FILENO);
    }

    ~StdinRedirect() {
        // Restore stdin
        dup2(original_stdin, STDIN_FILENO);
        close(original_stdin);
        close(fds[0]);
    }

private:
    int fds[2];
    int original_stdin;
};

TEST_F(LinenoiseTest, Readline_NonInteractive) {
    // Since we are running in unit tests, isatty(0) usually returns false.
    // linenoise falls back to std::getline or fgets.

    std::string input = "Hello World\n";
    StdinRedirect redirect(input);

    linenoise::linenoiseState ls;
    std::string result;
    bool quit = ls.Readline(result);

    EXPECT_FALSE(quit);
    EXPECT_EQ(result, "Hello World");
}

TEST_F(LinenoiseTest, Readline_Empty) {
	std::string input = "\n";
	StdinRedirect redirect(input);

	linenoise::linenoiseState ls;
	ls.SetPrompt("prompt> ");

	std::string result;
	bool quit = ls.Readline(result);

	// [Expect Correct Behavior] Just an empty line, not EOF
	EXPECT_FALSE(quit);
	EXPECT_EQ(result, "");
}

// =========================================================================
// 5. Global API Wrapper Tests
// =========================================================================

TEST_F(LinenoiseTest, Global_Api_Wrapper) {
    // Test the static function wrappers
    linenoise::SetMultiLine(true);

    linenoise::SetHistoryMaxLen(5);
    EXPECT_TRUE(linenoise::AddHistory("global_1"));
    EXPECT_TRUE(linenoise::AddHistory("global_2"));

    auto history = linenoise::GetHistory();
    ASSERT_EQ(history.size(), 2UL);
    EXPECT_EQ(history[0], "global_1");

    // Test Save/Load via global
    EXPECT_TRUE(linenoise::SaveHistory(historyPath.string().c_str()));

    // Clear global state simulation (cannot delete lglobal easily, but can clear via new loads)
    // Actually lglobal persists across tests in the same process unless cleaned.
    // We rely on TearDown calling linenoiseAtExit which deletes lglobal.
}

TEST_F(LinenoiseTest, Callback_Registration) {
    // Just verify code compiles and runs without crash.
    // Callbacks are only triggered in interactive Raw mode which is hard to mock via pipe.
    linenoise::CompletionCallback cb = [](const char*, std::vector<std::string>&) {};

    linenoise::linenoiseState ls;
    ls.SetCompletionCallback(cb);

    // Global wrapper
    linenoise::SetCompletionCallback(cb);
}

// =========================================================================
// 6. Config Tests
// =========================================================================

TEST_F(LinenoiseTest, MultiLineMode) {
    linenoise::linenoiseState ls;
    // Just ensuring state toggles don't crash
    ls.EnableMultiLine();
    ls.DisableMultiLine();
}

TEST_F(LinenoiseTest, PromptSetting) {
    linenoise::linenoiseState ls;
    std::string p = "MyPrompt> ";
    ls.SetPrompt(p);
    // Prompt is internal, verify it acts on Readline without crash

    std::string input = "test\n";
    StdinRedirect redirect(input);
    std::string res;
    ls.Readline(res);
    EXPECT_EQ(res, "test");
}