/**
 * @file DebugCommandHandler.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include "graph/core/Database.hpp"

namespace graph::cli {

	/**
	 * @brief Handles 'debug' commands from the REPL.
	 * Separates debugging logic from the main REPL loop for better maintainability.
	 */
	class DebugCommandHandler {
	public:
		explicit DebugCommandHandler(Database &db);

		/**
		 * @brief Parses and executes a debug command.
		 * @param commandLine The full command string (e.g., "debug states sys.config").
		 */
		void handle(const std::string &commandLine) const;

	private:
		Database &db_;

		static void printHelp();
	};

} // namespace graph::cli
