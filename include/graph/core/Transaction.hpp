/**
 * @file Transaction.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <unordered_map>
#include "Edge.hpp"
#include "Node.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph {

	class Database;

	class Transaction {
	public:
		explicit Transaction(const Database &db);

		[[nodiscard]] Node insertNode(const std::string &label) const;
		[[nodiscard]] Edge insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const;

		void commit() const;
		static void rollback();

	private:
		std::shared_ptr<storage::FileStorage> storage;
	};

} // namespace graph
