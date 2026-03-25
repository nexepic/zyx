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
#include <algorithm>
#include <iostream>
#include <sstream>
#include "graph/storage/DatabaseInspector.hpp"

namespace graph::cli {

	DebugCommandHandler::DebugCommandHandler(Database &db) : db_(db) {}

	void DebugCommandHandler::handle(const std::string &commandLine) const {
		auto storage = db_.getStorage();

		// Check if storage is null OR if it is logically closed.
		// If closed, getInspector() would fail or cause undefined behavior.
		if (!storage || !storage->isOpen()) {
			std::cerr << "Error: Storage is not active. Please open the database first.\n";
			return;
		}

		auto inspector = storage->getInspector();
		if (!inspector) {
			std::cerr << "Error: DatabaseInspector is not initialized.\n";
			return;
		}

		// Clean trailing semicolons
		std::string cleanCmd = commandLine;
		while (!cleanCmd.empty() && (cleanCmd.back() == ';' || std::isspace(cleanCmd.back()))) {
			cleanCmd.pop_back();
		}

		std::istringstream iss(cleanCmd);
		std::string cmd, target, arg;

		// Format: debug <target> [arg]
		iss >> cmd >> target;

		if (target.empty() || target == "help") {
			printHelp();
			return;
		}

		// Parse optional third argument (page number or state key)
		iss >> arg;

		try {
			if (target == "summary") {
				inspector->inspectSummary();
			} else if (target == "state" || target == "states") {
				// If the argument looks like a number, treat it as a page index.
				// Otherwise, treat it as a string Key for detailed lookup.
				bool isNumber = !arg.empty() && std::ranges::all_of(arg, ::isdigit);

				if (!arg.empty() && !isNumber) {
					// Inspect specific state data by Key
					inspector->inspectStateData(arg);
				} else {
					// Inspect raw segment page
					uint32_t page = isNumber ? std::stoi(arg) : 0;
					inspector->inspectStateSegments(page);
				}
			} else {
				// Handle standard segment inspectors
				uint32_t page = (!arg.empty() && std::ranges::all_of(arg, ::isdigit)) ? std::stoi(arg) : 0;

				if (target == "nodes")
					inspector->inspectNodeSegments(page);
				else if (target == "edges")
					inspector->inspectEdgeSegments(page);
				else if (target == "props")
					inspector->inspectPropertySegments(page);
				else if (target == "blobs")
					inspector->inspectBlobSegments(page);
				else if (target == "indexes")
					inspector->inspectIndexSegments(page);
				else {
					std::cerr << "Unknown debug target: '" << target << "'.\n";
					printHelp();
				}
			}
		} catch (const std::exception &e) {
			std::cerr << "Debug Error: " << e.what() << "\n";
		}
	}

	void DebugCommandHandler::printHelp() {
		std::cout << "Debug Commands:\n"
				  << "  debug summary           : Show global file header stats.\n"
				  << "  debug <type> [page]     : Inspect segment pages (types: nodes, edges, props, blobs, indexes, "
					 "states).\n"
				  << "  debug state <key>       : Inspect detailed properties of a specific State key.\n";
	}

} // namespace graph::cli
