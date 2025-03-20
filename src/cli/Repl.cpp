/**
 * @file Repl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/Repl.h"
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>

namespace graph {

	REPL::REPL(Database &db) : db(db) {}

	void REPL::run() const {
	    char *line = nullptr;
	    while ((line = readline("\033[1;32mmetrix>\033[0m ")) != nullptr) {
	        std::string command(line);
	        add_history(line);
	        free(line);

	        if (shouldExit(command)) {
	            break;
	        }

	        handleCommand(command);
	    }
	}

	bool REPL::shouldExit(const std::string &command) {
	    return command == "exit";
	}

	void REPL::handleCommand(const std::string &command) const {
		if (command == "addNode") {
			std::string label;
			std::cout << "Enter node label: ";
			std::getline(std::cin, label);
			auto transaction = db.beginTransaction();
			auto &node = transaction.createNode(label);
			transaction.commit();
			std::cout << "Added node with ID: " << node.getId() << "\n";
		} else if (command == "addEdge") {
			int from, to;
			std::string relation;
			std::cout << "Enter from node ID, to node ID, and relation: ";
			std::cin >> from >> to >> relation;
			auto transaction = db.beginTransaction();
			auto &fromNode = db.getStorage().getNodes().at(from);
			auto &toNode = db.getStorage().getNodes().at(to);
			auto &edge = transaction.createEdge(fromNode, toNode, relation);
			transaction.commit();
			std::cout << "Added edge with ID: " << edge.getId() << "\n";
		} else if (command == "queryNode") {
			int id;
			std::cout << "Enter node ID: ";
			std::cin >> id;
			try {
				const auto &node = db.getStorage().getNodes().at(id);
				std::cout << "Node ID: " << node.getId() << ", Label: " << node.getLabel() << "\n";
			} catch (const std::out_of_range &) {
				std::cout << "Node not found\n";
			}
		} else if (command == "queryEdge") {
			int id;
			std::cout << "Enter edge ID: ";
			std::cin >> id;
			try {
				const auto &edge = db.getStorage().getEdges().at(id);
				std::cout << "Edge ID: " << edge.getId() << ", From: " << edge.getFrom() << ", To: " << edge.getTo() << ", Relation: " << edge.getLabel() << "\n";
			} catch (const std::out_of_range &) {
				std::cout << "Edge not found\n";
			}
		} else {
			std::cout << "Unknown command\n";
		}
		db.close(); // Ensure the database is closed to save changes
	}

} // namespace graph
