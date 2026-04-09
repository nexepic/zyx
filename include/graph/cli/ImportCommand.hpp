/**
 * @file ImportCommand.hpp
 * @brief CLI bulk import command for ZYX database.
 *
 * Supports Neo4j-compatible CSV and JSONL formats.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph {
class Database;
}

namespace CLI {
class App;
}

namespace graph::cli {

/**
 * @brief Registers the `import` subcommand on the given CLI::App.
 */
void registerImportCommand(CLI::App &parentApp);

/**
 * @brief Context holding ID-to-internal-ID mapping and import statistics.
 */
struct ImportContext {
	std::unordered_map<std::string, uint64_t> nodeIdMap; // external ID -> internal node ID
	uint64_t nodesCreated = 0;
	uint64_t edgesCreated = 0;
	uint64_t propertiesSet = 0;
	uint64_t errorsSkipped = 0;
};

} // namespace graph::cli
