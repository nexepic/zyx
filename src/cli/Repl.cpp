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

	REPL::REPL(Database &db) : db(db) {}

	void REPL::run() const {
		// Ensure the database is open and ready
		db.getStorage().open();

		// Create a prompt that works on both Windows and Unix-like systems
#ifdef _WIN32
		const char* prompt = "metrix> ";
#else
		const char* prompt = "\033[1;32mmetrix>\033[0m ";
#endif

		// Create a linenoise state with our prompt
		linenoise::linenoiseState ls(prompt);

		// Set up history (in memory only)
		ls.SetHistoryMaxLen(100);

		// Enable multi-line mode for better editing
		ls.EnableMultiLine();

		// Set up auto-completion for common commands
		ls.SetCompletionCallback([](const char* editBuffer, std::vector<std::string>& completions) {
			const char* commands[] = {
				"addNode", "deleteNode", "addEdge", "deleteEdge", "addProperty",
				"listProperties", "queryNode", "queryEdge", "listNodes", "save",
				"help", "exit"
			};

			for (const auto& cmd : commands) {
				if (strncmp(editBuffer, cmd, strlen(editBuffer)) == 0) {
					completions.push_back(cmd);
				}
			}
		});

		std::string line;
		bool quit = false;

		while (!quit) {
			// Use the instance's Readline method
			quit = ls.Readline(line);

			if (quit || shouldExit(line)) {
				break;
			}

			if (!line.empty()) {
				// Process the command before adding to history
				handleCommand(line);

				// Add the command to history AFTER processing it
				ls.AddHistory(line.c_str());
			}
		}
	}

	bool REPL::shouldExit(const std::string &command) { return command == "exit"; }

	void REPL::handleCommand(const std::string &command) const {
		if (command == "addNode") {
			std::string label;
			std::cout << "Enter node label: ";
			std::getline(std::cin, label);

			auto transaction = db.beginTransaction();
			Node newNode = transaction.insertNode(label);
			// transaction.commit();

			std::cout << "Added node with ID: " << newNode.getId() << "\n";
		} else if (command == "deleteNode") {
			int nodeId;
			std::cout << "Enter node ID to delete: ";
			std::cin >> nodeId;
			std::cin.ignore(); // Clear the newline

			try {
				// auto transaction = db.beginTransaction();
				db.getStorage().deleteNode(nodeId); // Assuming cascadeEdges is true
				// transaction.commit();
				std::cout << "Deleted node with ID: " << nodeId << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "deleteEdge") {
			int edgeId;
			std::cout << "Enter edge ID to delete: ";
			std::cin >> edgeId;
			std::cin.ignore(); // Clear the newline

			try {
				// auto transaction = db.beginTransaction();
				db.getStorage().deleteEdge(edgeId);
				// transaction.commit();

				std::cout << "Deleted edge with ID: " << edgeId << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "addProperty") {
			// Add property to a node
			int nodeId;
			std::string key, valueType, value;

			std::cout << "Enter node ID: ";
			std::cin >> nodeId;
			std::cin.ignore(); // Clear the newline

			std::cout << "Enter property key: ";
			std::getline(std::cin, key);

			std::cout << "Enter property type (string/int/double/bool/text): ";
			std::getline(std::cin, valueType);

			std::cout << "Enter property value: ";

			PropertyValue propValue;

			if (valueType == "string") {
				std::getline(std::cin, value);
				propValue = value;
			} else if (valueType == "int") {
				int64_t intValue;
				std::cin >> intValue;
				std::cin.ignore();
				propValue = intValue;
			} else if (valueType == "double") {
				double doubleValue;
				std::cin >> doubleValue;
				std::cin.ignore();
				propValue = doubleValue;
			} else if (valueType == "bool") {
				std::string boolStr;
				std::getline(std::cin, boolStr);
				bool boolValue = (boolStr == "true" || boolStr == "1" || boolStr == "yes");
				propValue = boolValue;
			} else if (valueType == "text") {
				std::cout << "Enter multiline text (type END on a new line to finish):\n";
				std::string line;
				value = "";
				while (std::getline(std::cin, line) && line != "END") {
					value += line + "\n";
				}
				propValue = value;
			} else {
				std::cout << "Unsupported property type. Using string type.\n";
				std::getline(std::cin, value);
				propValue = value;
			}

			try {
				auto transaction = db.beginTransaction();
				auto node = db.getStorage().getNode(nodeId);

				if (node.getId() == 0) {
					std::cout << "Node not found\n";
					return;
				}

				// Create a map for the property
				std::unordered_map<std::string, PropertyValue> properties;
				properties[key] = propValue;

				// Add the property to the node
				db.getStorage().insertProperties(nodeId, Node::typeId, properties);
				// transaction.commit();

				std::cout << "Added property '" << key << "' to node " << nodeId << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "listProperties") {
			// List all properties of a node
			int nodeId;

			std::cout << "Enter node ID: ";
			std::cin >> nodeId;
			std::cin.ignore(); // Clear the newline

			try {
				auto node = db.getStorage().getNode(nodeId);

				if (node.getId() == 0) {
					std::cout << "Node not found\n";
					return;
				}

				const auto &properties = db.getStorage().getNodeProperties(node.getId());

				if (properties.empty()) {
					std::cout << "Node " << nodeId << " has no properties\n";
					return;
				}

				std::cout << "Properties of node " << nodeId << ":\n";
				for (const auto &[key, value]: properties) {
					std::cout << " - " << key << ": ";

					// Display type and brief value preview
					if (std::holds_alternative<std::string>(value)) {
						auto strValue = std::get<std::string>(value);
						// std::string preview = (strValue.length() > 50) ? strValue.substr(0, 47) + "..." : strValue;
						std::string preview = strValue;
						std::cout << "(string) " << preview << "\n";
					} else if (std::holds_alternative<int64_t>(value)) {
						std::cout << "(int) " << std::get<int64_t>(value) << "\n";
					} else if (std::holds_alternative<double>(value)) {
						std::cout << "(double) " << std::get<double>(value) << "\n";
					} else if (std::holds_alternative<bool>(value)) {
						std::cout << "(bool) " << (std::get<bool>(value) ? "true" : "false") << "\n";
					} else {
						std::cout << "(other type)\n";
					}
				}

			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		}
		else if (command == "addEdge") {
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

				Edge newEdge = transaction.insertEdge(from, to, relation);
				transaction.commit();
				std::cout << "Added edge with ID: " << newEdge.getId() << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
				std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			}
		} else if (command == "queryNode") {
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

					// Show property count
					const auto &props = node.getProperties();
					if (!props.empty()) {
						std::cout << "Properties: " << props.size() << " (use 'listProperties' command to view)\n";
					}
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
					std::cout << "ID: " << id << ", Label: " << node.getLabel();
					// Add property count information
					size_t propCount = node.getProperties().size();
					if (propCount > 0) {
						std::cout << " (" << propCount << " properties)";
					}
					std::cout << "\n";
				}
			}
		} else if (command == "save") {
			db.getStorage().flush();
			std::cout << "Database saved\n";
		} else if (command == "help") {
			std::cout << "Available commands:\n"
					  << "  addNode        - Add a new node\n"
					  << "  addEdge        - Add a new edge between nodes\n"
					  << "  addProperty    - Add a property to a node\n"
					  << "  getProperty    - Get a specific property from a node\n"
					  << "  listProperties - List all properties of a node\n"
					  << "  removeProperty - Remove a property from a node\n"
					  << "  addTextFile    - Add contents of a text file as a property\n"
					  << "  queryNode      - Look up a node by ID\n"
					  << "  queryEdge      - Look up an edge by ID\n"
					  << "  listNodes      - List all nodes\n"
					  << "  save           - Save changes to disk\n"
					  << "  exit           - Exit the program\n"
					  << "  help           - Show this help\n";
		} else if (command.empty()) {
			// Handle empty command
		} else {
			std::cout << "Unknown command. Type 'help' for available commands.\n";
		}
	}

} // namespace graph
