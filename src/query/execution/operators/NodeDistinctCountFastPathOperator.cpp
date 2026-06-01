#include "graph/query/execution/operators/NodeDistinctCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"

namespace graph::query::execution::operators {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}
	} // namespace

	NodeDistinctCountFastPathOperator::NodeDistinctCountFastPathOperator(
			std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im, NodeScanConfig config,
			NodeScanRequirements requirements, std::vector<VectorizedPropertyPredicate> predicates,
			std::string distinctProperty, std::string outputAlias) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), distinctProperty_(std::move(distinctProperty)),
		outputAlias_(std::move(outputAlias)) {}

	void NodeDistinctCountFastPathOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = NodeCandidateSet{};
		candidateSet_ = source.collectWithMetadata(config_);
		emitted_ = false;
	}

	std::optional<RecordBatch> NodeDistinctCountFastPathOperator::next() {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;

		const auto start = Clock::now();

		std::unordered_set<std::string> seen;
		NodeBatchLoader loader(dm_, threadPool_);
		const auto requirements = relaxSatisfiedCandidateChecks(requirements_, candidateSet_);
		for (size_t begin = 0; begin < candidateSet_.ids.size();) {
			if (queryContext_) {
				queryContext_->checkGuard();
			}
			const size_t batchSize = chooseColumnarNodeBatchSize(candidateSet_.ids.size() - begin, threadPool_,
																 PhysicalOperator::DEFAULT_BATCH_SIZE);
			const size_t end = begin + batchSize;
			auto batch = loader.load(candidateSet_.ids, begin, end, config_, requirements);
			applyPredicates(batch, predicates_);

			auto columnIt = batch.propertyColumns.find(distinctProperty_);
			if (columnIt == batch.propertyColumns.end()) {
				continue;
			}
			const auto &column = columnIt->second;
			for (size_t row = 0; row < batch.nodeIds.size(); ++row) {
				if (row >= batch.selected.size() || batch.selected[row] == 0 || row >= column.size() ||
					!column[row].has_value() || expressions::EvaluationContext::isNull(*column[row])) {
					continue;
				}
				seen.insert(column[row]->toString());
			}
			begin = end;
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.distinct_count", elapsedNs(start));
		}

		const auto count = static_cast<int64_t>(seen.size());
		Record record;
		record.setValue(outputAlias_, PropertyValue(count));
		RecordBatch output;
		output.push_back(std::move(record));
		return output;
	}

	void NodeDistinctCountFastPathOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		emitted_ = false;
	}

	std::vector<std::string> NodeDistinctCountFastPathOperator::getOutputVariables() const { return {outputAlias_}; }

	std::string NodeDistinctCountFastPathOperator::toString() const {
		return "NodeDistinctCountFastPath(" + config_.variable + "." + distinctProperty_ + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
