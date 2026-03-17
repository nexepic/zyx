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
#include <iomanip>
#include <iostream>
#include "graph/cli/linenoise.hpp"
#include "ProjectConfig.hpp"

// For isatty() and debugger detection
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace graph {

	// --- Helpers ---

	// Function to check if a debugger is attached (macOS specific)
	bool isDebuggerAttached() {
#ifdef __APPLE__
		int mib[4];
		struct kinfo_proc info;
		size_t size;

		// Initialize the flags so we don't touch any random memory.
		info.kp_proc.p_flag = 0;

		// Initialize mib, which tells sysctl the info we want, in this case
		// we're looking for information about a specific process ID.
		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC;
		mib[2] = KERN_PROC_PID;
		mib[3] = getpid();

		// Call sysctl.
		size = sizeof(info);
		if (sysctl(mib, std::size(mib), &info, &size, nullptr, 0) != 0) {
			return false; // Could not retrieve process info
		}

		// We're being debugged if the P_TRACED flag is set.
		return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
		// Fallback for other Unix-like systems or disable for non-Apple.
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

	void printResult(const query::QueryResult &result) {
		if (result.isEmpty()) {
			std::cout << "Empty result.\n";
			return;
		}

		const auto &cols = result.getColumns();
		const auto &rows = result.getRows();

		// Configuration: Stricter max width (30 chars) to prevent line wrapping
		const size_t MAX_COL_WIDTH = 30;

		// Helper: Truncate string with ellipsis
		auto truncate = [&](const std::string &str) -> std::string {
			if (str.length() > MAX_COL_WIDTH) {
				return str.substr(0, MAX_COL_WIDTH - 3) + "...";
			}
			return str;
		};

		// 1. Process Headers (Apply truncation AND calculate initial width)
		std::vector<std::string> displayHeaders;
		std::vector<size_t> widths;
		displayHeaders.reserve(cols.size());
		widths.reserve(cols.size());

		for (const auto &col: cols) {
			// Truncate the header name itself!
			std::string headerStr = truncate(col);
			displayHeaders.push_back(headerStr);
			widths.push_back(headerStr.length());
		}

		// 2. Process Data Rows
		std::vector<std::vector<std::string>> dataTable;
		dataTable.reserve(rows.size());

		for (const auto &row: rows) {
			std::vector<std::string> rowStrings;
			rowStrings.reserve(cols.size());
			for (size_t i = 0; i < cols.size(); ++i) {
				std::string valStr = "null";
				// Use original 'cols' to find data, but align with 'widths' index
				auto it = row.find(cols[i]);
				if (it != row.end()) {
					valStr = it->second.toString();
				}

				// Apply truncation to data values
				valStr = truncate(valStr);

				// Update max width if data is wider than header
				if (valStr.length() > widths[i]) {
					widths[i] = valStr.length();
				}
				rowStrings.push_back(std::move(valStr));
			}
			dataTable.push_back(std::move(rowStrings));
		}

		// Add padding (1 space left, 1 space right)
		for (auto &w: widths)
			w += 2;

		// Helper: Print divider line
		auto printDivider = [&](char junction = '+', char dash = '-') {
			std::cout << junction;
			for (size_t w: widths) {
				std::cout << std::string(w, dash) << junction;
			}
			std::cout << "\n";
		};

		// 3. Print Table
		printDivider(); // Top border

		// Print Header using truncated 'displayHeaders'
		std::cout << "|";
		for (size_t i = 0; i < cols.size(); ++i) {
			std::cout << " " << std::left << std::setw(widths[i] - 1) << displayHeaders[i] << "|";
		}
		std::cout << "\n";

		printDivider(); // Header separator

		// Print Data Rows
		for (const auto &rowStrings: dataTable) {
			std::cout << "|";
			for (size_t i = 0; i < cols.size(); ++i) {
				std::cout << " " << std::left << std::setw(widths[i] - 1) << rowStrings[i] << "|";
			}
			std::cout << "\n";
		}

		printDivider(); // Bottom border
		std::cout << "(" << result.rowCount() << " rows)\n";
	}

	// --- REPL Implementation ---

	REPL::REPL(Database &db) : db(db) {}

	// Fallback REPL for non-interactive sessions (like debugging in some IDEs)
	void REPL::runBasic() const {
		std::cout << "<" << PROJECT_DISPLAY_STR << "> Shell (Basic Mode).\n"
				  << "Type 'exit' to quit.\n";

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
		// Check if a debugger is attached or if not in a TTY.
		// If so, fall back to basic I/O to avoid conflicts.
		if (isDebuggerAttached() || !isatty(STDIN_FILENO)) {
			runBasic();
			return;
		}

		linenoise::SetMultiLine(true);
		linenoise::SetHistoryMaxLen(100);
		static const std::string PROMPT_COLOR = "\033[1;32m" + std::string(PROJECT_DISPLAY_STR) + ">\033[0m ";
		const char *promptNormal = PROMPT_COLOR.c_str();
		const char *promptMulti = "\033[1;33m     ->\033[0m "; // Yellow

		std::cout << "<" << PROJECT_DISPLAY_STR << "> Shell.\n"
				  << "Type 'help' or 'exit'.\n"
				  << "Enter queries ending with ';' OR press Enter on an empty line to execute.\n";

		std::string buffer;

		while (true) {
			const char *currentPrompt = buffer.empty() ? promptNormal : promptMulti;

			bool quit = false;
			std::string line = linenoise::Readline(currentPrompt, quit);

			if (quit)
				break; // Handle Ctrl+D

			std::string trimmedLine = trim(line);

			if (trimmedLine == "exit")
				break;

			if (trimmedLine.empty()) {
				if (!buffer.empty()) {
					linenoise::AddHistory(buffer.c_str());
					handleCommand(buffer);
					buffer.clear();
				}
				continue;
			}

			if (buffer.empty()) {
				if (trimmedLine == "help" || trimmedLine == "save") {
					linenoise::AddHistory(trimmedLine.c_str());
					handleCommand(trimmedLine);
					continue;
				}
			}

			if (!buffer.empty())
				buffer += "\n";
			buffer += line;

			if (trimmedLine.back() == ';') {
				linenoise::AddHistory(buffer.c_str());
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
				continue; // Skip comments

			if (!buffer.empty())
				buffer += "\n";
			buffer += line;

			if (trim(buffer).back() == ';') {
				std::cout << "[stmt " << ++statementsExecuted << "] ";
				handleCommand(buffer);
				buffer.clear();
			}
		}

		if (!trim(buffer).empty()) {
			std::cout << "[stmt " << ++statementsExecuted << "] (Implicit EOF) ";
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
			std::cout << "Commands:\n";
			constexpr int colWidth = 18;
			std::cout << "  " << std::left << std::setw(colWidth) << "save" << "Persist data to disk\n"
					  << "  " << std::left << std::setw(colWidth) << "debug" << "Enter debug mode (see 'debug help')\n"
					  << "  " << std::left << std::setw(colWidth) << "exit" << "Quit\n"
					  << "  " << std::left << std::setw(colWidth) << "<Query>" << "Cypher query ending in ';'\n";
			return;
		}

		if (trimmed == "save") {
			if (auto storage = db.getStorage()) {
				storage->flush();
				std::cout << "Database flushed to disk.\n";
			} else {
				std::cerr << "Error: Storage not accessible.\n";
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
			auto result = db.getQueryEngine()->execute(command);
			printResult(result);
		} catch (const std::exception &e) {
			std::cerr << "\033[1;31mError: " << e.what() << "\033[0m\n";
		}
	}

} // namespace graph
