#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/storage/StorageHeaders.hpp"

namespace graph::storage {

	class DataManager;

	struct RelationshipTypeSegmentStats {
		uint64_t segmentOffset = 0;
		int64_t startId = 0;
		int64_t endId = -1;
		uint32_t used = 0;
		uint32_t inactiveCount = 0;
		int64_t activeCount = 0;
		std::unordered_map<int64_t, int64_t> activeCountByType;
		std::unordered_map<int64_t, std::pair<int64_t, int64_t>> activeIdRangeByType;
		bool hasPropertyCandidates = false;
		std::vector<int64_t> activePropertyEntityIds;
		std::vector<int64_t> activePropertyEdgeIds;
		std::vector<int64_t> activeBlobEdgeIds;
		std::unordered_map<int64_t, std::vector<int64_t>> activePropertyEntityIdsByType;
		std::unordered_map<int64_t, std::vector<int64_t>> activePropertyEdgeIdsByType;
		std::unordered_map<int64_t, std::vector<int64_t>> activeBlobEdgeIdsByType;
	};

	struct RelationshipPropertyCandidateStats {
		std::vector<int64_t> propertyEntityIds;
		std::vector<int64_t> propertyEdgeIds;
		std::vector<int64_t> fallbackEdgeIds;
		size_t matchedEdges = 0;
	};

	class RelationshipSegmentStatsScanner {
	public:
		explicit RelationshipSegmentStatsScanner(const DataManager &dataManager);

		[[nodiscard]] std::optional<RelationshipTypeSegmentStats>
		build(uint64_t segmentOffset, const SegmentHeader &header, bool includePropertyCandidates) const;

		[[nodiscard]] std::optional<int64_t>
		countActiveInWindow(uint64_t segmentOffset, const SegmentHeader &header, int64_t firstId, int64_t lastId,
							int64_t typeId) const;

		[[nodiscard]] std::optional<bool> persistedEdgeMatchesType(int64_t edgeId, int64_t typeId) const;

	private:
		const DataManager &dataManager_;
	};

} // namespace graph::storage
