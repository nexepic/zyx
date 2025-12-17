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
#include <iostream>
#include <filesystem>
#include "graph/cli/CLI11.hpp"
#include "graph/cli/Repl.hpp"
#include "graph/core/Database.hpp"
#include "graph/log/Log.hpp"

namespace graph::cli {

    CommandLineInterface::CommandLineInterface() = default;

    int CommandLineInterface::run(const int argc, char** argv) {
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
            Database db(dbPath);
            log::Log::info("Database initialized at: ", dbPath);
            const REPL repl(db);
            repl.run(); // Blocks until exit
        });

        // --- 2. Open (Interactive) ---
		const auto openCmd = database->add_subcommand("open", "Open an existing database interactively");
        openCmd->add_option("path", dbPath, "Database directory path")->required();
        openCmd->callback([&]() {
            Database db(dbPath);
            db.open();
            const REPL repl(db);
            repl.run(); // Blocks until exit
            db.close();
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

            Database db(dbPath);
            db.open(); // Will create if not exists or open if exists

			const REPL repl(db);
            // Non-blocking execution
            repl.runScript(scriptPath);

            if (auto storage = db.getStorage()) {
                storage->flush();
            }
            db.close();
        });

        // --- Boilerplate ---
        database->callback([&]() {
            if (database->get_subcommands().empty()) {
                throw CLI::CallForHelp();
            }
        });

        app.require_subcommand(0, 1);

        try {
            app.parse(argc, argv);
        } catch (const CLI::CallForHelp& e) {
            // output help is not an error
            return app.exit(e);
        } catch (const CLI::ParseError& e) {
            return app.exit(e);
        } catch (const std::exception& e) {
            std::cerr << "Runtime Error: " << e.what() << std::endl;
            return 1;
        }

        return 0;
    }

} // namespace graph::cli