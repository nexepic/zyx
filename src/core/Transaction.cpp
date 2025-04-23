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

	Node Transaction::insertNode(const std::string &label) {
		Node node = storage.insertNode(label);
		return node;
	}

	Edge Transaction::insertEdge(const uint64_t &from, const uint64_t &to, const std::string &label) {
		Edge edge = storage.insertEdge(from, to, label);
		return edge;
	}

	void Transaction::commit() const { storage.commitWrite(); }

	void Transaction::rollback() const { storage.rollbackWrite(); }

} // namespace graph
