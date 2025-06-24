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

	class IndexManager {
	public:
		explicit IndexManager(std::shared_ptr<storage::FileStorage> storage);

		bool buildLabelIndex();

		bool buildPropertyIndex(const std::string& key);

		bool dropIndex(const std::string& indexType, const std::string& key);

		std::vector<std::pair<std::string, std::string>> listIndexes();

		// Build all indexes from the current storage
		bool buildIndexes();

		// Rebuild indexes incrementally with changed data
		void updateIndexes(const std::vector<Node> &addedNodes, const std::vector<Node> &updatedNodes,
						   const std::vector<uint64_t> &removedNodeIds, const std::vector<Edge> &addedEdges,
						   const std::vector<Edge> &updatedEdges, const std::vector<uint64_t> &removedEdgeIds);

		// Get specific indexes
		[[nodiscard]] std::shared_ptr<LabelIndex> getLabelIndex() const;
		[[nodiscard]] std::shared_ptr<PropertyIndex> getPropertyIndex() const;
		[[nodiscard]] std::shared_ptr<RelationshipIndex> getRelationshipIndex() const;
		[[nodiscard]] std::shared_ptr<FullTextIndex> getFullTextIndex() const;

		// Find nodes by various criteria
		std::vector<int64_t> findNodeIdsByLabel(const std::string &label) const;
		std::vector<int64_t> findNodeIdsByProperty(const std::string &key, const std::string &value) const;
		std::vector<int64_t> findNodeIdsByPropertyRange(const std::string &key, double minValue, double maxValue) const;
		std::vector<int64_t> findNodeIdsByTextSearch(const std::string &key, const std::string &searchText) const;

		// Relationship queries
		std::vector<int64_t> findEdgeIdsByRelationship(const std::string &label) const;
		std::vector<int64_t> findEdgeIdsByNodes(int64_t fromNodeId, int64_t toNodeId) const;
		std::vector<int64_t> findOutgoingEdgeIds(int64_t nodeId, const std::string &label = "") const;
		std::vector<int64_t> findIncomingEdgeIds(int64_t nodeId, const std::string &label = "") const;

		void persistState() const;

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<LabelIndex> labelIndex_;
		std::shared_ptr<PropertyIndex> propertyIndex_;
		std::shared_ptr<RelationshipIndex> relationshipIndex_;
		std::shared_ptr<FullTextIndex> fullTextIndex_;
	};

} // namespace graph::query::indexes
