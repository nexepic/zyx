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
#include "graph/cli/linenoise.hpp"

namespace graph {

    // --- Helpers ---

    std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) {
            return "";
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    void printProperties(const std::unordered_map<std::string, PropertyValue>& props) {
        if (props.empty()) return;
        std::cout << " {";
        auto it = props.begin();
        while (it != props.end()) {
            std::cout << it->first << ": " << it->second.toString();
            if (++it != props.end()) std::cout << ", ";
        }
        std::cout << "}";
    }

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

    // --- REPL Implementation ---

    REPL::REPL(Database &db) : db(db) {}

    void REPL::run() const {
        const char *promptNormal = "\033[1;32mmetrix>\033[0m "; // Green
        const char *promptMulti  = "\033[1;33m     ->\033[0m "; // Yellow

        linenoise::linenoiseState ls(promptNormal);
        ls.SetHistoryMaxLen(100);

        std::cout << "<Metrix> Shell.\n"
                  << "Type 'help' or 'exit'.\n"
                  << "Enter queries ending with ';' OR press Enter on an empty line to execute.\n";

        std::string buffer;
        std::string line;

        while (true) {
            // Update prompt based on buffer state
            if (buffer.empty()) {
                ls.SetPrompt(promptNormal);
            } else {
                ls.SetPrompt(promptMulti);
            }

            if (ls.Readline(line)) break; // Ctrl+D to exit

            std::string trimmedLine = trim(line);

            if (trimmedLine == "exit") break;

            // Empty Line Handling (Double Enter)
            if (trimmedLine.empty()) {
                // If buffer has content and user hits Enter on a blank line, EXECUTE.
                if (!buffer.empty()) {
                    handleCommand(buffer);
                    buffer.clear();
                }
                continue;
            }

            ls.AddHistory(line.c_str());

            // Handle System Commands (immediate execution if buffer empty)
            if (buffer.empty()) {
                if (trimmedLine == "help" || trimmedLine == "save") {
                    handleCommand(trimmedLine);
                    continue;
                }
            }

            // Accumulate Buffer
            if (!buffer.empty()) buffer += "\n";
            buffer += line;

            // Check for Semicolon at the end
            if (trimmedLine.back() == ';') {
                handleCommand(buffer);
                buffer.clear();
            }
        }
    }

    void REPL::runScript(const std::string& scriptPath) const {
        std::ifstream file(scriptPath);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open file " << scriptPath << "\n";
            return;
        }

        std::cout << "Executing script: " << scriptPath << "\n";

        std::string line;
        std::string buffer;
        int lineNum = 0;
        int statementsExecuted = 0;

        while (std::getline(file, line)) {
            lineNum++;
            std::string trimmed = trim(line);

            if (trimmed.empty()) continue;
            if (trimmed.rfind("//", 0) == 0) continue; // Skip comments

            if (!buffer.empty()) buffer += "\n";
            buffer += line;

            // Check for semicolon
            if (trim(buffer).back() == ';') {
                std::cout << "[stmt " << ++statementsExecuted << "] ";
                handleCommand(buffer);
                buffer.clear();
            }
        }

        if (!trim(buffer).empty()) {
            std::cout << "[stmt " << ++statementsExecuted << "] (Implicit EOF) ";
            handleCommand(buffer);
        }
    }

    void REPL::handleCommand(const std::string &command) const {
        if (command == "help") {
            std::cout << "Commands:\n"
                      << "  save             Persist data to disk\n"
                      << "  exit             Quit\n"
                      << "  <Cypher Query>   Ends with ';' or Empty Line\n";
            return;
        }

        if (command == "save") {
            db.getStorage()->flush();
            std::cout << "Database flushed to disk.\n";
            return;
        }

        try {
            auto result = db.getQueryEngine()->execute(command);
            printResult(result);
        } catch (const std::exception &e) {
            std::cerr << "\033[1;31mError: " << e.what() << "\033[0m\n";
        }
    }

} // namespace graph