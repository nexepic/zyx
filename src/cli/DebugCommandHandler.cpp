/**
 * @file DebugCommandHandler.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/DebugCommandHandler.hpp"
#include <iostream>
#include <sstream>
#include "graph/storage/DatabaseInspector.hpp"

namespace graph::cli {

	DebugCommandHandler::DebugCommandHandler(Database &db) : db_(db) {}

	void DebugCommandHandler::handle(const std::string &commandLine) const {
		auto storage = db_.getStorage();
		if (!storage) {
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
				bool isNumber = !arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit);

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
				uint32_t page = (!arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit)) ? std::stoi(arg) : 0;

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
