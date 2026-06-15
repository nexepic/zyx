/**
 * @file NativeBulkLoader.cpp
 * @brief Storage-native bulk ingest session implementation.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/storage/data/NativeBulkLoader.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {

	NativeBulkLoader::NativeBulkLoader(std::shared_ptr<DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {
		if (!dataManager_) {
			throw std::invalid_argument("NativeBulkLoader requires a DataManager");
		}
	}

	int64_t NativeBulkLoader::tokenIdFor(const std::string &name) {
		if (name.empty()) {
			throw std::invalid_argument("Native bulk load requires non-empty token names");
		}
		if (const auto it = tokenCache_.find(name); it != tokenCache_.end()) {
			return it->second;
		}

		debug::ScopedPerfTimer timer("native_bulk.token_resolve");
		const int64_t tokenId = dataManager_->getOrCreateTokenId(name);
		tokenCache_.emplace(name, tokenId);
		return tokenId;
	}

	std::vector<int64_t> NativeBulkLoader::addNodes(
			const std::string &label,
			size_t count,
			const std::vector<BulkPropertyColumn> &columns) {
		const int64_t labelId = tokenIdFor(label);
		debug::ScopedPerfTimer timer("native_bulk.nodes.columnar_write");
		auto ids = dataManager_->addNodesColumnar(labelId, count, columns);
		stats_.nodeRows += ids.size();
		return ids;
	}

	std::vector<int64_t> NativeBulkLoader::addEdges(
			const std::string &type,
			const std::vector<int64_t> &sourceIds,
			const std::vector<int64_t> &targetIds,
			const std::vector<BulkPropertyColumn> &columns) {
		const int64_t typeId = tokenIdFor(type);
		debug::ScopedPerfTimer timer("native_bulk.edges.columnar_write");
		auto ids = dataManager_->addEdgesColumnar(typeId, sourceIds, targetIds, columns);
		stats_.edgeRows += ids.size();
		return ids;
	}

	void NativeBulkLoader::deferNodePropertyIndexes(
			const std::string &label,
			const std::vector<std::string> &properties) {
		if (label.empty()) {
			throw std::invalid_argument("Native bulk deferred node indexes require a non-empty label");
		}
		deferPropertyIndexes("node", label, properties);
	}

	void NativeBulkLoader::deferEdgePropertyIndexes(const std::vector<std::string> &properties) {
		deferPropertyIndexes("edge", "", properties);
	}

	void NativeBulkLoader::deferPropertyIndexes(
			const std::string &entityType,
			const std::string &label,
			const std::vector<std::string> &properties) {
		if (entityType != "node" && entityType != "edge") {
			throw std::invalid_argument("Native bulk deferred indexes require node or edge entity type");
		}
		for (const auto &property: properties) {
			if (property.empty()) {
				throw std::invalid_argument("Native bulk deferred indexes require non-empty property names");
			}
			const std::string key = entityType + "\n" + label + "\n" + property;
			if (!deferredIndexKeys_.insert(key).second) {
				continue;
			}
			deferredIndexes_.push_back({"", entityType, label, property});
		}
		stats_.deferredIndexRequests = deferredIndexes_.size();
	}

	std::vector<graph::query::indexes::IndexManager::IndexCreateResult>
	NativeBulkLoader::buildDeferredIndexes(graph::query::indexes::IndexManager &indexManager) {
		if (deferredIndexes_.empty()) {
			return {};
		}

		debug::ScopedPerfTimer timer("native_bulk.deferred_index_build");
		auto results = indexManager.createIndexes(deferredIndexes_);
		const bool success = std::all_of(results.begin(), results.end(), [](const auto &result) {
			return result.success;
		});
		if (success) {
			deferredIndexes_.clear();
			deferredIndexKeys_.clear();
			stats_.deferredIndexRequests = 0;
		}
		return results;
	}

} // namespace graph::storage
