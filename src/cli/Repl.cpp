/**
 * @file Repl.cpp
 * @author Nexepic
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/cli/Repl.hpp"
#include "graph/cli/CypherCompleter.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "inputxx/Session.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "ProjectConfig.hpp"

// For isatty() and debugger detection
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace graph {

	// --- ANSI Color Helpers ---

	namespace ansi {
		inline bool colorEnabled() {
			static const bool enabled = isatty(STDOUT_FILENO) != 0;
			return enabled;
		}

		inline std::string dim(const std::string &s) { return colorEnabled() ? "\033[90m" + s + "\033[0m" : s; }
		inline std::string bold(const std::string &s) { return colorEnabled() ? "\033[1m" + s + "\033[0m" : s; }
		inline std::string boldWhite(const std::string &s) { return colorEnabled() ? "\033[1;37m" + s + "\033[0m" : s; }
		inline std::string green(const std::string &s) { return colorEnabled() ? "\033[32m" + s + "\033[0m" : s; }
		inline std::string boldGreen(const std::string &s) { return colorEnabled() ? "\033[1;32m" + s + "\033[0m" : s; }
		inline std::string boldYellow(const std::string &s) { return colorEnabled() ? "\033[1;33m" + s + "\033[0m" : s; }
		inline std::string boldRed(const std::string &s) { return colorEnabled() ? "\033[1;31m" + s + "\033[0m" : s; }
		inline std::string dimItalic(const std::string &s) { return colorEnabled() ? "\033[3;90m" + s + "\033[0m" : s; }
	} // namespace ansi

	// --- Helpers ---

	bool isDebuggerAttached() {
#ifdef __APPLE__
		int mib[4];
		struct kinfo_proc info;
		size_t size;

		info.kp_proc.p_flag = 0;

		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC;
		mib[2] = KERN_PROC_PID;
		mib[3] = getpid();

		size = sizeof(info);
		if (sysctl(mib, std::size(mib), &info, &size, nullptr, 0) != 0) {
			return false;
		}

		return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
		return false;
#endif
	}

	std::string trim(const std::string &str) {
		size_t first = str.find_first_not_of(" \t\r\n");
		if (std::string::npos == first) {
			return "";
		}
		size_t last = str.find_last_not_of(" \t\r\n");
		return str.substr(first, (last - first + 1));
	}

	std::string formatDuration(std::chrono::steady_clock::duration elapsed) {
		auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
		std::ostringstream oss;
		if (us >= 1000000) {
			oss << std::fixed << std::setprecision(2) << (static_cast<double>(us) / 1000000.0) << "s";
		} else {
			oss << std::fixed << std::setprecision(2) << (static_cast<double>(us) / 1000.0) << "ms";
		}
		return oss.str();
	}

	void printResult(const query::QueryResult &result, const query::ResultValue::TokenResolver &resolver,
					 std::chrono::steady_clock::duration elapsed) {
		auto rowCount = result.rowCount();
		std::string rowWord = (rowCount == 1) ? "row" : "rows";
		std::string timing = formatDuration(elapsed);

		if (result.isEmpty()) {
			std::cout << ansi::dim("(0 " + rowWord + ", " + timing + ")") << "\n";
			return;
		}

		const auto &cols = result.getColumns();
		const auto &rows = result.getRows();

		const size_t MAX_COL_WIDTH = 30;

		auto truncate = [&](const std::string &str) -> std::string {
			if (str.length() > MAX_COL_WIDTH) {
				return str.substr(0, MAX_COL_WIDTH - 3) + "...";
			}
			return str;
		};

		// 1. Process Headers
		std::vector<std::string> displayHeaders;
		std::vector<size_t> widths;
		displayHeaders.reserve(cols.size());
		widths.reserve(cols.size());

		for (const auto &col: cols) {
			std::string headerStr = truncate(col);
			displayHeaders.push_back(headerStr);
			widths.push_back(headerStr.length());
		}

		// 2. Process Data Rows (track which cells are null for coloring)
		std::vector<std::vector<std::string>> dataTable;
		std::vector<std::vector<bool>> isNull;
		dataTable.reserve(rows.size());
		isNull.reserve(rows.size());

		for (const auto &row: rows) {
			std::vector<std::string> rowStrings;
			std::vector<bool> rowNulls;
			rowStrings.reserve(cols.size());
			rowNulls.reserve(cols.size());
			for (size_t i = 0; i < cols.size(); ++i) {
				std::string valStr = "null";
				bool cellIsNull = true;
				auto it = row.find(cols[i]);
				if (it != row.end()) {
					valStr = it->second.toString(resolver);
					cellIsNull = false;
				}

				valStr = truncate(valStr);

				if (valStr.length() > widths[i]) {
					widths[i] = valStr.length();
				}
				rowStrings.push_back(std::move(valStr));
				rowNulls.push_back(cellIsNull);
			}
			dataTable.push_back(std::move(rowStrings));
			isNull.push_back(std::move(rowNulls));
		}

		// Add padding (1 space left, 1 space right)
		for (auto &w: widths)
			w += 2;

		// Unicode box-drawing characters
		const std::string H = "\u2500"; // ─
		const std::string V = "\u2502"; // │
		const std::string TL = "\u250c"; // ┌
		const std::string TR = "\u2510"; // ┐
		const std::string BL = "\u2514"; // └
		const std::string BR = "\u2518"; // ┘
		const std::string LT = "\u251c"; // ├
		const std::string RT = "\u2524"; // ┤
		const std::string TT = "\u252c"; // ┬
		const std::string BT = "\u2534"; // ┴
		const std::string CR = "\u253c"; // ┼

		// Helper: repeat a UTF-8 string n times
		auto repeat = [](const std::string &s, size_t n) -> std::string {
			std::string result;
			result.reserve(s.size() * n);
			for (size_t i = 0; i < n; ++i)
				result += s;
			return result;
		};

		// Helper: Print divider line with specified junction characters
		auto printDivider = [&](const std::string &left, const std::string &mid, const std::string &right) {
			std::cout << ansi::dim(left);
			for (size_t i = 0; i < widths.size(); ++i) {
				std::cout << ansi::dim(repeat(H, widths[i]));
				if (i < widths.size() - 1)
					std::cout << ansi::dim(mid);
			}
			std::cout << ansi::dim(right) << "\n";
		};

		// 3. Print Table
		printDivider(TL, TT, TR); // Top border: ┌──┬──┐

		// Print Header
		std::cout << ansi::dim(V);
		for (size_t i = 0; i < cols.size(); ++i) {
			std::ostringstream cell;
			cell << " " << std::left << std::setw(static_cast<int>(widths[i] - 1)) << displayHeaders[i];
			std::cout << ansi::boldWhite(cell.str()) << ansi::dim(V);
		}
		std::cout << "\n";

		printDivider(LT, CR, RT); // Header separator: ├──┼──┤

		// Print Data Rows
		for (size_t r = 0; r < dataTable.size(); ++r) {
			std::cout << ansi::dim(V);
			for (size_t i = 0; i < cols.size(); ++i) {
				std::ostringstream cell;
				cell << " " << std::left << std::setw(static_cast<int>(widths[i] - 1)) << dataTable[r][i];
				if (isNull[r][i]) {
					std::cout << ansi::dimItalic(cell.str());
				} else {
					std::cout << cell.str();
				}
				std::cout << ansi::dim(V);
			}
			std::cout << "\n";
		}

		printDivider(BL, BT, BR); // Bottom border: └──┴──┘

		// Row count with timing
		std::cout << ansi::dim("(" + std::to_string(rowCount) + " " + rowWord + ", " + timing + ")") << "\n";
	}

	// --- REPL Implementation ---

	REPL::REPL(Database &db) : db(db) {}

	const std::vector<std::string>& REPL::getCommandNames() {
		static const std::vector<std::string> commands = {
			"help", "save", "clear", "exit", "debug"
		};
		return commands;
	}

	void REPL::printBanner(bool basicMode) const {
		std::string version = PROJECT_VERSION_STR;
		std::string dbPath = db.getPath();

		if (basicMode) {
			std::cout << PROJECT_DISPLAY_STR << " Graph Database v" << version << " (Basic Mode)\n"
					  << "Connected to: " << dbPath << "\n"
					  << "Type 'exit' to quit.\n";
		} else {
			std::cout << ansi::boldGreen(std::string(PROJECT_DISPLAY_STR) + " Graph Database") << " v" << version
					  << "\n"
					  << ansi::dim("Connected to: " + dbPath) << "\n"
					  << "Type " << ansi::bold("help") << " for commands, " << ansi::bold("exit") << " to quit.\n";
		}
	}

	void REPL::echoMultilineBuffer(const std::string &buffer) const {
		// Only echo if the buffer contains multiple lines
		if (buffer.find('\n') == std::string::npos)
			return;

		std::istringstream stream(buffer);
		std::string line;
		while (std::getline(stream, line)) {
			std::string trimmed = trim(line);
			if (!trimmed.empty()) {
				std::cout << ansi::dim("  \u00bb " + trimmed) << "\n";
			}
		}
	}

	void REPL::runBasic() const {
		printBanner(true);

		std::string line;
		std::string buffer;
		static const std::string PROMPT_NORMAL = std::string(PROJECT_DISPLAY_STR) + "> ";

		while (true) {
			const char *prompt = buffer.empty() ? PROMPT_NORMAL.c_str() : "     -> ";
			std::cout << prompt << std::flush;

			if (!std::getline(std::cin, line)) {
				break; // EOF (Ctrl+D)
			}

			std::string trimmedLine = trim(line);
			if (trimmedLine == "exit") {
				break;
			}

			if (!buffer.empty()) {
				buffer += "\n";
			}
			buffer += line;

			if ((!trimmedLine.empty() && trimmedLine.back() == ';') || (trimmedLine.empty() && !buffer.empty())) {
				handleCommand(buffer);
				buffer.clear();
			}
		}
	}

	void REPL::run() const {
		if (isDebuggerAttached() || !isatty(STDIN_FILENO)) {
			runBasic();
			return;
		}

		inputxx::Session replSession(100);

		cli::CypherCompleter completer;
		replSession.setCompletionCallback(
			[&completer](const std::string& line, std::vector<std::string>& completions) {
				completer.complete(line, completions);
			});

		static const std::string PROMPT_COLOR = "\033[1;32m" + std::string(PROJECT_DISPLAY_STR) + ">\033[0m ";
		const std::string promptNormal = PROMPT_COLOR;
		const std::string promptMulti = "\033[1;33m     \u00b7\u00b7\u00b7\033[0m "; // Yellow ···

		printBanner(false);

		std::string buffer;

		while (true) {
			const std::string& currentPrompt = buffer.empty() ? promptNormal : promptMulti;

			auto result = replSession.readLine(currentPrompt);

			if (!result.has_value()) {
				break;
			}

			std::string line = result.value();
			std::string trimmedLine = trim(line);

			if (trimmedLine == "exit")
				break;

			if (trimmedLine.empty()) {
				if (!buffer.empty()) {
					replSession.addHistory(buffer);
					echoMultilineBuffer(buffer);
					handleCommand(buffer);
					buffer.clear();
				}
				continue;
			}

			if (buffer.empty()) {
				if (trimmedLine == "help" || trimmedLine == "save" || trimmedLine == "clear") {
					replSession.addHistory(trimmedLine);
					handleCommand(trimmedLine);
					continue;
				}
			}

			if (!buffer.empty())
				buffer += "\n";
			buffer += line;

			if (trimmedLine.back() == ';') {
				replSession.addHistory(buffer);
				echoMultilineBuffer(buffer);
				handleCommand(buffer);
				buffer.clear();
			}
		}
	}

	void REPL::runScript(const std::string &scriptPath) const {
		std::ifstream file(scriptPath);
		if (!file.is_open()) {
			std::cerr << "Error: Could not open file " << scriptPath << "\n";
			return;
		}

		std::cout << "Executing script: " << scriptPath << "\n";

		std::string line;
		std::string buffer;
		int statementsExecuted = 0;

		while (std::getline(file, line)) {
			std::string trimmed = trim(line);

			if (trimmed.empty())
				continue;
			if (trimmed.rfind("//", 0) == 0)
				continue;

			if (!buffer.empty())
				buffer += "\n";
			buffer += line;

			if (trim(buffer).back() == ';') {
				std::cout << ansi::dim("[stmt " + std::to_string(++statementsExecuted) + "]") << "\n";
				handleCommand(buffer);
				buffer.clear();
			}
		}

		if (!trim(buffer).empty()) {
			std::cout << ansi::dim("[stmt " + std::to_string(++statementsExecuted) + "] (Implicit EOF)") << "\n";
			handleCommand(buffer);
		}
	}

	void REPL::handleCommand(const std::string &command) const {
		std::string trimmed = trim(command);

		// Remove trailing semicolon if present for command matching
		if (!trimmed.empty() && trimmed.back() == ';') {
			trimmed = trim(trimmed.substr(0, trimmed.length() - 1));
		}

		// --- 1. Basic System Commands ---
		if (trimmed == "help") {
			printHelp();
			return;
		}

		if (trimmed == "clear") {
			std::cout << "\033[2J\033[H" << std::flush;
			return;
		}

		if (trimmed == "save") {
			if (auto storage = db.getStorage()) {
				storage->flush();
				std::cout << ansi::green("Database flushed to disk.") << "\n";
			} else {
				std::cerr << ansi::boldRed("Error: Storage not accessible.") << "\n";
			}
			return;
		}

		// --- 2. Debug Command Handler (Delegated) ---
		if (trimmed.rfind("debug", 0) == 0) {
			debugHandler_.handle(trimmed);
			return;
		}

		// --- 3. Execute Cypher Query ---
		try {
			auto start = std::chrono::steady_clock::now();
			auto result = db.getQueryEngine()->execute(command);
			auto elapsed = std::chrono::steady_clock::now() - start;

			// Build token resolver from DataManager for human-readable output
			query::ResultValue::TokenResolver resolver = nullptr;
			if (auto storage = db.getStorage()) {
				auto dm = storage->getDataManager();
				resolver = [dm](int64_t id) -> std::string {
					return dm->resolveTokenName(id);
				};
			}

			printResult(result, resolver, elapsed);
		} catch (const std::exception &e) {
			std::cerr << ansi::boldRed("Error: " + std::string(e.what())) << "\n";
		}
	}

	void REPL::printHelp() const {
		// Unicode table for help display
		const std::string H = "\u2500";
		const std::string V = "\u2502";
		const std::string TL = "\u250c";
		const std::string TR = "\u2510";
		const std::string BL = "\u2514";
		const std::string BR = "\u2518";
		const std::string LT = "\u251c";
		const std::string RT = "\u2524";
		const std::string TT = "\u252c";
		const std::string BT = "\u2534";

		auto repeat = [](const std::string &s, size_t n) -> std::string {
			std::string result;
			result.reserve(s.size() * n);
			for (size_t i = 0; i < n; ++i)
				result += s;
			return result;
		};

		const size_t col1W = 12; // command column
		const size_t col2W = 30; // description column

		auto printRow = [&](const std::string &cmd, const std::string &desc) {
			std::ostringstream c1, c2;
			c1 << " " << std::left << std::setw(static_cast<int>(col1W - 1)) << cmd;
			c2 << " " << std::left << std::setw(static_cast<int>(col2W - 1)) << desc;
			std::cout << ansi::dim(V) << ansi::boldWhite(c1.str()) << ansi::dim(V) << c2.str() << ansi::dim(V) << "\n";
		};

		auto printDiv = [&](const std::string &l, const std::string &m, const std::string &r) {
			std::cout << ansi::dim(l + repeat(H, col1W) + m + repeat(H, col2W) + r) << "\n";
		};

		auto printHeader = [&](const std::string &title) {
			std::ostringstream oss;
			oss << " " << std::left << std::setw(static_cast<int>(col1W + col2W)) << title;
			std::cout << ansi::dim(V) << ansi::boldWhite(oss.str()) << ansi::dim(V) << "\n";
		};

		// Top border
		std::cout << ansi::dim(TL + repeat(H, col1W + col2W + 1) + TR) << "\n";
		printHeader("Commands");
		std::cout << ansi::dim(LT + repeat(H, col1W) + TT + repeat(H, col2W) + RT) << "\n";

		printRow("help", "Show this help message");
		printRow("save", "Persist data to disk");
		printRow("clear", "Clear the screen");
		printRow("exit", "Quit the shell");

		printDiv(LT, "\u253c", RT);

		printRow("debug", "Debug mode (see 'debug help')");
		printRow("<query>;", "Execute a Cypher query");

		// Bottom border
		printDiv(BL, BT, BR);
	}

} // namespace graph
