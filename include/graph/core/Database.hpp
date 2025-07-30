/**
 * @file Database.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <string>
#include "graph/core/Transaction.hpp"
#include "graph/query/QueryEngine.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph {

	class Database {
	public:
		explicit Database(const std::string &dbPath, size_t cacheSize = 10000);
		~Database();

		void open();
		void close() const;
		void save() const;
		void flush() const;

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const;

		Transaction beginTransaction();

		// Query engine access
		std::shared_ptr<query::QueryEngine> getQueryEngine() { return queryEngine; }

		// Access to storage for advanced operations
		[[nodiscard]] std::shared_ptr<storage::FileStorage> getStorage() const { return storage; }

	private:
		std::shared_ptr<storage::FileStorage> storage;
		std::shared_ptr<query::QueryEngine> queryEngine;
		std::string dbPath;
	};

} // namespace graph
