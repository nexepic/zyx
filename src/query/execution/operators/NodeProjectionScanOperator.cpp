#include "graph/query/execution/operators/NodeProjectionScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"

namespace graph::query::execution::operators {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void addRequiredProperty(NodeScanRequirements &requirements, const std::string &property) {
		if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), property) ==
		    requirements.requiredProperties.end()) {
			requirements.requiredProperties.push_back(property);
		}
	}
} // namespace

NodeProjectionScanOperator::NodeProjectionScanOperator(
		std::shared_ptr<storage::DataManager> dm,
		std::shared_ptr<indexes::IndexManager> im,
		NodeScanConfig config,
		NodeScanRequirements requirements,
		std::vector<VectorizedPropertyPredicate> predicates,
		std::vector<NodeProjectionScanItem> projections,
		std::optional<size_t> limit,
		std::vector<ExplainAttribute> explainAttributes)
	: dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
	  predicates_(std::move(predicates)), projections_(std::move(projections)), limit_(limit),
	  explainAttributes_(std::move(explainAttributes)) {
	requirements_.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	for (const auto &projection : projections_) {
		addRequiredProperty(requirements_, projection.property);
	}
	for (const auto &predicate : predicates_) {
		addRequiredProperty(requirements_, predicate.propertyKey);
	}
}

void NodeProjectionScanOperator::open() {
	candidateSet_ = NodeCandidateSet{};
	currentIdx_ = 0;
	emittedRows_ = 0;
	NodeCandidateSource source(dm_, im_);
	candidateSet_ = source.collectWithMetadata(config_);
}

std::optional<RecordBatch> NodeProjectionScanOperator::next() {
	const bool traceEnabled = debug::PerfTrace::isEnabled();
	const auto start = traceEnabled ? Clock::now() : Clock::time_point{};
	if (remainingLimit() == 0 || currentIdx_ >= candidateSet_.ids.size()) {
		return std::nullopt;
	}

	NodeBatchLoader loader(dm_, threadPool_);
	PropertyPredicateKernel predicateKernel(predicates_);
	const auto requirements = relaxSatisfiedCandidateChecks(requirements_, candidateSet_);

	while (currentIdx_ < candidateSet_.ids.size() && remainingLimit() > 0) {
		if (queryContext_) {
			queryContext_->checkGuard();
		}
		const size_t begin = currentIdx_;
		const size_t end = std::min(candidateSet_.ids.size(), begin + nextBatchSize());
		currentIdx_ = end;

		auto batch = loader.load(candidateSet_.ids, begin, end, config_, requirements);
		predicateKernel.apply(batch);

		RecordBatch output;
		output.reserve(std::min(batch.selectedCount(), remainingLimit()));
		for (size_t row = 0; row < batch.nodeIds.size() && output.size() < remainingLimit(); ++row) {
			if (!batch.isSelected(row)) {
				continue;
			}
			output.push_back(makeRecord(batch, row));
		}
		emittedRows_ += output.size();

		if (!output.empty()) {
			if (traceEnabled) {
				debug::PerfTrace::addDuration("node_projection_scan", elapsedNs(start));
			}
			return output;
		}
	}

	if (traceEnabled) {
		debug::PerfTrace::addDuration("node_projection_scan", elapsedNs(start));
	}
	return std::nullopt;
}

void NodeProjectionScanOperator::close() {
	candidateSet_ = NodeCandidateSet{};
	currentIdx_ = 0;
	emittedRows_ = 0;
}

void NodeProjectionScanOperator::setOutputLimitHint(size_t limit) {
	if (!limit_.has_value() || limit < *limit_) {
		limit_ = limit;
	}
}

std::vector<std::string> NodeProjectionScanOperator::getOutputVariables() const {
	std::vector<std::string> variables;
	variables.reserve(projections_.size());
	for (const auto &projection : projections_) {
		variables.push_back(projection.alias);
	}
	return variables;
}

std::string NodeProjectionScanOperator::toString() const {
	std::ostringstream oss;
	oss << "NodeProjectionScan(" << config_.variable;
	if (limit_.has_value()) {
		oss << " LIMIT " << *limit_;
	}
	oss << ")";
	return oss.str();
}

size_t NodeProjectionScanOperator::remainingLimit() const {
	if (!limit_.has_value()) {
		return std::numeric_limits<size_t>::max();
	}
	return emittedRows_ < *limit_ ? *limit_ - emittedRows_ : 0;
}

size_t NodeProjectionScanOperator::nextBatchSize() const {
	const size_t remainingCandidates = candidateSet_.ids.size() - currentIdx_;
	if (remainingCandidates == 0) {
		return 0;
	}
	const size_t limitWindow = limit_.has_value()
			? std::max<size_t>(remainingLimit(), DEFAULT_BATCH_SIZE / 4)
			: chooseColumnarNodeBatchSize(remainingCandidates, threadPool_, DEFAULT_BATCH_SIZE);
	return std::min(remainingCandidates, limitWindow);
}

Record NodeProjectionScanOperator::makeRecord(const NodeColumnBatch &batch, size_t row) const {
	Record record;
	for (const auto &projection : projections_) {
		PropertyValue value;
		if (auto columnIt = batch.propertyColumns.find(projection.property);
		    columnIt != batch.propertyColumns.end() && row < columnIt->second.size() && columnIt->second[row].has_value()) {
			value = *columnIt->second[row];
		}
		record.setValue(projection.alias, std::move(value));
	}
	return record;
}

} // namespace graph::query::execution::operators
