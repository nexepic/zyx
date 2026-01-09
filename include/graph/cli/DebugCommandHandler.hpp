/**
 * @file DebugCommandHandler.hpp
 * @author Nexepic
 * @date 2025/12/31
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
