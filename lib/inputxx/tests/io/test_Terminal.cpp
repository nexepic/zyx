#include <gtest/gtest.h>

#include "inputxx/io/Terminal.hpp"

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

class ScopedPseudoTerminalInput final {
public:
	ScopedPseudoTerminalInput() {
		master_ = posix_openpt(O_RDWR | O_NOCTTY);
		if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
			throw std::runtime_error("could not create a pseudo-terminal input stream");
		}
		char *const slavePath = ptsname(master_);
		if (slavePath == nullptr) {
			throw std::runtime_error("pseudo-terminal has no slave path");
		}
		const int slave = open(slavePath, O_RDWR | O_NOCTTY);
		if (slave < 0) {
			throw std::runtime_error("could not open pseudo-terminal slave");
		}
		saved_stdin_ = dup(STDIN_FILENO);
		saved_standard_output_ = dup(STDOUT_FILENO);
		if (saved_stdin_ < 0 || saved_standard_output_ < 0 ||
			dup2(slave, STDIN_FILENO) != STDIN_FILENO) {
			close(slave);
			if (saved_stdin_ >= 0) {
				static_cast<void>(close(saved_stdin_));
			}
			if (saved_standard_output_ >= 0) {
				static_cast<void>(close(saved_standard_output_));
			}
			saved_stdin_ = -1;
			saved_standard_output_ = -1;
			throw std::runtime_error("could not redirect standard input to pseudo-terminal");
		}
		close(slave);
	}

	~ScopedPseudoTerminalInput() {
		restoreStandardOutput();
		if (saved_stdin_ >= 0) {
			static_cast<void>(dup2(saved_stdin_, STDIN_FILENO));
			static_cast<void>(close(saved_stdin_));
		}
		if (master_ >= 0) {
			static_cast<void>(close(master_));
		}
	}

	ScopedPseudoTerminalInput(const ScopedPseudoTerminalInput &) = delete;
	ScopedPseudoTerminalInput &operator=(const ScopedPseudoTerminalInput &) = delete;

	void writeInput(const std::string_view input) const {
		size_t written = 0U;
		while (written < input.size()) {
			const ssize_t count = write(master_, input.data() + written, input.size() - written);
			if (count <= 0) {
				throw std::runtime_error("could not write pseudo-terminal input");
			}
			written += static_cast<size_t>(count);
		}
	}

	void waitForInput() const {
		fd_set readDescriptors;
		FD_ZERO(&readDescriptors);
		FD_SET(STDIN_FILENO, &readDescriptors);
		struct timeval timeout {1, 0};
		if (select(STDIN_FILENO + 1, &readDescriptors, nullptr, nullptr, &timeout) != 1) {
			throw std::runtime_error("pseudo-terminal input did not become readable");
		}
	}

	void redirectStandardOutput(const unsigned short columns) {
		const int slave = open(ptsname(master_), O_RDWR | O_NOCTTY);
		if (slave < 0) {
			throw std::runtime_error("could not reopen pseudo-terminal for standard output");
		}
		struct winsize size {};
		size.ws_col = columns;
		if (ioctl(slave, TIOCSWINSZ, &size) != 0 ||
			dup2(slave, STDOUT_FILENO) != STDOUT_FILENO) {
			static_cast<void>(close(slave));
			throw std::runtime_error("could not redirect standard output to pseudo-terminal");
		}
		static_cast<void>(close(slave));
	}

	void restoreStandardOutput() {
		if (saved_standard_output_ >= 0) {
			static_cast<void>(dup2(saved_standard_output_, STDOUT_FILENO));
			static_cast<void>(close(saved_standard_output_));
			saved_standard_output_ = -1;
		}
	}

private:
	int master_ = -1;
	int saved_stdin_ = -1;
	int saved_standard_output_ = -1;
};

class ScopedTerminalEnvironment final {
public:
	ScopedTerminalEnvironment() {
		if (const char *const value = std::getenv("TERM"); value != nullptr) {
			original_ = value;
		}
		if (setenv("TERM", "xterm", 1) != 0) {
			throw std::runtime_error("could not set a terminal type for pseudo-terminal testing");
		}
	}

	~ScopedTerminalEnvironment() {
		if (original_.has_value()) {
			static_cast<void>(setenv("TERM", original_->c_str(), 1));
		} else {
			static_cast<void>(unsetenv("TERM"));
		}
	}

	ScopedTerminalEnvironment(const ScopedTerminalEnvironment &) = delete;
	ScopedTerminalEnvironment &operator=(const ScopedTerminalEnvironment &) = delete;

private:
	std::optional<std::string> original_;
};

