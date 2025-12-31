/**
 * @file Repl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/Repl.hpp"
#include <iostream>
#include "graph/cli/linenoise.hpp"

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

	void printProperties(const std::unordered_map<std::string, PropertyValue> &props) {
		if (props.empty())
			return;
		std::cout << " {";
		auto it = props.begin();
		while (it != props.end()) {
			std::cout << it->first << ": " << it->second.toString();
			if (++it != props.end())
				std::cout << ", ";
		}
		std::cout << "}";
	}

	void printResult(const query::QueryResult &result) {
		if (result.isEmpty()) {
			std::cout << "Empty result (or operation successful with no return data).\n";
			return;
		}

		if (result.nodeCount() > 0) {
			std::cout << "--- Nodes (" << result.nodeCount() << ") ---\n";
			for (const auto &node: result.getNodes()) {
				std::cout << "ID: " << node.getId()
						  << " | Label: " << (node.getLabel().empty() ? "<No Label>" : node.getLabel());
				printProperties(node.getProperties());
				std::cout << "\n";
			}
		}

		if (result.edgeCount() > 0) {
			std::cout << "--- Edges (" << result.edgeCount() << ") ---\n";
			for (const auto &edge: result.getEdges()) {
				std::cout << "ID: " << edge.getId() << " | " << edge.getSourceNodeId() << " -[" << edge.getLabel();
				printProperties(edge.getProperties());
				std::cout << "]-> " << edge.getTargetNodeId() << "\n";
			}
		}

		if (result.rowCount() > 0) {
			std::cout << "--- Records (" << result.rowCount() << ") ---\n";
			if (result.getRows().empty())
				return;
			for (const auto &row: result.getRows()) {
				std::cout << "| ";
				for (const auto &[key, val]: row) {
					std::cout << key << ": " << val.toString() << " | ";
				}
				std::cout << "\n";
			}
		}
	}

	// --- REPL Implementation ---

	REPL::REPL(Database &db) : db(db) {}

	// Fallback REPL for non-interactive sessions (like debugging in some IDEs)
	void REPL::runBasic() const {
		std::cout << "<Metrix> Shell (Basic Mode).\n"
				  << "Type 'exit' to quit.\n";

		std::string line;
		std::string buffer;

		while (true) {
			const char *prompt = buffer.empty() ? "metrix> " : "     -> ";
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

		const char *promptNormal = "\033[1;32mmetrix>\033[0m "; // Green
		const char *promptMulti = "\033[1;33m     ->\033[0m "; // Yellow

		linenoise::SetMultiLine(true);
		linenoise::SetHistoryMaxLen(100);

		std::cout << "<Metrix> Shell.\n"
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
