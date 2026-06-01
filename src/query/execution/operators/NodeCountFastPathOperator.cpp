#include "graph/query/execution/operators/NodeCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"

namespace graph::query::execution::operators {

	NodeCountFastPathOperator::NodeCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
														 std::shared_ptr<indexes::IndexManager> im,
														 NodeScanConfig config, NodeScanRequirements requirements,
														 std::vector<VectorizedPropertyPredicate> predicates,
														 std::string outputAlias) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), outputAlias_(std::move(outputAlias)) {}

	void NodeCountFastPathOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = NodeCandidateSet{};
		directCount_.reset();
		const bool canUseIndexCount = requirements_.countOnly &&
									  requirements_.materialization == NodeMaterializationMode::NSM_ID_ONLY &&
									  predicates_.empty();
		if (canUseIndexCount) {
			auto candidateCount = source.countWithMetadata(config_);
			const bool satisfiesRequirements = candidateCount.available &&
											   (!requirements_.needsActiveCheck || candidateCount.activeOnly) &&
											   (!requirements_.needsLabels || candidateCount.labelsSatisfied);
			if (satisfiesRequirements) {
				directCount_ = candidateCount.count;
				emitted_ = false;
				return;
			}
		}

		candidateSet_ = source.collectWithMetadata(config_);
		emitted_ = false;
	}

	std::optional<RecordBatch> NodeCountFastPathOperator::next() {
		using Clock = std::chrono::steady_clock;
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;

		const auto start = Clock::now();
		int64_t count = 0;
		const bool canCountCandidatesDirectly =
				requirements_.countOnly && requirements_.materialization == NodeMaterializationMode::NSM_ID_ONLY &&
				predicates_.empty() && (!requirements_.needsActiveCheck || candidateSet_.activeOnly) &&
				(!requirements_.needsLabels || candidateSet_.labelsSatisfied);

		if (directCount_.has_value()) {
			count = *directCount_;
		} else if (canCountCandidatesDirectly) {
			count = static_cast<int64_t>(candidateSet_.ids.size());
		} else {
			bool countedByPredicateKernel = false;
			if (!predicates_.empty() && requirements_.countOnly &&
				requirements_.materialization == NodeMaterializationMode::NSM_SELECTED_PROPERTIES) {
				NodeColumnarPredicateCounter counter(dm_, threadPool_);
				const auto predicateCount =
						counter.count(candidateSet_.ids, candidateSet_, config_, requirements_, predicates_);
				if (predicateCount.available) {
					count = predicateCount.count;
					countedByPredicateKernel = true;
				}
			}

			if (!countedByPredicateKernel) {
				NodeBatchLoader loader(dm_, threadPool_);
				const auto requirements = relaxSatisfiedCandidateChecks(requirements_, candidateSet_);
				for (size_t begin = 0; begin < candidateSet_.ids.size();) {
					const size_t batchSize = chooseColumnarNodeBatchSize(candidateSet_.ids.size() - begin, threadPool_,
																		 PhysicalOperator::DEFAULT_BATCH_SIZE);
					const size_t end = begin + batchSize;
					auto batch = loader.load(candidateSet_.ids, begin, end, config_, requirements);
					applyPredicates(batch, predicates_);
					count += static_cast<int64_t>(batch.selectedCount());
					begin = end;
				}
			}
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration(
					"node_scan.count",
					static_cast<uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
		}

		Record record;
		record.setValue(outputAlias_, PropertyValue(count));
		RecordBatch output;
		output.push_back(std::move(record));
		return output;
	}

	void NodeCountFastPathOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		directCount_.reset();
		emitted_ = false;
	}

	std::vector<std::string> NodeCountFastPathOperator::getOutputVariables() const { return {outputAlias_}; }

	std::string NodeCountFastPathOperator::toString() const {
		return "NodeCountFastPath(" + config_.variable + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