class ScopedStandardOutputCapture final {
public:
	ScopedStandardOutputCapture() {
		int pipeDescriptors[2]{};
		if (pipe(pipeDescriptors) != 0) {
			throw std::runtime_error("could not create a standard-output capture pipe");
		}
		read_end_ = pipeDescriptors[0];
		saved_standard_output_ = dup(STDOUT_FILENO);
		if (saved_standard_output_ < 0 || dup2(pipeDescriptors[1], STDOUT_FILENO) != STDOUT_FILENO) {
			static_cast<void>(close(pipeDescriptors[0]));
			static_cast<void>(close(pipeDescriptors[1]));
			throw std::runtime_error("could not redirect standard output");
		}
		static_cast<void>(close(pipeDescriptors[1]));
	}

	~ScopedStandardOutputCapture() { restore(); }

	ScopedStandardOutputCapture(const ScopedStandardOutputCapture &) = delete;
	ScopedStandardOutputCapture &operator=(const ScopedStandardOutputCapture &) = delete;

	[[nodiscard]] std::string restoreAndRead() {
		restore();
		std::string result;
		char buffer[32]{};
		for (;;) {
			const ssize_t count = read(read_end_, buffer, sizeof(buffer));
			if (count <= 0) {
				break;
			}
			result.append(buffer, static_cast<size_t>(count));
		}
		static_cast<void>(close(read_end_));
		read_end_ = -1;
		return result;
	}

private:
	void restore() {
		if (saved_standard_output_ >= 0) {
			static_cast<void>(dup2(saved_standard_output_, STDOUT_FILENO));
			static_cast<void>(close(saved_standard_output_));
			saved_standard_output_ = -1;
		}
	}

	int read_end_ = -1;
	int saved_standard_output_ = -1;
};

} // namespace

TEST(InputxxTerminalTest, RejectsRawModeForANonTerminalInputStream) {
	int pipeDescriptors[2]{};
	ASSERT_EQ(pipe(pipeDescriptors), 0);
	ASSERT_EQ(write(pipeDescriptors[1], "\x1b", 1), 1);
	const int savedStandardInput = dup(STDIN_FILENO);
	ASSERT_GE(savedStandardInput, 0);
	ASSERT_EQ(dup2(pipeDescriptors[0], STDIN_FILENO), STDIN_FILENO);
	static_cast<void>(close(pipeDescriptors[0]));
	static_cast<void>(close(pipeDescriptors[1]));

	inputxx::io::Terminal terminal;
	EXPECT_FALSE(terminal.enableRawMode());
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::ESC);
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);

	ASSERT_EQ(dup2(savedStandardInput, STDIN_FILENO), STDIN_FILENO);
	static_cast<void>(close(savedStandardInput));
}

TEST(InputxxTerminalTest, RejectsDumbPseudoTerminalBeforeChangingItsMode) {
	ScopedPseudoTerminalInput input;
	const char *const inheritedTerm = std::getenv("TERM");
	const std::optional<std::string> original = inheritedTerm == nullptr ? std::nullopt
																 : std::optional<std::string>(inheritedTerm);
	ASSERT_EQ(setenv("TERM", "dumb", 1), 0);

	inputxx::io::Terminal terminal;
	EXPECT_FALSE(terminal.enableRawMode());

	if (original.has_value()) {
		ASSERT_EQ(setenv("TERM", original->c_str(), 1), 0);
	} else {
		ASSERT_EQ(unsetenv("TERM"), 0);
	}
}

