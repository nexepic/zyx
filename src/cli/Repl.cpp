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
#include <charconv>
#include <iostream>
#include <sstream>
#include "graph/cli/linenoise.hpp"

namespace graph {

	// Helper function to pretty-print a PropertyValue
	void printPropertyValue(const PropertyValue& value) {
		std::visit([](const auto& arg) {
			using T = std::decay_t<decltype(arg)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				std::cout << "(null)";
			} else if constexpr (std::is_same_v<T, bool>) {
				std::cout << "(bool) " << (arg ? "true" : "false");
			} else if constexpr (std::is_same_v<T, int64_t>) {
				std::cout << "(integer) " << arg;
			} else if constexpr (std::is_same_v<T, double>) {
				std::cout << "(double) " << arg;
			} else if constexpr (std::is_same_v<T, std::string>) {
				std::cout << "(string) \"" << arg << "\"";
			}
		}, value.getVariant());
	}

	// Helper function to smartly parse user input into a PropertyValue
	PropertyValue parsePropertyValue(const std::string& input) {
		// 1. Check for specific keywords first.
		if (input == "true") return PropertyValue(true);
		if (input == "false") return PropertyValue(false);
		if (input == "null") return PropertyValue(std::monostate{});

		// 2. Try to parse as an integer. std::from_chars is best for this.
		int64_t int_val;
		auto [ptr_int, ec_int] = std::from_chars(input.data(), input.data() + input.size(), int_val);
		if (ec_int == std::errc() && ptr_int == input.data() + input.size()) {
			// Success: the entire string was consumed and is a valid integer.
			return PropertyValue(int_val);
		}

		// 3. Try to parse as a double using a more robust method.
		try {
			size_t pos = 0;
			double double_val = std::stod(input, &pos);
			// Check if the entire string was consumed. This is crucial for correctness.
			if (pos == input.size()) {
				return PropertyValue(double_val);
			}
		} catch (const std::invalid_argument&) {
			// Not a double, proceed.
		} catch (const std::out_of_range&) {
			// It's a number but too large/small for a double, treat as string.
		}

		// 4. If all other parsing attempts fail, treat it as a string.
		return PropertyValue(input);
	}

	REPL::REPL(Database &db) : db(db) {}

	void REPL::run() const {
#ifdef _WIN32
		const char *prompt = "metrix> ";
#else
		const char *prompt = "\033[1;32mmetrix>\033[0m ";
#endif

		linenoise::linenoiseState ls(prompt);
		ls.SetHistoryMaxLen(100);
		ls.EnableMultiLine();

		// Updated and complete list for auto-completion
		ls.SetCompletionCallback([](const char *editBuffer, std::vector<std::string> &completions) {
			const char *commands[] = {"addNode",
									  "addEdge",
									  "addProperty",
									  "deleteNode",
									  "deleteEdge",
									  "queryNode",
									  "queryEdge",
									  "listNodes",
									  "listEdges",
									  "listProperties",
									  "findNodesByLabel",
									  "findNodesByProperty",
									  "findEdgesByLabel",
									  "findEdgesByProperty",
									  "createIndex",
									  "dropIndex",
									  "listIndexes",
									  "save",
									  "help",
									  "exit"};

			for (const auto &cmd: commands) {
				if (strncmp(editBuffer, cmd, strlen(editBuffer)) == 0) {
					completions.emplace_back(cmd);
				}
			}
		});

		std::string line;
		while (true) {
			bool quit = ls.Readline(line);

			if (quit || shouldExit(line)) {
				break;
			}

			if (!line.empty()) {
				handleCommand(line);
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
			std::cout << "Added node with ID: " << newNode.getId() << "\n";
		} else if (command == "addEdge") {
			int64_t from, to;
			std::string relation;
			std::cout << "Enter from node ID: ";
			std::cin >> from;
			std::cout << "Enter to node ID: ";
			std::cin >> to;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			std::cout << "Enter relation (label): ";
			std::getline(std::cin, relation);
			try {
				auto transaction = db.beginTransaction();
				Edge newEdge = transaction.insertEdge(from, to, relation);
				std::cout << "Added edge with ID: " << newEdge.getId() << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "deleteNode") {
			int64_t nodeId;
			std::cout << "Enter node ID to delete: ";
			std::cin >> nodeId;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			try {
				db.getStorage()->deleteNode(nodeId); // Assuming cascade delete
				std::cout << "Deleted node with ID: " << nodeId << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "deleteEdge") {
			int64_t edgeId;
			std::cout << "Enter edge ID to delete: ";
			std::cin >> edgeId;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			try {
				db.getStorage()->deleteEdge(edgeId);
				std::cout << "Deleted edge with ID: " << edgeId << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "addProperty" || command == "listProperties" || command == "removeProperty") {
			// Consolidated block for all property-related operations
			std::string entityType;
			std::cout << "Enter entity type (node/edge): ";
			std::getline(std::cin, entityType);
			if (entityType != "node" && entityType != "edge") {
				std::cout << "Invalid entity type. Please use 'node' or 'edge'.\n";
				return;
			}

			int64_t entityId;
			std::cout << "Enter " << entityType << " ID: ";
			std::cin >> entityId;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

			uint32_t typeId = (entityType == "node") ? Node::typeId : Edge::typeId;

			if (command == "addProperty") {
			// A much smarter 'addProperty'
			std::string entityType, key, valueStr;
			int64_t entityId;

			std::cout << "Enter entity type (node/edge): ";
			std::getline(std::cin, entityType);
			std::cout << "Enter " << entityType << " ID: ";
			std::cin >> entityId; std::cin.ignore(); // Consume newline
			std::cout << "Enter property key: ";
			std::getline(std::cin, key);
			std::cout << "Enter property value: ";
			std::getline(std::cin, valueStr);

			if (entityType != "node" && entityType != "edge") {
				std::cout << "Invalid entity type.\n";
				return;
			}

			// Automatically parse the value
			PropertyValue propValue = parsePropertyValue(valueStr);

			try {
				uint32_t typeId = (entityType == "node") ? Node::typeId : Edge::typeId;
				db.getStorage()->insertProperties(entityId, typeId, {{key, propValue}});
				std::cout << "Added/updated property '" << key << "' for " << entityType << " " << entityId << " with value: ";
				printPropertyValue(propValue);
				std::cout << "\n";
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}

		} else if (command == "listProperties") {
			std::string entityType;
			int64_t entityId;
			std::cout << "Enter entity type (node/edge): ";
			std::getline(std::cin, entityType);
			std::cout << "Enter " << entityType << " ID: ";
			std::cin >> entityId; std::cin.ignore();

			if (entityType != "node" && entityType != "edge") {
				std::cout << "Invalid entity type.\n";
				return;
			}

			try {
				auto properties = (entityType == "node")
					? db.getStorage()->getNodeProperties(entityId)
					: db.getStorage()->getEdgeProperties(entityId);

				if (properties.empty()) {
					std::cout << "No properties found for " << entityType << " " << entityId << ".\n";
				} else {
					std::cout << "Properties for " << entityType << " " << entityId << ":\n";
					for (const auto &[key, value] : properties) {
						std::cout << " - " << key << ": ";
						printPropertyValue(value); // Use the helper to print
						std::cout << "\n";
					}
				}
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}

		}
		} else if (command == "queryNode") {
			int64_t id;
			std::cout << "Enter node ID: ";
			std::cin >> id;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			try {
				auto node = db.getStorage()->getNode(id);
				if (node.getId() == 0) {
					std::cout << "Node not found.\n";
				} else {
					std::cout << "Node ID: " << node.getId() << ", Label: " << node.getLabel() << "\n";
				}
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "queryEdge") {
			int64_t id;
			std::cout << "Enter edge ID: ";
			std::cin >> id;
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			try {
				auto edge = db.getStorage()->getEdge(id);
				if (edge.getId() == 0) {
					std::cout << "Edge not found.\n";
				} else {
					std::cout << "Edge ID: " << edge.getId() << ", From: " << edge.getSourceNodeId()
							  << ", To: " << edge.getTargetNodeId() << ", Label: " << edge.getLabel() << "\n";
				}
			} catch (const std::exception &e) {
				std::cout << "Error: " << e.what() << "\n";
			}
		} else if (command == "listNodes") {
			const auto &nodes = db.getStorage()->getAllNodes();
			if (nodes.empty()) {
				std::cout << "No nodes found in the database.\n";
			} else {
				std::cout << "Total nodes: " << nodes.size() << "\n";
				for (const auto &[id, node]: nodes) {
					std::cout << "ID: " << id << ", Label: " << node.getLabel() << "\n";
				}
			}
		} else if (command == "listEdges") {
			const auto &edges = db.getStorage()->getAllEdges();
			if (edges.empty()) {
				std::cout << "No edges found in the database.\n";
			} else {
				std::cout << "Total edges: " << edges.size() << "\n";
				for (const auto &[id, edge]: edges) {
					std::cout << "ID: " << id << ", " << edge.getSourceNodeId() << " -[" << edge.getLabel() << "]-> "
							  << edge.getTargetNodeId() << "\n";
				}
			}
		} else if (command == "createIndex" || command == "dropIndex" || command == "listIndexes") {
			std::string entityType;
			std::cout << "Enter entity type (node/edge): ";
			std::getline(std::cin, entityType);
			if (entityType != "node" && entityType != "edge") {
				std::cout << "Invalid entity type.\n";
				return;
			}
			if (command == "createIndex") {
				db.getQueryEngine()->buildIndexes(entityType);
				std::cout << "Build command for all '" << entityType << "' indexes issued.\n";
			} else if (command == "dropIndex") {
				std::string indexType;
				std::cout << "Enter index type to drop (label/property): ";
				std::getline(std::cin, indexType);
				std::string key = "";
				if (indexType == "property") {
					std::cout << "Enter property key (leave empty to drop all property indexes for this type): ";
					std::getline(std::cin, key);
				}
				db.getQueryEngine()->dropIndex(entityType, indexType, key);
				std::cout << "Drop command for " << entityType << " " << indexType << " index issued.\n";
			} else { // listIndexes
				auto indexes = db.getQueryEngine()->listIndexes(entityType);
				std::cout << "Active indexes for '" << entityType << "':\n";
				if (indexes.empty()) {
					std::cout << "  (none)\n";
				} else {
					for (const auto &[type, key]: indexes) {
						std::cout << "- " << type << (key.empty() ? "" : " on key: '" + key + "'") << "\n";
					}
				}
			}
		} else if (command == "findNodesByLabel" || command == "findNodesByProperty" || command == "findEdgesByLabel" ||
				   command == "findEdgesByProperty") {
			// This block handles all indexed find operations
			if (command == "findNodesByLabel" || command == "findEdgesByLabel") {
				std::string label;
				std::cout << "Enter label: ";
				std::getline(std::cin, label);
				if (command == "findNodesByLabel") {
					auto result = db.getQueryEngine()->findNodesByLabel(label);
					std::cout << "Found " << result.getNodes().size() << " node(s).\n";
					for (const auto &n: result.getNodes())
						std::cout << "  Node ID: " << n.getId() << ", Label: " << n.getLabel() << "\n";
				} else {
					auto result = db.getQueryEngine()->findEdgesByLabel(label);
					std::cout << "Found " << result.getEdges().size() << " edge(s).\n";
					for (const auto &e: result.getEdges())
						std::cout << "  Edge ID: " << e.getId() << ", " << e.getSourceNodeId() << "-[" << e.getLabel()
								  << "]->" << e.getTargetNodeId() << "\n";
				}
			} else if (command == "findNodesByProperty" || command == "findEdgesByProperty") {
				std::string key, valueStr;
				std::cout << "Enter property key: ";
				std::getline(std::cin, key);
				std::cout << "Enter property value to find: ";
				std::getline(std::cin, valueStr);

				// Parse the input string to find the correct PropertyValue to search for.
				PropertyValue searchValue = parsePropertyValue(valueStr);

				if (command == "findNodesByProperty") {
					auto result = db.getQueryEngine()->findNodesByProperty(key, searchValue);
					std::cout << "Found " << result.getNodes().size() << " node(s).\n";
					for (const auto &n: result.getNodes())
						std::cout << "  Node ID: " << n.getId() << ", Label: " << n.getLabel() << "\n";
				} else {
					auto result = db.getQueryEngine()->findEdgesByProperty(key, searchValue);
					std::cout << "Found " << result.getEdges().size() << " edge(s).\n";
					for (const auto &e: result.getEdges())
						std::cout << "  Edge ID: " << e.getId() << ", " << e.getSourceNodeId() << "-[" << e.getLabel()
								  << "]->" << e.getTargetNodeId() << "\n";
				}
			}
		} else if (command == "save") {
			db.getStorage()->flush();
			std::cout << "Database state flushed to disk.\n";
		} else if (command == "help") {
			std::cout << "--- MetrixDB REPL Help ---\n\n"
					  << "  -- Graph Modification --\n"
					  << "  addNode                  Add a new node.\n"
					  << "  addEdge                  Add a new edge between nodes.\n"
					  << "  addProperty              Add/update a property on a node or edge.\n"
					  << "  deleteNode               Delete a node and its connected edges.\n"
					  << "  deleteEdge               Delete an edge.\n\n"
					  << "  -- Index Management --\n"
					  << "  createIndex              Build all label and property indexes for nodes or edges.\n"
					  << "  dropIndex                Remove an index (prompts for details).\n"
					  << "  listIndexes              List all active indexes for nodes or edges.\n\n"
					  << "  -- Querying & Inspection --\n"
					  << "  queryNode                Look up a node by its ID.\n"
					  << "  queryEdge                Look up an edge by its ID.\n"
					  << "  listNodes                List all nodes in the database.\n"
					  << "  listEdges                List all edges in the database.\n"
					  << "  listProperties           List all properties of a node or edge.\n"
					  << "  findNodesByLabel         Find nodes with a specific label (uses index).\n"
					  << "  findNodesByProperty      Find nodes with a specific property (uses index).\n"
					  << "  findEdgesByLabel         Find edges with a specific label (uses index).\n"
					  << "  findEdgesByProperty      Find edges with a specific property (uses index).\n\n"
					  << "  -- System --\n"
					  << "  save                     Save all changes to disk.\n"
					  << "  exit                     Exit the application.\n"
					  << "  help                     Show this help message.\n\n";
		} else if (command.empty()) {
			// Do nothing for an empty command
		} else {
			std::cout << "Unknown command: '" << command << "'. Type 'help' for available commands.\n";
		}
	}

} // namespace graph
