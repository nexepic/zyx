/**
 * @file CommandLineInterface.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/CommandLineInterface.hpp"
#include <filesystem>
#include <iostream>
#include "graph/cli/CLI11.hpp"
#include "graph/cli/Repl.hpp"
#include "graph/core/Database.hpp"
#include "graph/log/Log.hpp"

namespace graph::cli {

	CommandLineInterface::CommandLineInterface() = default;

	int CommandLineInterface::run(const int argc, char **argv) {
		CLI::App app{"Metrix Database CLI"};

		// Prevent CLI11 from parsing automatically on destruction or throwing exit exceptions
		// We want to handle return codes manually for testing.
		app.set_help_all_flag("--help-all", "Expand all help");

		const auto database = app.add_subcommand("database", "Database operations");
		std::string dbPath;

		// --- 1. Create (Interactive) ---
		const auto createCmd = database->add_subcommand("create", "Create and open a new database interactively");
		createCmd->add_option("path", dbPath, "Database directory path")->required();
		createCmd->callback([&]() {
			try {
				Database db(dbPath, storage::OpenMode::CREATE_NEW);

				// Actual creation happens here (when file header is initialized)
				// If file exists, db.open() (called inside REPL or manually) will throw.
				// Since REPL constructor might not call open, we should call open() explicitly to trigger the check.
				db.open();

				log::Log::info("Database initialized at: ", dbPath);
				const REPL repl(db);
				repl.run();
			} catch (const std::exception &e) {
				std::cerr << "Error: " << e.what() << std::endl;
				// Don't swallow error, maybe exit(1) or let exception propagate if not handled here
			}
		});

		// --- 2. Open (Interactive) ---
		const auto openCmd = database->add_subcommand("open", "Open an existing database interactively");
		openCmd->add_option("path", dbPath, "Database directory path")->required();

		// Optional flag: Allow creation if missing via explicit flag
		bool createIfMissing = false;
		openCmd->add_flag("-c,--create-if-missing", createIfMissing, "Create database if it does not exist");

		openCmd->callback([&]() {
			try {
				auto mode = createIfMissing ? storage::OpenMode::CREATE_OR_OPEN : storage::OpenMode::OPEN_EXISTING;

				Database db(dbPath, mode);
				db.open(); // Will throw if OPEN_EXISTING and file missing

				const REPL repl(db);
				repl.run();
				db.close();
			} catch (const std::exception &e) {
				std::cerr << "Error: " << e.what() << std::endl;
			}
		});

		// --- 3. Exec (Script Mode - Testable) ---
		std::string scriptPath;
		const auto execCmd = database->add_subcommand("exec", "Execute a Cypher script and exit");
		execCmd->add_option("path", dbPath, "Database directory path")->required();
		execCmd->add_option("script", scriptPath, "Path to .cypher script")->required();

		execCmd->callback([&]() {
			if (!std::filesystem::exists(scriptPath)) {
				throw std::runtime_error("Script file not found: " + scriptPath);
			}

			try {
				Database db(dbPath, graph::storage::OpenMode::CREATE_OR_OPEN);
				db.open();

				const REPL repl(db);
				repl.runScript(scriptPath);

				if (auto storage = db.getStorage()) {
					storage->flush();
				}
				db.close();
			} catch (const std::exception &e) {
				std::cerr << "Execution Error: " << e.what() << std::endl;
			}
		});

		// --- Boilerplate ---
		database->callback([&]() {
			if (database->get_subcommands().empty()) {
				throw CLI::CallForHelp();
			}
		});

		app.require_subcommand(1);

		try {
			app.parse(argc, argv);
		} catch (const CLI::CallForHelp &e) {
			// output help is not an error
			return app.exit(e);
		} catch (const CLI::ParseError &e) {
			return app.exit(e);
		} catch (const std::exception &e) {
			std::cerr << "Runtime Error: " << e.what() << std::endl;
			return 1;
		}

		return 0;
	}

} // namespace graph::cli
