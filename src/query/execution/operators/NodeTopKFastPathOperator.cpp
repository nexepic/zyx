#include "graph/query/execution/operators/NodeTopKFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/NodePropertyColumnLoader.hpp"

namespace graph::query::execution::operators {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void addRequiredProperty(std::vector<std::string> &properties, const std::string &property) {
		if (std::find(properties.begin(), properties.end(), property) == properties.end()) {
			properties.push_back(property);
		}
	}
} // namespace

NodeTopKFastPathOperator::NodeTopKFastPathOperator(
		std::shared_ptr<storage::DataManager> dm,
		std::shared_ptr<indexes::IndexManager> im,
		NodeScanConfig config,
		NodeScanRequirements requirements,
		std::vector<VectorizedPropertyPredicate> predicates,
		std::vector<NodeTopKProjection> projections,
		std::string sortProperty,
		bool ascending,
		int64_t limit)
	: dm_(std::move(dm)),
	  im_(std::move(im)),
	  config_(std::move(config)),
	  requirements_(std::move(requirements)),
	  predicates_(std::move(predicates)),
	  projections_(std::move(projections)),
	  sortProperty_(std::move(sortProperty)),
	  ascending_(ascending),
	  limit_(normalizeLimit(limit)) {}

void NodeTopKFastPathOperator::open() {
	NodeCandidateSource source(dm_, im_);
	candidateSet_ = source.collectWithMetadata(config_);
	outputRows_.clear();
	currentOutputIndex_ = 0;
	built_ = false;
}

std::optional<RecordBatch> NodeTopKFastPathOperator::next() {
	if (!built_) {
		buildTopK();
		built_ = true;
	}

	if (currentOutputIndex_ >= outputRows_.size()) {
		return std::nullopt;
	}

	RecordBatch batch;
	batch.reserve(std::min(DEFAULT_BATCH_SIZE, outputRows_.size() - currentOutputIndex_));
	while (batch.size() < DEFAULT_BATCH_SIZE && currentOutputIndex_ < outputRows_.size()) {
		batch.push_back(std::move(outputRows_[currentOutputIndex_++]));
	}
	return batch;
}

void NodeTopKFastPathOperator::close() {
	candidateSet_ = NodeCandidateSet{};
	outputRows_.clear();
	currentOutputIndex_ = 0;
	built_ = false;
}

std::vector<std::string> NodeTopKFastPathOperator::getOutputVariables() const {
	std::vector<std::string> variables;
	variables.reserve(projections_.size());
	for (const auto &projection : projections_) {
		variables.push_back(projection.alias);
	}
	return variables;
}

std::string NodeTopKFastPathOperator::toString() const {
	return "NodeTopKFastPath(" + config_.variable + "." + sortProperty_ +
	       (ascending_ ? " ASC LIMIT " : " DESC LIMIT ") + std::to_string(limit_) + ")";
}

void NodeTopKFastPathOperator::buildTopK() {
	const auto start = Clock::now();
	if (limit_ == 0 || projections_.empty()) {
		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.topk", elapsedNs(start));
		}
		return;
	}

	std::vector<Row> heap;
	heap.reserve(limit_);
	auto heapComparator = [this](const Row &left, const Row &right) {
		// comesBefore() is final output order; the heap root is the worst retained row.
		return comesBefore(left.sortKey, right.sortKey);
	};

	NodeBatchLoader loader(dm_, threadPool_);
	const auto requirements = relaxSatisfiedCandidateChecks(makeSelectionRequirements(), candidateSet_);
	for (size_t begin = 0; begin < candidateSet_.ids.size();) {
		if (queryContext_) {
			queryContext_->checkGuard();
		}
		const size_t batchSize = chooseColumnarNodeBatchSize(
				candidateSet_.ids.size() - begin, threadPool_, PhysicalOperator::DEFAULT_BATCH_SIZE);
		const size_t end = begin + batchSize;
		auto batch = loader.load(candidateSet_.ids, begin, end, config_, requirements);
		applyPredicates(batch, predicates_);
		const auto &sortColumn = batch.propertyColumns.at(sortProperty_);

		for (size_t row = 0; row < batch.nodeIds.size(); ++row) {
			if (!batch.isSelected(row)) {
				continue;
			}

			Row candidate;
			candidate.nodeId = batch.nodeIds[row];
			candidate.sortKey = sortColumn[row].value_or(PropertyValue());

			if (heap.size() < limit_) {
				heap.push_back(std::move(candidate));
				std::push_heap(heap.begin(), heap.end(), heapComparator);
			} else if (comesBefore(candidate.sortKey, heap.front().sortKey)) {
				std::pop_heap(heap.begin(), heap.end(), heapComparator);
				heap.back() = std::move(candidate);
				std::push_heap(heap.begin(), heap.end(), heapComparator);
			}
		}
		begin = end;
	}

	auto finalComparator = [this](const Row &left, const Row &right) {
		return comesBefore(left.sortKey, right.sortKey);
	};
	std::sort(heap.begin(), heap.end(), finalComparator);
	loadProjectionValues(heap);

	outputRows_.reserve(heap.size());
	for (const auto &row : heap) {
		outputRows_.push_back(makeRecord(row));
	}

	if (debug::PerfTrace::isEnabled()) {
		debug::PerfTrace::addDuration("node_scan.topk", elapsedNs(start));
	}
}

