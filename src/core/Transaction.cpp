/**
 * @file Transaction.cpp
 * @author Nexepic
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/core/Transaction.hpp"
#include "graph/core/Database.hpp"

namespace graph {

	Transaction::Transaction(const Database &db) : storage(db.getStorage()) { storage::FileStorage::beginWrite(); }

	Node Transaction::insertNode(const std::string &label) const {
		// TODO: Implement node insertion
		// Node node = storage->insertNode(label);
		// return node;
		return Node(); // Return default-constructed node until implementation is ready
	}

	Edge Transaction::insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const {
		// TODO: Implement edge insertion
		// Edge edge = storage->insertEdge(from, to, label);
		// return edge;
		return Edge(); // Return default-constructed edge until implementation is ready
	}

	void Transaction::commit() const { storage->commitWrite(); }

	void Transaction::rollback() { storage::FileStorage::rollbackWrite(); }

} // namespace graph
