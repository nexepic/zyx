/**
 * @file NativeBulkLoader.hpp
 * @brief Storage-native bulk ingest session with token caching and deferred index requests.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph/storage/data/ColumnarBulkInput.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::storage {
	class DataManager;

	class NativeBulkLoader {
	public:
		struct Stats {
			size_t nodeRows = 0;
			size_t edgeRows = 0;
			size_t deferredIndexRequests = 0;
		};

		explicit NativeBulkLoader(std::shared_ptr<DataManager> dataManager);

		std::vector<int64_t> addNodes(
				const std::string &label,
				size_t count,
				const std::vector<BulkPropertyColumn> &columns);

		std::vector<int64_t> addEdges(
				const std::string &type,
				const std::vector<int64_t> &sourceIds,
				const std::vector<int64_t> &targetIds,
				const std::vector<BulkPropertyColumn> &columns);

		void deferNodePropertyIndexes(const std::string &label, const std::vector<std::string> &properties);
		void deferEdgePropertyIndexes(const std::vector<std::string> &properties);

		std::vector<graph::query::indexes::IndexManager::IndexCreateResult>
		buildDeferredIndexes(graph::query::indexes::IndexManager &indexManager);

		[[nodiscard]] const Stats &stats() const noexcept { return stats_; }
		[[nodiscard]] bool hasDeferredIndexes() const noexcept { return !deferredIndexes_.empty(); }

	private:
		int64_t tokenIdFor(const std::string &name);
		void deferPropertyIndexes(
				const std::string &entityType,
				const std::string &label,
				const std::vector<std::string> &properties);

		std::shared_ptr<DataManager> dataManager_;
		std::unordered_map<std::string, int64_t> tokenCache_;
		std::vector<graph::query::indexes::IndexManager::IndexCreateRequest> deferredIndexes_;
		std::unordered_set<std::string> deferredIndexKeys_;
		Stats stats_;
	};

} // namespace graph::storage
