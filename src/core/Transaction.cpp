/**
 * @file Transaction
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Transaction.h"
#include "graph/core/Database.h"

namespace graph {

	Transaction::Transaction(Database &db) : storage(db.getStorage()) { storage.beginWrite(); }

	Node &Transaction::createNode(const std::string &label) {
		Node node = storage.createNode(label);
		newNodes[node.getId()] = node;
		return newNodes[node.getId()];
	}

	Edge &Transaction::createEdge(const Node &from, const Node &to, const std::string &label) {
		Edge edge = storage.createEdge(from, to, label);
		newEdges[edge.getId()] = edge;
		return newEdges[edge.getId()];
	}

	void Transaction::commit() const { storage.commitWrite(); }

	void Transaction::rollback() const { storage.rollbackWrite(); }

} // namespace graph
