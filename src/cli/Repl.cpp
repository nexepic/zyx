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
#include <sstream>

namespace graph {

    REPL::REPL(Database &db) : db(db) {}

    void REPL::run() const {
        // Ensure the database is open and ready
        db.getStorage().open();

        std::cout << "Database opened with "
                  << db.getStorage().getNodeCount() << " nodes and "
                  << db.getStorage().getEdgeCount() << " edges.\n";

        char *line = nullptr;
        while ((line = readline("\033[1;32mmetrix>\033[0m ")) != nullptr) {
            std::string command(line);
            if (!command.empty()) {
                add_history(line);
            }
            free(line);

            if (shouldExit(command)) {
                break;
            }

            handleCommand(command);
        }

        // Save changes before exiting
        db.getStorage().save();
        std::cout << "Database saved and closed.\n";
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
    		Node node(0, label); // Assuming Node constructor takes id and label
    		auto &newNode = transaction.insertNode(node);
    		transaction.commit();

    		std::cout << "Added node with ID: " << newNode.getId() << "\n";
    	}
    	else if (command != "addEdge") {
			if (command == "queryNode") {
				int id;
				std::cout << "Enter node ID: ";
				std::cin >> id;
				std::cin.ignore(); // Clear the newline

				try {
					// Use getNode instead of getNodes().at() for better error handling
					auto node = db.getStorage().getNode(id);
					if (node.getId() == 0) {
						std::cout << "Node not found\n";
					} else {
						std::cout << "Node ID: " << node.getId() << ", Label: " << node.getLabel() << "\n";
					}
				} catch (const std::exception &e) {
					std::cout << "Error: " << e.what() << "\n";
				}
			} else if (command == "queryEdge") {
				int id;
				std::cout << "Enter edge ID: ";
				std::cin >> id;
				std::cin.ignore(); // Clear the newline

				try {
					// Use getEdge instead of getEdges().at()
					auto edge = db.getStorage().getEdge(id);
					if (edge.getId() == 0) {
						std::cout << "Edge not found\n";
					} else {
						std::cout << "Edge ID: " << edge.getId() << ", From: " << edge.getFromNodeId()
								  << ", To: " << edge.getToNodeId() << ", Relation: " << edge.getLabel() << "\n";
					}
				} catch (const std::exception &e) {
					std::cout << "Error: " << e.what() << "\n";
				}
			} else if (command == "listNodes") {
				// Add a command to list all nodes
				const auto &nodes = db.getStorage().getAllNodes();
				if (nodes.empty()) {
					std::cout << "No nodes found\n";
				} else {
					std::cout << "Node count: " << nodes.size() << "\n";
					for (const auto &[id, node]: nodes) {
						std::cout << "ID: " << id << ", Label: " << node.getLabel() << "\n";
					}
				}
			} else if (command == "save") {
				db.getStorage().save();
				std::cout << "Database saved\n";
			} else if (command == "help") {
				std::cout << "Available commands:\n"
						  << "  addNode    - Add a new node\n"
						  << "  addEdge    - Add a new edge between nodes\n"
						  << "  queryNode  - Look up a node by ID\n"
						  << "  queryEdge  - Look up an edge by ID\n"
						  << "  listNodes  - List all nodes\n"
						  << "  save       - Save changes to disk\n"
						  << "  exit       - Exit the program\n"
						  << "  help       - Show this help\n";
			} else if (command.empty()) {
				// Handle empty command
			} else {
				std::cout << "Unknown command. Type 'help' for available commands.\n";
			}
		} else {
			int from, to;
			std::string relation;
			std::cout << "Enter from node ID: ";
			std::cin >> from;
			std::cout << "Enter to node ID: ";
			std::cin >> to;
			std::cin.ignore(); // Clear the newline
			std::cout << "Enter relation: ";
			std::getline(std::cin, relation);

			try {
				auto transaction = db.beginTransaction();
				// First get the nodes to make sure they exist
				auto fromNode = db.getStorage().getNode(from);
				auto toNode = db.getStorage().getNode(to);

				if (fromNode.getId() == 0 || toNode.getId() == 0) {
					std::cout << "One or both nodes do not exist\n";
					return;
				}

				Edge edge(0, fromNode.getId(), toNode.getId(),
						  relation); // Assuming Edge constructor takes id, from, to, and label
				auto &newEdge = transaction.insertEdge(fromNode, toNode, relation);
				transaction.commit();
				std::cout << "Added edge with ID: " << newEdge.getId() << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
				std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			}
		}
	}

} // namespace graph