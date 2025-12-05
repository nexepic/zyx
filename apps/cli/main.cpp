/**
 * @file main.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <iostream>
#include "graph/cli/CLI11.hpp"
#include "graph/cli/Repl.hpp"
#include "graph/core/Database.hpp"
#include "graph/log/Log.hpp"

int main(int argc, char **argv) {
	CLI::App app{"metrix"};

	bool debugMode = false;
	app.add_flag("--debug", debugMode, "Enable verbose debug output (errors and internal states)");

	// Database subcommand
	auto database = app.add_subcommand("database", "Database operations");

	// --- Helper Lambda to Setup Environment ---
	auto setupEnv = [&]() {
		graph::log::Log::setDebug(debugMode);
		if (debugMode) {
			graph::log::Log::debug("Debug mode enabled.");
		}
	};

	// database create command
	std::string dbPath;
	auto createCmd = database->add_subcommand("create", "Create a new database");
	createCmd->add_option("path", dbPath, "Database file path")->required();
	createCmd->callback([&]() {
		setupEnv();
		graph::Database db(dbPath);
		graph::log::Log::info("Database created at: ", dbPath);
		const graph::REPL repl(db);
		repl.run();
	});

	// database open command
	auto openCmd = database->add_subcommand("open", "Open an existing database");
	openCmd->add_option("path", dbPath, "Database file path")->required();
	openCmd->callback([&]() {
		setupEnv();
		graph::Database db(dbPath);
		db.open();
		const graph::REPL repl(db);
		repl.run();
		db.close();
	});

	// database run command
	auto runCmd = database->add_subcommand("run", "Run database operations");
	runCmd->add_option("path", dbPath, "Database file path")->required();
	runCmd->callback([&]() {
		setupEnv();
		graph::Database db(dbPath);
		db.open();
		const graph::REPL repl(db);
		repl.run();
		db.close();
	});

	// Ensure correct subcommand usage
	database->callback([&]() {
		if (database->get_subcommands().empty()) {
			graph::log::Log::error("Missing or incorrect subcommand. Available subcommands: create, run");
			std::cout << database->help() << std::endl;
			exit(EXIT_FAILURE);
		}
	});

	app.require_subcommand(0, 1);

	try {
		CLI11_PARSE(app, argc, argv);
	} catch (const std::exception &e) {
		std::cerr << "Command line error: " << e.what() << std::endl;
		return 1;
	}

	if (argc == 1) {
		std::cout << app.help() << std::endl;
	}

	return 0;
}
