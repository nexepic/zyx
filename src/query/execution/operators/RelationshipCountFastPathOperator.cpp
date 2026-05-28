#include "graph/query/execution/operators/RelationshipCountFastPathOperator.hpp"

#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"

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
		                                    std::move(outputAlias)) {}

	RelationshipCountFastPathOperator::RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
	                                                                   std::shared_ptr<indexes::IndexManager> im,
	                                                                   NodeScanConfig seedConfig,
	                                                                   NodeScanRequirements seedRequirements,
	                                                                   std::vector<VectorizedPropertyPredicate> seedPredicates,
	                                                                   std::vector<RelationshipExpandConfig> hops,
	                                                                   std::string outputAlias)
		: dm_(std::move(dm)),
		  im_(std::move(im)),
		  seedConfig_(std::move(seedConfig)),
		  seedRequirements_(std::move(seedRequirements)),
		  seedPredicates_(std::move(seedPredicates)),
		  hops_(std::move(hops)),
		  outputAlias_(std::move(outputAlias)) {}

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
		    seedRequirements_.materialization == NodeMaterializationMode::NSM_ID_ONLY &&
		    (!seedRequirements_.needsActiveCheck || candidateSet.activeOnly) &&
		    (!seedRequirements_.needsLabels || candidateSet.labelsSatisfied)) {
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
			if (batch.selected.empty() || batch.selected[i] != 0) {
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
			for (size_t i = 0; i < batch.rows.size(); ++i) {
				if (batch.selected.empty() || batch.selected[i] != 0) {
					current.push_back(batch.rows[i].targetId);
				}
			}
			if (current.empty()) {
				break;
			}
		}
		return count;
	}

	std::optional<RecordBatch> RelationshipCountFastPathOperator::next() {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;

		const auto countStart = Clock::now();
		const auto seedIds = collectSeedIds();
		const int64_t count = seedIds.empty() || hops_.empty() ? 0 : countExpandedPaths(seedIds);
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
