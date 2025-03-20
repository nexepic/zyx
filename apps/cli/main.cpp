/**
 * @file main
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Database.h"
#include "graph/cli/CLI11.hpp"
#include "graph/cli/Repl.h"
#include <iostream>

int main(int argc, char **argv) {
	CLI::App app{"metrix"};

	// Database subcommand
	auto database = app.add_subcommand("database", "Database operations");

	// database create command
	std::string dbPath;
	auto createCmd = database->add_subcommand("create", "Create a new database");
	createCmd->add_option("path", dbPath, "Database file path")->required();
	createCmd->callback([&]() {
		graph::Database db(dbPath);
		std::cout << "Database created at: " << dbPath << "\n";
		graph::REPL repl(db);
		repl.run();
	});

	// database open command
	auto openCmd = database->add_subcommand("open", "Open an existing database");
	openCmd->add_option("path", dbPath, "Database file path")->required();
	openCmd->callback([&]() {
		graph::Database db(dbPath);
		db.open();
		graph::REPL repl(db);
		repl.run();
		db.close();
	});

	// database run command
	auto runCmd = database->add_subcommand("run", "Run database operations");
	runCmd->add_option("path", dbPath, "Database file path")->required();
	runCmd->callback([&]() {
		graph::Database db(dbPath);
		db.open();
		graph::REPL repl(db);
		repl.run();
		db.close();
	});

	// Ensure correct subcommand usage
	database->callback([&]() {
		if (database->get_subcommands().empty()) {
			std::cout << "Error: Missing or incorrect subcommand. Available subcommands: create, run\n";
			std::cout << database->help() << std::endl;
			exit(EXIT_FAILURE);
		}
	});

	// Show help if no subcommand is provided
	app.require_subcommand(0, 1);

	CLI11_PARSE(app, argc, argv);

	if (argc == 1) {
		std::cout << app.help() << std::endl;
	}

	return 0;
}
