/**
 * @file Database.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <memory>
#include "Query.h"
#include "graph/core/Transaction.h"
#include "graph/storage/FileStorage.h"

namespace graph {

	class Database {
	public:
		explicit Database(const std::string& dbPath);
		~Database();

		void open();
		void close();
		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const; // Add this method declaration

		Query query();
		Transaction beginTransaction();

		[[nodiscard]] storage::FileStorage& getStorage() const { return *storage; }

	private:
		std::unique_ptr<storage::FileStorage> storage;
		std::string dbPath;
		bool openFlag = false; // Rename isOpen to openFlag to avoid conflict with method name
		std::unordered_map<uint64_t, Node> nodes;
		std::unordered_map<uint64_t, Edge> edges;
	};

} // namespace graph