/**
 * @file Repl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/Repl.hpp"
#include <iostream>
#include <sstream>
#include "graph/cli/linenoise.hpp"

namespace graph {

    // Helper to print properties map
    void printProperties(const std::unordered_map<std::string, PropertyValue>& props) {
        if (props.empty()) return;
        std::cout << " {";
        auto it = props.begin();
        while (it != props.end()) {
            std::cout << it->first << ": " << it->second.toString();
            if (++it != props.end()) {
                std::cout << ", ";
            }
        }
        std::cout << "}";
    }

    // Helper to print results prettily
    void printResult(const query::QueryResult& result) {
        if (result.isEmpty()) {
            std::cout << "Empty result (or operation successful with no return data).\n";
            return;
        }

        if (result.nodeCount() > 0) {
            std::cout << "--- Nodes (" << result.nodeCount() << ") ---\n";
            for (const auto& node : result.getNodes()) {
                std::cout << "ID: " << node.getId()
                          << " | Label: " << (node.getLabel().empty() ? "<No Label>" : node.getLabel());

                // Print properties for better debugging
                printProperties(node.getProperties());
                std::cout << "\n";
            }
        }

        if (result.edgeCount() > 0) {
            std::cout << "--- Edges (" << result.edgeCount() << ") ---\n";
            for (const auto& edge : result.getEdges()) {
                std::cout << "ID: " << edge.getId()
                          << " | " << edge.getSourceNodeId()
                          << " -[" << edge.getLabel();

                printProperties(edge.getProperties());

                std::cout << "]-> " << edge.getTargetNodeId() << "\n";
            }
        }

    	if (result.rowCount() > 0) {
    		std::cout << "--- Records (" << result.rowCount() << ") ---\n";

    		if (result.getRows().empty()) return;

    		for (const auto& row : result.getRows()) {
    			std::cout << "| ";
    			for (const auto& [key, val] : row) {
    				std::cout << key << ": " << val.toString() << " | ";
    			}
    			std::cout << "\n";
    		}
    	}
    }

    REPL::REPL(Database &db) : db(db) {}

    void REPL::run() const {
        const char *prompt = "\033[1;32mmetrix>\033[0m "; // Green prompt

        linenoise::linenoiseState ls(prompt);
        ls.SetHistoryMaxLen(100);

        std::cout << "<Metrix> Shell. Type 'help' for commands or enter Cypher queries directly.\n";

        std::string line;
        while (true) {
            if (ls.Readline(line)) break; // Ctrl+D to exit

            if (line == "exit") break;

            if (!line.empty()) {
                handleCommand(line);
                ls.AddHistory(line.c_str());
            }
        }
    }

    void REPL::handleCommand(const std::string &command) const {
        // --- 1. System Commands ---
        if (command == "help") {
            std::cout << "Commands:\n"
                      << "  save             Persist data to disk (Flush)\n"
                      << "  exit             Quit\n"
                      << "  [Cypher Query]   Execute Cypher (e.g., CREATE (n:User {name: 'Admin'}))\n";
            return;
        }

        if (command == "save") {
            // CRITICAL CHANGE: Use flush() instead of save().
            // flush() triggers the IStorageEventListener, ensuring IndexManager
            // persists the B-Tree roots before the file is saved.
            db.getStorage()->flush();
            std::cout << "Database flushed to disk.\n";
            return;
        }

        // --- 2. DSL Execution ---
        try {
            // The Engine orchestrates Parser -> Plan (Pipeline) -> Executor
            // We use the default language (Cypher)
            auto result = db.getQueryEngine()->execute(command);
            printResult(result);
        } catch (const std::exception &e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

} // namespace graph