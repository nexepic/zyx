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

	Node &Transaction::insertNode(const Node &node) {
		uint64_t nodeId = storage.insertNode(node);
		newNodes[nodeId] = node;
		newNodes[nodeId].setId(nodeId);
		return newNodes[nodeId];
	}

	Edge &Transaction::insertEdge(const Node &from, const Node &to, const std::string &label) {
		Edge edge(0, from.getId(), to.getId(), label);
		uint64_t edgeId = storage.insertEdge(edge);
		newEdges[edgeId] = edge;
		newEdges[edgeId].setId(edgeId);
		return newEdges[edgeId];
	}

	void Transaction::commit() const { storage.commitWrite(); }

	void Transaction::rollback() const { storage.rollbackWrite(); }

} // namespace graph
