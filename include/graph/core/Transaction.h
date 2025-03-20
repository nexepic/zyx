/**
 * @file Transaction.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <unordered_map>
#include "Edge.h"
#include "Node.h"
#include "graph/storage/FileStorage.h"

namespace graph {

	class Database;

	class Transaction {
	public:
	    explicit Transaction(Database &db);

	    Node &createNode(const std::string &label);
	    Edge &createEdge(const Node &from, const Node &to, const std::string &label);

	    void commit() const;
	    void rollback() const;

	private:
	    storage::FileStorage &storage;
	    std::unordered_map<uint64_t, Node> newNodes;
	    std::unordered_map<uint64_t, Edge> newEdges;
	};

} // namespace graph