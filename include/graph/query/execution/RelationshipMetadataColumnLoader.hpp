#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	struct RelationshipMetadataBatch {
		std::vector<int64_t> edgeIds;
		std::vector<int64_t> sourceNodeIds;
		std::vector<int64_t> targetNodeIds;
		std::vector<int64_t> typeIds;
		std::vector<int64_t> propertyEntityIds;
		std::vector<PropertyStorageType> propertyStorageTypes;
		std::vector<uint8_t> active;

		[[nodiscard]] size_t size() const { return edgeIds.size(); }
		void reserve(size_t rowCount);
		void appendDefault();
		void setFromEdge(size_t row, const Edge &edge);
		[[nodiscard]] bool isValid(size_t row) const { return row < edgeIds.size() && edgeIds[row] != 0; }
		[[nodiscard]] Edge toEdge(size_t row) const;
	};

	struct RelationshipPropertyCandidateBatch {
		std::vector<int64_t> edgeIds;
		std::vector<int64_t> propertyEntityIds;
		std::vector<size_t> propertyRows;
		std::vector<size_t> fallbackRows;

		[[nodiscard]] size_t size() const { return edgeIds.size(); }
		void reserve(size_t rowCount);
	};

	struct RelationshipPropertyCountCandidates {
		std::vector<int64_t> propertyEntityIds;
		std::vector<int64_t> propertyEdgeIds;
		std::vector<int64_t> fallbackEdgeIds;
		size_t matchedEdges = 0;

		void reserve(size_t rowCount);
	};

	struct RelationshipPropertyCountCandidateOptions {
		bool collectPropertyEdgeRefs = true;
	};

	class RelationshipMetadataColumnLoader {
	public:
		explicit RelationshipMetadataColumnLoader(std::shared_ptr<storage::DataManager> dm);

		[[nodiscard]] std::optional<RelationshipMetadataBatch> loadRange(int64_t beginId, int64_t endId) const;
		[[nodiscard]] std::optional<int64_t> countActiveByType(int64_t beginId, int64_t endId, int64_t typeId) const;
		[[nodiscard]] std::optional<int64_t> countActiveByType(int64_t beginId,
		                                                       int64_t endId,
		                                                       int64_t typeId,
		                                                       concurrent::ThreadPool *threadPool) const;
		[[nodiscard]] std::optional<RelationshipPropertyCandidateBatch>
		collectPropertyCandidatesByType(int64_t beginId, int64_t endId, int64_t typeId) const;
		[[nodiscard]] std::optional<RelationshipPropertyCountCandidates>
		collectPropertyCountCandidatesByType(int64_t beginId, int64_t endId, int64_t typeId,
											 RelationshipPropertyCountCandidateOptions options = {}) const;
		[[nodiscard]] std::optional<RelationshipPropertyCountCandidates>
		collectPropertyCountCandidatesByType(int64_t beginId,
		                                     int64_t endId,
		                                     int64_t typeId,
		                                     concurrent::ThreadPool *threadPool,
		                                     RelationshipPropertyCountCandidateOptions options = {}) const;

	private:
		[[nodiscard]] bool canLoad(int64_t beginId, int64_t endId) const;
		[[nodiscard]] bool canCountActiveByType(int64_t beginId, int64_t endId) const;

		std::shared_ptr<storage::DataManager> dm_;
	};

} // namespace graph::query::execution
