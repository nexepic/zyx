/**
 * @file Transaction.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Transaction.hpp"
#include "graph/core/Database.hpp"

namespace graph {

	Transaction::Transaction(const Database &db) : storage(db.getStorage()) { storage::FileStorage::beginWrite(); }

	Node Transaction::insertNode(const std::string &label) const {
		Node node = storage->insertNode(label);
		return node;
	}

	Edge Transaction::insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const {
		Edge edge = storage->insertEdge(from, to, label);
		return edge;
	}

	void Transaction::commit() const { storage->commitWrite(); }

	void Transaction::rollback() { storage::FileStorage::rollbackWrite(); }

} // namespace graph
