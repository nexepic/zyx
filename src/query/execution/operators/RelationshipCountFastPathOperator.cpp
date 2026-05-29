#include "graph/query/execution/operators/RelationshipCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/RelationshipCandidateSource.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::execution::operators {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void addProfile(const char *phase, Clock::time_point start) {
		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration(phase, elapsedNs(start));
		}
	}

	void appendRowsMissingFromBulkLoad(const std::vector<size_t> &externalRows,
	                                   std::vector<size_t> loadedRows,
	                                   std::vector<size_t> &fallbackRows) {
		std::sort(loadedRows.begin(), loadedRows.end());
		loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
		for (const size_t row : externalRows) {
			if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) { // ZYX_COV_EXCL_LINE
				fallbackRows.push_back(row);
			}
		}
	}

	bool propertyMapMatches(const std::unordered_map<std::string, PropertyValue> &properties,
	                        const std::unordered_map<std::string, PropertyValue> &expected) {
		for (const auto &[key, value] : expected) {
			const auto it = properties.find(key);
			if (it == properties.end() || it->second != value) { // ZYX_COV_EXCL_LINE
				return false;
			}
		}
		return true;
	}

	bool keysCoverAllPredicates(const std::vector<std::string> &coveredKeys,
	                            const std::unordered_map<std::string, PropertyValue> &expected) {
		if (coveredKeys.size() != expected.size()) {
			return false;
		}
		for (const auto &key : coveredKeys) {
			if (!expected.contains(key)) {
				return false;
			}
		}
		return true;
	}
} // namespace

	RelationshipCountFastPathOperator::RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   std::string outputAlias)
		: RelationshipCountFastPathOperator(std::move(dm),
		                                    std::move(im),
		                                    std::move(seedConfig),
		                                    std::move(seedRequirements),
		                                    {},
		                                    std::move(hops),
		                                    {},
		                                    std::move(outputAlias)) {}

	RelationshipCountFastPathOperator::RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<VectorizedPropertyPredicate> seedPredicates,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   DirectRelationshipCountConfig directCount,
	                                                                   std::string outputAlias)
		: dm_(std::move(dm)),
		  im_(std::move(im)),
		  seedConfig_(std::move(seedConfig)),
		  seedRequirements_(std::move(seedRequirements)),
		  seedPredicates_(std::move(seedPredicates)),
		  hops_(std::move(hops)),
		  directCount_(std::move(directCount)),
		  outputAlias_(std::move(outputAlias)) {}

	RelationshipCountFastPathOperator::RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<VectorizedPropertyPredicate> seedPredicates,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   std::string outputAlias)
		: RelationshipCountFastPathOperator(std::move(dm),
		                                    std::move(im),
		                                    std::move(seedConfig),
		                                    std::move(seedRequirements),
		                                    std::move(seedPredicates),
		                                    std::move(hops),
		                                    {},
		                                    std::move(outputAlias)) {}

	void RelationshipCountFastPathOperator::open() {
		emitted_ = false;
		if (!dm_) {
			return;
		}
		for (auto &hop : hops_) {
			hop.edgeTypeId = 0;
			if (!hop.edgeType.empty()) {
				hop.edgeTypeId = dm_->resolveTokenId(hop.edgeType);
				if (hop.edgeTypeId == 0) {
					hop.edgeTypeId = -1;
				}
			}

			hop.targetLabelIds.clear();
			hop.targetLabelIds.reserve(hop.targetLabels.size());
			for (const auto &label : hop.targetLabels) {
				const int64_t labelId = dm_->resolveTokenId(label);
				hop.targetLabelIds.push_back(labelId == 0 ? -1 : labelId);
			}
		}
	}

	std::vector<int64_t> RelationshipCountFastPathOperator::collectSeedIds() const {
		const auto candidateStart = Clock::now();
		NodeCandidateSource source(dm_, im_);
		const auto candidateSet = source.collectWithMetadata(seedConfig_);
		const auto &candidates = candidateSet.ids;
		addProfile("relationship_expand.seed_candidates", candidateStart);

		if (seedPredicates_.empty() &&
		    seedRequirements_.materialization == NodeMaterializationMode::NSM_ID_ONLY && // ZYX_COV_EXCL_LINE
		    (!seedRequirements_.needsActiveCheck || candidateSet.activeOnly) && // ZYX_COV_EXCL_LINE
		    (!seedRequirements_.needsLabels || candidateSet.labelsSatisfied)) { // ZYX_COV_EXCL_LINE
			return candidates;
		}

		const auto loadStart = Clock::now();
		NodeBatchLoader loader(dm_, threadPool_);
		NodeScanRequirements requirements = seedRequirements_;
		requirements.countOnly = true;
		auto batch = loader.load(candidates, 0, candidates.size(), seedConfig_, requirements);
		applyPredicates(batch, seedPredicates_);
		addProfile("relationship_expand.seed_load", loadStart);

		std::vector<int64_t> ids;
		ids.reserve(batch.nodeIds.size());
		for (size_t i = 0; i < batch.nodeIds.size(); ++i) {
			if (batch.selected[i] != 0) {
				ids.push_back(batch.nodeIds[i]);
			}
		}
		return ids;
	}

	int64_t RelationshipCountFastPathOperator::countExpandedPaths(const std::vector<int64_t> &seedIds) const {
		RelationshipAdjacencyCursor cursor(dm_);
		std::vector<int64_t> current = seedIds;
		int64_t count = 0;
		for (size_t hopIndex = 0; hopIndex < hops_.size(); ++hopIndex) {
			const auto edgeStart = Clock::now();
			const auto batch = cursor.expand(current, hops_[hopIndex], RelationshipExpandRequirements{});
			addProfile("relationship_expand.edges", edgeStart);
			addProfile("relationship_expand.target_check", edgeStart);

			if (hopIndex + 1 == hops_.size()) {
				count += static_cast<int64_t>(batch.selectedCount());
				break;
			}

			current.clear();
			current.reserve(batch.rows.size());
			for (const auto &row : batch.rows) {
				current.push_back(row.targetId);
			}
			if (current.empty()) {
				break;
			}
		}
		return count;
	}

	int64_t RelationshipCountFastPathOperator::countDirectRelationships() const {
		if (!dm_) {
			return 0;
		}

		const int64_t edgeTypeId = directCount_.edgeType.empty() ? 0 : dm_->resolveTokenId(directCount_.edgeType);
		if (!directCount_.edgeType.empty() && edgeTypeId == 0) {
			return 0;
		}
		if (auto indexedCount = countDirectRelationshipsFromIndexes(edgeTypeId)) {
			return *indexedCount;
		}

		const auto scanStart = Clock::now();
		int64_t count = 0;
		const int64_t maxId = dm_->getIdAllocator(EntityType::Edge)->getCurrentMaxId();
		const bool usedDirectDiskLoad = !dm_->hasUnsavedChanges() && dm_->hasPreadSupport(); // ZYX_COV_EXCL_LINE

		if (usedDirectDiskLoad) {
			RelationshipMetadataColumnLoader metadataLoader(dm_);
			if (directCount_.edgeProperties.empty()) {
				if (auto directCount = metadataLoader.countActiveByType(1, maxId, edgeTypeId)) {
					addProfile("relationship_count.direct_scan", scanStart);
					return *directCount;
				}
			} else if (auto candidates = metadataLoader.collectPropertyCandidatesByType(1, maxId, edgeTypeId)) {
				const auto propertyStart = Clock::now();
				storage::PropertyEntityPredicateMatchOptions predicateOptions;
				predicateOptions.collectLoadedRows = false;
				predicateOptions.collectMatchedRows = false;
				auto predicateResult = dm_->bulkMatchPropertyEntityPredicates(
					candidates->propertyEntityIds,
					candidates->propertyRows,
					candidates->size(),
					directCount_.edgeProperties,
					threadPool_,
					predicateOptions);
				if (predicateResult.loadedCount != candidates->propertyRows.size()) {
					predicateOptions.collectLoadedRows = true;
					predicateResult = dm_->bulkMatchPropertyEntityPredicates(
						candidates->propertyEntityIds,
						candidates->propertyRows,
						candidates->size(),
						directCount_.edgeProperties,
						threadPool_,
						predicateOptions);
				}
				addProfile("relationship_count.property_predicate", propertyStart);
				count += static_cast<int64_t>(predicateResult.matchedCount);

				std::vector<size_t> fallbackRows = candidates->fallbackRows;
				// The common clean-storage path loads every property entity, so avoid
				// sorting all rows just to prove there is no fallback work.
				if (predicateResult.loadedCount != candidates->propertyRows.size()) {
					appendRowsMissingFromBulkLoad(candidates->propertyRows, std::move(predicateResult.loadedRows), fallbackRows);
				}
				if (!fallbackRows.empty()) {
					std::sort(fallbackRows.begin(), fallbackRows.end());
					fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
					for (const size_t row : fallbackRows) {
						count += propertyMapMatches(dm_->getEdgeProperties(candidates->edgeIds[row]), directCount_.edgeProperties)
							? int64_t{1}
							: int64_t{0};
					}
				}

				addProfile("relationship_count.direct_scan", scanStart);
				return count;
			}
		}

		auto loadEdgesWithEntityManager = [&]() {
			std::vector<Edge> loaded;
			static constexpr size_t kBatchSize = 4096;
			for (int64_t startId = 1; startId <= maxId; startId += static_cast<int64_t>(kBatchSize)) {
				const int64_t endId = std::min<int64_t>(maxId, startId + static_cast<int64_t>(kBatchSize) - 1);
				auto batch = dm_->getEdgesInRange(startId, endId, static_cast<size_t>(endId - startId + 1));
				loaded.insert(loaded.end(), batch.begin(), batch.end());
			}
			return loaded;
		};

		std::vector<Edge> edges;
		if (usedDirectDiskLoad) {
			edges = dm_->bulkLoadEntities<Edge>(1, maxId);
		} else {
			edges = loadEdgesWithEntityManager();
		}

		std::vector<uint8_t> selected;
		std::vector<size_t> candidateRows;
		candidateRows.reserve(edges.size());
		if (!directCount_.edgeProperties.empty()) {
			selected.assign(edges.size(), 0);
		}

		for (size_t row = 0; row < edges.size(); ++row) {
			const auto &edge = edges[row];
			if (edgeTypeId != 0 && edge.getTypeId() != edgeTypeId) {
				continue;
			}
			if (directCount_.edgeProperties.empty()) {
				++count;
				continue;
			}
			selected[row] = 1;
			candidateRows.push_back(row);
		}

		if (!directCount_.edgeProperties.empty() && !candidateRows.empty()) {
			std::vector<std::string> propertyKeys;
			propertyKeys.reserve(directCount_.edgeProperties.size());
			for (const auto &entry : directCount_.edgeProperties) {
				propertyKeys.push_back(entry.first);
			}

			const auto propertyStart = Clock::now();
			RelationshipPropertyColumnLoader loader(dm_, threadPool_);
			const auto columns = loader.loadColumns(edges, selected, propertyKeys);
			addProfile("relationship_count.property_columns", propertyStart);

			for (const size_t row : candidateRows) {
				if (edgeMatchesPropertyColumns(columns, row)) {
					++count;
				}
			}
		}
		addProfile("relationship_count.direct_scan", scanStart);
		return count;
	}

	std::optional<int64_t> RelationshipCountFastPathOperator::countDirectRelationshipsFromIndexes(
			int64_t edgeTypeId) const {
		if (!im_) {
			return std::nullopt;
		}

		const auto candidateStart = Clock::now();
		RelationshipCandidateSource source(dm_, im_);
		auto candidates = source.collect(directCount_);
		if (!candidates.available) {
			return std::nullopt;
		}
		addProfile("relationship_count.index_candidates", candidateStart);

		if (candidates.ids.empty()) {
			return int64_t{0};
		}

		const bool typeNeedsVerification = edgeTypeId != 0 && !candidates.typeSatisfied;
		const bool propertiesNeedVerification =
				!directCount_.edgeProperties.empty() &&
				!keysCoverAllPredicates(candidates.propertyKeysSatisfied, directCount_.edgeProperties);
		if (!typeNeedsVerification && !propertiesNeedVerification) {
			return static_cast<int64_t>(candidates.ids.size());
		}

		const auto filterStart = Clock::now();
		int64_t count = 0;
		for (const int64_t edgeId : candidates.ids) {
			if (typeNeedsVerification) {
				const Edge edge = dm_->getEdge(edgeId);
				if (edge.getId() == 0 || !edge.isActive() || edge.getTypeId() != edgeTypeId) {
					continue;
				}
			}
			if (propertiesNeedVerification &&
			    !propertyMapMatches(dm_->getEdgeProperties(edgeId), directCount_.edgeProperties)) {
				continue;
			}
			++count;
		}
		addProfile("relationship_count.index_filter", filterStart);
		return count;
	}

	bool RelationshipCountFastPathOperator::edgeMatchesPropertyColumns(
			const std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
			size_t row) const {
		for (const auto &[key, value] : directCount_.edgeProperties) {
			const auto columnIt = columns.find(key);
			// RelationshipPropertyColumnLoader creates and sizes every requested column to the edge batch.
			const auto &cell = columnIt->second[row];
			if (!cell.has_value() || cell.value() != value) {
				return false;
			}
		}
		return true;
	}

	std::optional<RecordBatch> RelationshipCountFastPathOperator::next() {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;

		const auto countStart = Clock::now();
		const int64_t count = directCount_.enabled
			? countDirectRelationships()
			: [&]() {
				const auto seedIds = collectSeedIds();
				return seedIds.empty() || hops_.empty() ? int64_t{0} : countExpandedPaths(seedIds);
			}();
		addProfile("relationship_expand.count", countStart);

		Record record;
		record.setValue(outputAlias_, PropertyValue(count));
		RecordBatch batch;
		batch.push_back(std::move(record));
		return batch;
	}

	void RelationshipCountFastPathOperator::close() {}

	std::string RelationshipCountFastPathOperator::toString() const {
		return "RelationshipCountFastPath(" + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
