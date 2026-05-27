#include "graph/query/execution/operators/NodeCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution::operators {

	NodeCountFastPathOperator::NodeCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
	                                                       std::shared_ptr<indexes::IndexManager> im,
	                                                       NodeScanConfig config,
	                                                       NodeScanRequirements requirements,
	                                                       std::vector<VectorizedPropertyPredicate> predicates,
	                                                       std::string outputAlias)
		: dm_(std::move(dm)),
		  im_(std::move(im)),
		  config_(std::move(config)),
		  requirements_(std::move(requirements)),
		  predicates_(std::move(predicates)),
		  outputAlias_(std::move(outputAlias)) {}

	void NodeCountFastPathOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidates_ = source.collect(config_);
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
		NodeBatchLoader loader(dm_, threadPool_);

		for (size_t begin = 0; begin < candidates_.size(); begin += PhysicalOperator::DEFAULT_BATCH_SIZE) {
			const size_t end = std::min(begin + PhysicalOperator::DEFAULT_BATCH_SIZE, candidates_.size());
			auto batch = loader.load(candidates_, begin, end, config_, requirements_);
			applyPredicates(batch, predicates_);
			count += static_cast<int64_t>(batch.selectedCount());
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration(
					"node_scan.count",
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
		}

		Record record;
		record.setValue(outputAlias_, PropertyValue(count));
		RecordBatch output;
		output.push_back(std::move(record));
		return output;
	}

	void NodeCountFastPathOperator::close() {
		candidates_.clear();
		emitted_ = false;
	}

	std::vector<std::string> NodeCountFastPathOperator::getOutputVariables() const {
		return {outputAlias_};
	}

	std::string NodeCountFastPathOperator::toString() const {
		return "NodeCountFastPath(" + config_.variable + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