TEST(InputxxTerminalTest, ParsesPosixControlAndEscapeSequencesFromARawPseudoTerminal) {
	ScopedPseudoTerminalInput input;
	ScopedTerminalEnvironment environment;
	inputxx::io::Terminal terminal;
	ASSERT_TRUE(terminal.enableRawMode());
	EXPECT_TRUE(terminal.enableRawMode());

	input.writeInput("x");
	const inputxx::io::KeyEvent character = terminal.readKey();
	EXPECT_EQ(character.code, inputxx::io::KeyCode::CHARACTER);
	EXPECT_EQ(character.character, 'x');
	input.writeInput("\x01");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::CTRL_A);
	const auto expectControl = [&](const char inputByte, const inputxx::io::KeyCode expected) {
		input.writeInput(std::string_view(&inputByte, 1U));
		EXPECT_EQ(terminal.readKey().code, expected);
	};
	expectControl(2, inputxx::io::KeyCode::CTRL_B);
	expectControl(3, inputxx::io::KeyCode::CTRL_C);
	expectControl(4, inputxx::io::KeyCode::CTRL_D);
	expectControl(5, inputxx::io::KeyCode::CTRL_E);
	expectControl(6, inputxx::io::KeyCode::CTRL_F);
	expectControl(9, inputxx::io::KeyCode::TAB);
	expectControl(11, inputxx::io::KeyCode::CTRL_K);
	expectControl(12, inputxx::io::KeyCode::CTRL_L);
	expectControl(14, inputxx::io::KeyCode::CTRL_N);
	expectControl(16, inputxx::io::KeyCode::CTRL_P);
	expectControl(20, inputxx::io::KeyCode::CTRL_T);
	expectControl(21, inputxx::io::KeyCode::CTRL_U);
	expectControl(23, inputxx::io::KeyCode::CTRL_W);
	expectControl(8, inputxx::io::KeyCode::BACKSPACE);
	expectControl(127, inputxx::io::KeyCode::BACKSPACE);
	input.writeInput("\n");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::ENTER);
	input.writeInput("\r\nx");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::ENTER);
	const inputxx::io::KeyEvent afterCarriageReturn = terminal.readKey();
	EXPECT_EQ(afterCarriageReturn.code, inputxx::io::KeyCode::CHARACTER);
	EXPECT_EQ(afterCarriageReturn.character, 'x');
	input.writeInput("\x1b");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::ESC);
	input.writeInput("\x1b[A");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UP);
	input.writeInput("\x1b[B");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::DOWN);
	input.writeInput("\x1b[C");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::RIGHT);
	input.writeInput("\x1b[D");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::LEFT);
	input.writeInput("\x1b[H");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::HOME_KEY);
	input.writeInput("\x1b[F");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::END_KEY);
	input.writeInput("\x1b[3~");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::DELETE_KEY);
	input.writeInput("\x1b[1~");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::HOME_KEY);
	input.writeInput("\x1b[4~");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::END_KEY);
	input.writeInput("\x1b[1;5C");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::CTRL_RIGHT);
	input.writeInput("\x1b[1;5D");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::CTRL_LEFT);
	input.writeInput("\x1bOH");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::HOME_KEY);
	input.writeInput("\x1bOF");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::END_KEY);
	input.writeInput("\x1b" "b");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::CTRL_LEFT);
	input.writeInput("\x1b" "f");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::CTRL_RIGHT);
	input.writeInput("\x1b[2~");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1b[1;4C");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1b[/");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1b[2X");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1b[1;5Z");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1bOZ");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);
	input.writeInput("\x1bZ");
	EXPECT_EQ(terminal.readKey().code, inputxx::io::KeyCode::UNKNOWN);

	// Disable must preserve already typed input and make it available before any
	// subsequent terminal read, rather than discarding a paste tail.
	input.writeInput("q");
	input.waitForInput();
	terminal.disableRawMode();
	const inputxx::io::KeyEvent queued = terminal.readKey();
	EXPECT_EQ(queued.code, inputxx::io::KeyCode::CHARACTER);
	EXPECT_EQ(queued.character, 'q');
	EXPECT_GT(terminal.getColumns(), 0);
	ScopedStandardOutputCapture output;
	terminal.clearScreen();
	terminal.beep();
	EXPECT_EQ(output.restoreAndRead(), "\x1b[H\x1b[2J\x7");
	input.redirectStandardOutput(0U);
	EXPECT_EQ(terminal.getColumns(), 80);
	input.redirectStandardOutput(132U);
	EXPECT_EQ(terminal.getColumns(), 132);
	input.restoreStandardOutput();

	// A second raw-mode session has no typeahead to drain and must still restore
	// the pseudo-terminal cleanly.
	inputxx::io::Terminal emptyTerminal;
	ASSERT_TRUE(emptyTerminal.enableRawMode());
	emptyTerminal.disableRawMode();
	inputxx::io::Terminal queuedEscapeTerminal;
	ASSERT_TRUE(queuedEscapeTerminal.enableRawMode());
	input.writeInput("\x1b[A");
	input.waitForInput();
	queuedEscapeTerminal.disableRawMode();
	EXPECT_EQ(queuedEscapeTerminal.readKey().code, inputxx::io::KeyCode::UP);
	ASSERT_EQ(unsetenv("TERM"), 0);
	inputxx::io::Terminal terminalWithoutTerm;
	ASSERT_TRUE(terminalWithoutTerm.enableRawMode());
	terminalWithoutTerm.disableRawMode();
	ASSERT_EQ(setenv("TERM", "xterm", 1), 0);
}

#else

TEST(InputxxTerminalTest, PosixPseudoTerminalCoverageIsNotApplicableOnWindows) {
	GTEST_SKIP() << "Windows uses the native console input implementation";
}

#endif
