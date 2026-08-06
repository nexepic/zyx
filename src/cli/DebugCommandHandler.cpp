/**
 * @file DebugCommandHandler.cpp
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

#include "graph/cli/DebugCommandHandler.hpp"
#include <iostream>
#include <sstream>

namespace graph::cli {

	DebugCommandHandler::DebugCommandHandler([[maybe_unused]] Database &db) {}

	void DebugCommandHandler::handle(const std::string &commandLine) const {
		// Clean trailing semicolons
		std::string cleanCmd = commandLine;
		while (!cleanCmd.empty() &&
				(cleanCmd.back() == ';' || std::isspace(static_cast<unsigned char>(cleanCmd.back())))) {
			cleanCmd.pop_back();
		}

		std::istringstream iss(cleanCmd);
		std::string cmd, target;

		// Format: debug <target> [arg]
		iss >> cmd >> target;

		if (target.empty() || target == "help") {
			printHelp();
			return;
		}
		// Public Database is native relocatable-only; the segment inspector belongs to
		// the isolated legacy migration and physical-test paths.
		std::cerr << "Error: the legacy segment inspector is unavailable for the native relocatable database format.\n";
	}

	void DebugCommandHandler::printHelp() {
		std::cout << "Debug Commands:\n"
				  << "  debug help              : Show this message.\n"
				  << "  Physical segment inspection is unavailable for native relocatable databases.\n";
	}

} // namespace graph::cli