NodeScanRequirements NodeTopKFastPathOperator::makeSelectionRequirements() const {
	NodeScanRequirements requirements = requirements_;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties.clear();
	addRequiredProperty(requirements.requiredProperties, sortProperty_);
	for (const auto &predicate : predicates_) {
		addRequiredProperty(requirements.requiredProperties, predicate.propertyKey);
	}
	return requirements;
}

void NodeTopKFastPathOperator::loadProjectionValues(std::vector<Row> &rows) const {
	if (rows.empty()) {
		return;
	}

	std::vector<std::string> projectionProperties;
	projectionProperties.reserve(projections_.size());
	for (const auto &projection : projections_) {
		if (projection.property != sortProperty_) {
			addRequiredProperty(projectionProperties, projection.property);
		}
	}

	std::unordered_map<int64_t, std::unordered_map<std::string, PropertyValue>> valuesByNodeId;
	if (!projectionProperties.empty()) {
		std::vector<int64_t> topIds;
		topIds.reserve(rows.size());
		for (const auto &row : rows) {
			topIds.push_back(row.nodeId);
		}

		const auto nodes = dm_->getNodeBatch(topIds);
		// getNodeBatch() filters missing/inactive ids, so every returned node participates in projection loading.
		std::vector<uint8_t> selected(nodes.size(), uint8_t{1});

		NodePropertyColumnLoader propertyLoader(dm_, threadPool_);
		const auto columns = propertyLoader.loadColumns(nodes, selected, projectionProperties);
		valuesByNodeId.reserve(nodes.size());
		for (size_t row = 0; row < nodes.size(); ++row) {
			auto &values = valuesByNodeId[nodes[row].getId()];
			for (const auto &[key, column] : columns) {
				if (column[row].has_value()) {
					values.emplace(key, *column[row]);
				}
			}
		}
	}

	for (auto &row : rows) {
		row.values.clear();
		row.values.reserve(projections_.size());
		const auto valuesIt = valuesByNodeId.find(row.nodeId);
		for (const auto &projection : projections_) {
			if (projection.property == sortProperty_) {
				row.values.push_back(row.sortKey);
			} else if (valuesIt != valuesByNodeId.end()) {
				const auto valueIt = valuesIt->second.find(projection.property);
				row.values.push_back(valueIt != valuesIt->second.end() ? valueIt->second : PropertyValue());
			} else {
				row.values.emplace_back();
			}
		}
	}
}

bool NodeTopKFastPathOperator::comesBefore(const PropertyValue &left, const PropertyValue &right) const {
	if (left == right) {
		return false;
	}
	return ascending_ ? left < right : left > right;
}

Record NodeTopKFastPathOperator::makeRecord(const Row &row) const {
	Record record;
	for (size_t index = 0; index < projections_.size(); ++index) {
		record.setValue(projections_[index].alias, PropertyValue(row.values[index]));
	}
	return record;
}

size_t NodeTopKFastPathOperator::normalizeLimit(int64_t limit) {
	if (limit <= 0) {
		return 0;
	}
	const auto unsignedLimit = static_cast<uint64_t>(limit);
	const auto maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
	return static_cast<size_t>(std::min(unsignedLimit, maxSize));
}

} // namespace graph::query::execution::operators
