#include "graph/query/execution/operators/RelationshipCountScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/query/execution/RelationshipCandidateSource.hpp"
#include "graph/query/execution/RelationshipColumnarCountKernel.hpp"
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

	std::vector<VectorizedPropertyPredicate> effectiveEdgePredicates(const DirectRelationshipCountConfig &config) {
		if (!config.edgePredicates.empty()) {
			return config.edgePredicates;
		}
		return PropertyPredicateKernel::fromEqualityPredicates(config.edgeProperties).predicates();
	}

	std::vector<std::string> predicateKeys(const std::vector<VectorizedPropertyPredicate> &predicates) {
		std::vector<std::string> keys;
		keys.reserve(predicates.size());
		for (const auto &predicate : predicates) {
			if (std::find(keys.begin(), keys.end(), predicate.propertyKey) == keys.end()) {
				keys.push_back(predicate.propertyKey);
			}
		}
		return keys;
	}

	bool keysCoverAllPredicates(const std::vector<std::string> &coveredKeys,
	                            const std::vector<VectorizedPropertyPredicate> &predicates) {
		const auto keys = predicateKeys(predicates);
		if (coveredKeys.size() != keys.size() || predicates.size() != keys.size()) {
			return false;
		}
		for (const auto &predicate : predicates) {
			if (predicate.op != VectorPredicateOp::VPO_EQ ||
			    std::find(coveredKeys.begin(), coveredKeys.end(), predicate.propertyKey) == coveredKeys.end()) {
				return false;
			}
		}
		return true;
	}
} // namespace

	RelationshipCountScanOperator::RelationshipCountScanOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   std::string outputAlias,
	                                                                   std::vector<ExplainAttribute> explainAttributes)
		: RelationshipCountScanOperator(std::move(dm),
		                                    std::move(im),
		                                    std::move(seedConfig),
		                                    std::move(seedRequirements),
		                                    {},
		                                    std::move(hops),
		                                    {},
		                                    std::move(outputAlias),
		                                    std::move(explainAttributes)) {}

	RelationshipCountScanOperator::RelationshipCountScanOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<VectorizedPropertyPredicate> seedPredicates,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   DirectRelationshipCountConfig directCount,
	                                                                   std::string outputAlias,
	                                                                   std::vector<ExplainAttribute> explainAttributes)
		: dm_(std::move(dm)),
		  im_(std::move(im)),
		  seedConfig_(std::move(seedConfig)),
		  seedRequirements_(std::move(seedRequirements)),
		  seedPredicates_(std::move(seedPredicates)),
		  hops_(std::move(hops)),
		  directCount_(std::move(directCount)),
		  outputAlias_(std::move(outputAlias)),
		  explainAttributes_(std::move(explainAttributes)) {}

	RelationshipCountScanOperator::RelationshipCountScanOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<VectorizedPropertyPredicate> seedPredicates,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   std::string outputAlias,
	                                                                   std::vector<ExplainAttribute> explainAttributes)
		: RelationshipCountScanOperator(std::move(dm),
		                                    std::move(im),
		                                    std::move(seedConfig),
		                                    std::move(seedRequirements),
		                                    std::move(seedPredicates),
		                                    std::move(hops),
		                                    {},
		                                    std::move(outputAlias),
		                                    std::move(explainAttributes)) {}

	void RelationshipCountScanOperator::open() {
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

	std::vector<int64_t> RelationshipCountScanOperator::collectSeedIds() const {
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

	int64_t RelationshipCountScanOperator::countExpandedPaths(const std::vector<int64_t> &seedIds) const {
		RelationshipAdjacencyCursor cursor(dm_, threadPool_);
		std::vector<int64_t> current = seedIds;
		int64_t count = 0;
		for (size_t hopIndex = 0; hopIndex < hops_.size(); ++hopIndex) {
			const auto &hop = hops_[hopIndex];
			const bool finalHop = hopIndex + 1 == hops_.size();
			RelationshipExpandRequirements requirements;
			requirements.countOnly = finalHop;
			requirements.materialization = finalHop
					? RelationshipMaterializationMode::RMM_ID_ONLY
					: RelationshipMaterializationMode::RMM_EDGE_AND_TARGET;
			requirements.needsTargetLabels = !hop.targetLabelIds.empty();
			// Relationship adjacency lists are maintained transactionally: deleting a node
			// marks connected relationships inactive. Count-only traversal can therefore
			// avoid loading target nodes unless a target label predicate needs them.
			requirements.needsTargetActiveCheck = requirements.needsTargetLabels;

			const auto edgeStart = Clock::now();
			if (finalHop) {
				count += cursor.count(current, hop, requirements);
			} else {
				auto expanded = cursor.expand(current, hop, requirements);
				std::vector<int64_t> next;
				next.reserve(expanded.rows.size());
				for (const auto &row: expanded.rows) {
					next.push_back(row.targetId);
				}
				current = std::move(next);
			}
			addProfile("relationship_expand.edges", edgeStart);
			addProfile("relationship_expand.target_check", edgeStart);

			if (current.empty()) {
				break;
			}
		}
		return count;
	}

	int64_t RelationshipCountScanOperator::countDirectRelationships() const {
		if (!dm_) {
			return 0;
		}

		const int64_t edgeTypeId = directCount_.edgeType.empty() ? 0 : dm_->resolveTokenId(directCount_.edgeType);
		if (!directCount_.edgeType.empty() && edgeTypeId == 0) {
			return 0;
		}

		const int64_t maxId = dm_->getIdAllocator(EntityType::Edge)->getCurrentMaxId();
		const bool hasPropertyFilters = !directCount_.edgeProperties.empty() || !directCount_.edgePredicates.empty();
		const bool hasPlannedIndexCandidate =
				directCount_.candidateSource.type != DirectRelationshipCandidateSourceType::DRCS_AUTO;
		if (hasPlannedIndexCandidate) {
			if (auto indexedCount = countDirectRelationshipsFromIndexes(edgeTypeId)) {
				return *indexedCount;
			}
		}

		if (!hasPropertyFilters) {
			const auto metadataStart = Clock::now();
			if (auto metadataCount = countDirectRelationshipsWithColumnarKernel(edgeTypeId, maxId)) {
				addProfile("relationship_count.direct_scan", metadataStart);
				return *metadataCount;
			}
		}

		if (!hasPlannedIndexCandidate) {
			if (auto indexedCount = countDirectRelationshipsFromIndexes(edgeTypeId)) {
				return *indexedCount;
			}
		}

		const auto scanStart = Clock::now();
		int64_t count = 0;
		const bool usedDirectDiskLoad = !dm_->hasUnsavedChanges() && dm_->hasPreadSupport(); // ZYX_COV_EXCL_LINE

		if (usedDirectDiskLoad) {
			if (auto directCount = countDirectRelationshipsWithColumnarKernel(edgeTypeId, maxId)) {
				addProfile("relationship_count.direct_scan", scanStart);
				return *directCount;
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
		const auto edgePredicates = effectiveEdgePredicates(directCount_);
		if (!edgePredicates.empty()) {
			selected.assign(edges.size(), 0);
		}

		for (size_t row = 0; row < edges.size(); ++row) {
			const auto &edge = edges[row];
			if (edgeTypeId != 0 && edge.getTypeId() != edgeTypeId) {
				continue;
			}
			if (edgePredicates.empty()) {
				++count;
				continue;
			}
			selected[row] = 1;
			candidateRows.push_back(row);
		}

		if (!edgePredicates.empty() && !candidateRows.empty()) {
			const auto propertyKeys = predicateKeys(edgePredicates);

			const auto propertyStart = Clock::now();
			RelationshipPropertyColumnLoader loader(dm_, threadPool_);
			const auto columns = loader.loadColumns(edges, selected, propertyKeys);
			addProfile("relationship_count.property_columns", propertyStart);
			const PropertyPredicateKernel predicateKernel(edgePredicates);

			for (const size_t row : candidateRows) {
				if (edgeMatchesPropertyColumns(columns, row, predicateKernel)) {
					++count;
				}
			}
		}
		addProfile("relationship_count.direct_scan", scanStart);
		return count;
	}

	std::optional<int64_t> RelationshipCountScanOperator::countDirectRelationshipsWithColumnarKernel(
			int64_t edgeTypeId, int64_t maxId) const {
		if (!dm_) {
			return std::nullopt;
		}
		if (maxId <= 0) {
			return int64_t{0};
		}

		RelationshipColumnarCountKernel kernel(dm_, threadPool_);
		RelationshipColumnarCountRequest request;
		request.beginId = 1;
		request.endId = maxId;
		request.typeId = edgeTypeId;
		request.propertyPredicates = directCount_.edgeProperties;
		request.vectorPredicates = directCount_.edgePredicates;
		if (auto directCount = kernel.count(request)) {
			return directCount->count;
		}
		return std::nullopt;
	}

	std::optional<int64_t> RelationshipCountScanOperator::countDirectRelationshipsFromExactIndex(
			int64_t edgeTypeId) const {
		if (!im_) {
			return std::nullopt;
		}

		const auto edgePredicates = effectiveEdgePredicates(directCount_);
		const auto &candidateSource = directCount_.candidateSource;
		if (candidateSource.type == DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX &&
			edgeTypeId != 0 && edgePredicates.empty() && !directCount_.edgeType.empty() &&
			im_->hasLabelIndex("edge")) {
			return static_cast<int64_t>(im_->estimateEdgeIdsByType(directCount_.edgeType));
		}

		if (candidateSource.type != DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX ||
			edgeTypeId != 0 || candidateSource.propertyKeys.size() != 1 ||
			!keysCoverAllPredicates(candidateSource.propertyKeys, edgePredicates)) {
			return std::nullopt;
		}

		const auto &key = candidateSource.propertyKeys.front();
		const auto predicateIt = std::find_if(edgePredicates.begin(), edgePredicates.end(), [&](const auto &predicate) {
			return predicate.propertyKey == key && predicate.op == VectorPredicateOp::VPO_EQ;
		});
		if (predicateIt == edgePredicates.end() || !im_->hasPropertyIndex("edge", key)) {
			return std::nullopt;
		}
		return static_cast<int64_t>(im_->estimateEdgeIdsByProperty(key, predicateIt->value));
	}

	std::optional<int64_t> RelationshipCountScanOperator::countDirectRelationshipsFromIndexes(
			int64_t edgeTypeId) const {
		if (!im_) {
			return std::nullopt;
		}

		const auto candidateStart = Clock::now();
		if (auto exactCount = countDirectRelationshipsFromExactIndex(edgeTypeId)) {
			addProfile("relationship_count.index_count", candidateStart);
			return exactCount;
		}

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
		const auto edgePredicates = effectiveEdgePredicates(directCount_);
		const bool propertiesNeedVerification =
				!edgePredicates.empty() &&
				!keysCoverAllPredicates(candidates.propertyKeysSatisfied, edgePredicates);
		if (!typeNeedsVerification && !propertiesNeedVerification) {
			return static_cast<int64_t>(candidates.ids.size());
		}

		const auto filterStart = Clock::now();
		int64_t count = 0;
		const PropertyPredicateKernel predicateKernel(edgePredicates);
		for (const int64_t edgeId : candidates.ids) {
			if (typeNeedsVerification) {
				const Edge edge = dm_->getEdge(edgeId);
				if (edge.getId() == 0 || !edge.isActive() || edge.getTypeId() != edgeTypeId) {
					continue;
				}
			}
			if (propertiesNeedVerification &&
			    !predicateKernel.matchesMap(dm_->getEdgeProperties(edgeId))) {
				continue;
			}
			++count;
		}
		addProfile("relationship_count.index_filter", filterStart);
		return count;
	}

	bool RelationshipCountScanOperator::edgeMatchesPropertyColumns(
			const std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
			size_t row,
			const PropertyPredicateKernel &predicateKernel) const {
		for (const auto &predicate : predicateKernel.predicates()) {
			const auto columnIt = columns.find(predicate.propertyKey);
			if (columnIt == columns.end() || row >= columnIt->second.size() ||
			    !predicateKernel.matchesValue(columnIt->second[row], predicate)) {
				return false;
			}
		}
		return true;
	}

	std::optional<RecordBatch> RelationshipCountScanOperator::next() {
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

	void RelationshipCountScanOperator::close() {}

	std::string RelationshipCountScanOperator::toString() const {
		return "RelationshipCountScan(" + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
