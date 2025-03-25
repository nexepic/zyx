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
#include <string>
#include "graph/storage/FileStorage.h"
#include "graph/core/Node.h"
#include "graph/core/Edge.h"
#include "graph/core/query/QueryEngine.h"
#include "graph/core/Transaction.h"

namespace graph {

	class Database {
	public:
		explicit Database(const std::string& dbPath,
				 size_t cacheSize = 10000);
		~Database();

		void open();
		void close();
		void save();
		void flush();

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const { return openFlag; }

		Transaction beginTransaction();

		// Node and Edge access methods
		Node getNode(uint64_t id);
		Edge getEdge(uint64_t id);

		std::vector<Node> getNodes(const std::vector<uint64_t>& ids);
		std::vector<Edge> getEdges(const std::vector<uint64_t>& ids);

		std::vector<Node> getNodesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);
		std::vector<Edge> getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);

		// Query engine access
		std::shared_ptr<query::QueryEngine> getQueryEngine() { return queryEngine; }

		// Access to storage for advanced operations
		[[nodiscard]] storage::FileStorage& getStorage() const { return *storage; }

	private:
		std::shared_ptr<storage::FileStorage> storage;
		std::shared_ptr<query::QueryEngine> queryEngine;
		std::string dbPath;
		bool openFlag = false;
	};

} // namespace graph