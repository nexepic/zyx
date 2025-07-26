/**
 * @file IndexManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include "FullTextIndex.hpp"
#include "LabelIndex.hpp"
#include "PropertyIndex.hpp"
#include "RelationshipIndex.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph::query::indexes {

	class IndexBuilder;

	struct IndexConfig {
		bool labelIndexEnabled = false;
		std::unordered_set<std::string> propertyIndexKeys;
		bool relationshipIndexEnabled = false;
		bool fullTextIndexEnabled = false;
	};

	class IndexManager : public std::enable_shared_from_this<IndexManager> {
	public:
		explicit IndexManager(std::shared_ptr<storage::FileStorage> storage);
		~IndexManager();

		void initialize();

		// Index creation methods
		bool buildLabelIndex();
		bool buildPropertyIndex(const std::string &key);
		bool buildIndexes();

		// Async index building methods
		bool startBuildLabelIndex() const;
		bool startBuildPropertyIndex(const std::string &key) const;
		bool startBuildAllIndexes() const;

		// Check status of async index building
		bool isIndexBuilding() const;
		int getIndexBuildProgress() const;
		bool waitForIndexCompletion(int timeoutSeconds = 60) const;
		void cancelIndexBuild() const;

		// Drop index methods
		bool dropIndex(const std::string &indexType, const std::string &key = "");

		// List all active indexes
		std::vector<std::pair<std::string, std::string>> listIndexes() const;

		// Persist indexes to storage
		void persistState() const;

		// Enable/disable automatic index updates
		void enableLabelIndex(bool enable = true);
		void enablePropertyIndex(const std::string &key, bool enable = true);
		void enableRelationshipIndex(bool enable = true);
		void enableFullTextIndex(bool enable = true);

		// Update indexes with entity changes (for incremental updates)
		void updateIndexes(const std::vector<Node> &addedNodes, const std::vector<Node> &updatedNodes,
						   const std::vector<int64_t> &removedNodeIds, const std::vector<Edge> &addedEdges,
						   const std::vector<Edge> &updatedEdges, const std::vector<int64_t> &removedEdgeIds) const;

		// Update a single node in all enabled indexes
		void updateNodeIndexes(const Node &node, bool isNew = false, bool isDeleted = false) const;

		// Update a single edge in all enabled indexes
		void updateEdgeIndexes(const Edge &edge, bool isNew = false, bool isDeleted = false) const;

		// Accessors for individual indexes
		std::shared_ptr<LabelIndex> getLabelIndex() const;
		std::shared_ptr<PropertyIndex> getPropertyIndex() const;
		std::shared_ptr<RelationshipIndex> getRelationshipIndex() const;
		std::shared_ptr<FullTextIndex> getFullTextIndex() const;

		// Query methods
		std::vector<int64_t> findNodeIdsByLabel(const std::string &label) const;
		std::vector<int64_t> findNodeIdsByProperty(const std::string &key, const std::string &value) const;
		std::vector<int64_t> findNodeIdsByPropertyRange(const std::string &key, double minValue, double maxValue) const;
		std::vector<int64_t> findNodeIdsByTextSearch(const std::string &key, const std::string &searchText) const;
		std::vector<int64_t> findEdgeIdsByRelationship(const std::string &label) const;
		std::vector<int64_t> findEdgeIdsByNodes(int64_t fromNodeId, int64_t toNodeId) const;
		std::vector<int64_t> findOutgoingEdgeIds(int64_t nodeId, const std::string &label = "") const;
		std::vector<int64_t> findIncomingEdgeIds(int64_t nodeId, const std::string &label = "") const;

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::DataManager> dataManager_;

		std::shared_ptr<LabelIndex> labelIndex_;
		std::shared_ptr<PropertyIndex> propertyIndex_;
		std::shared_ptr<RelationshipIndex> relationshipIndex_;
		std::shared_ptr<FullTextIndex> fullTextIndex_;

		// Configuration for automatic index updates
		IndexConfig indexConfig_;

		// For asynchronous index building
		std::unique_ptr<IndexBuilder> indexBuilder_;

		// Thread safety
		mutable std::recursive_mutex mutex_;

		// Helper method to update property indexes
		void updatePropertyIndexes(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &oldProps,
								   const std::unordered_map<std::string, PropertyValue> &newProps) const;
	};

} // namespace graph::query::indexes
