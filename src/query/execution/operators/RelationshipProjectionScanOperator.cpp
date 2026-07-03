#include "graph/query/execution/operators/RelationshipProjectionScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/query/execution/RelationshipCandidateSource.hpp"
#include "graph/query/execution/RelationshipPropertyColumnLoader.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::execution::operators {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void addUnique(std::vector<std::string> &values, const std::string &value) {
		if (std::find(values.begin(), values.end(), value) == values.end()) {
			values.push_back(value);
		}
	}
} // namespace

RelationshipProjectionScanOperator::RelationshipProjectionScanOperator(
		std::shared_ptr<storage::DataManager> dm,
		std::shared_ptr<indexes::IndexManager> im,
		DirectRelationshipCountConfig config,
		std::string targetVariable,
		std::vector<std::string> targetLabels,
		std::vector<RelationshipProjectionScanItem> projections,
		std::optional<size_t> limit,
		std::vector<ExplainAttribute> explainAttributes)
	: dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)),
	  targetVariable_(std::move(targetVariable)), targetLabels_(std::move(targetLabels)),
	  projections_(std::move(projections)), limit_(limit), explainAttributes_(std::move(explainAttributes)) {
	if (config_.edgePredicates.empty() && !config_.edgeProperties.empty()) {
		config_.edgePredicates.reserve(config_.edgeProperties.size());
		for (const auto &[key, value] : config_.edgeProperties) {
			VectorizedPropertyPredicate predicate;
			predicate.propertyKey = key;
			predicate.op = VectorPredicateOp::VPO_EQ;
			predicate.value = value;
			config_.edgePredicates.push_back(std::move(predicate));
		}
	}
}

void RelationshipProjectionScanOperator::open() {
	candidateIds_.clear();
	hasCandidateIds_ = false;
	candidateIndex_ = 0;
	nextEdgeId_ = 1;
	emittedRows_ = 0;

	edgeTypeId_ = 0;
	if (!config_.edgeType.empty()) {
		edgeTypeId_ = dm_->resolveTokenId(config_.edgeType);
		if (edgeTypeId_ == 0) {
			edgeTypeId_ = -1;
		}
	}

	targetLabelIds_.clear();
	targetLabelIds_.reserve(targetLabels_.size());
	for (const auto &label : targetLabels_) {
		const int64_t labelId = dm_->resolveTokenId(label);
		targetLabelIds_.push_back(labelId == 0 ? -1 : labelId);
	}

	const auto edgeAllocator = dm_->getIdAllocator(EntityType::Edge);
	maxEdgeId_ = edgeAllocator ? edgeAllocator->getCurrentMaxId() : 0; // ZYX_COV_EXCL_LINE: DataManager owns an edge allocator for the database lifetime.

	RelationshipCandidateSource source(dm_, im_);
	auto candidates = source.collect(config_);
	if (candidates.available) {
		candidateIds_ = std::move(candidates.ids);
		hasCandidateIds_ = true;
	}
}

std::optional<RecordBatch> RelationshipProjectionScanOperator::next() {
	const bool traceEnabled = debug::PerfTrace::isEnabled();
	const auto start = traceEnabled ? Clock::now() : Clock::time_point{};
	if (remainingLimit() == 0) {
		return std::nullopt;
	}

	const auto relationshipProperties = requiredRelationshipProperties();
	RelationshipPropertyColumnLoader propertyLoader(dm_, threadPool_);
	PropertyPredicateKernel predicateKernel(config_.edgePredicates);

	while (remainingLimit() > 0) {
		if (queryContext_) {
			queryContext_->checkGuard();
		}
		auto metadataOpt = loadNextMetadataBatch();
		if (!metadataOpt.has_value()) {
			break;
		}
		auto &metadata = *metadataOpt;
		if (metadata.size() == 0) {
			continue;
		}

		std::vector<uint8_t> selected(metadata.size(), uint8_t{0});
		for (size_t row = 0; row < metadata.size(); ++row) {
			selected[row] = metadata.isValid(row) && metadata.active[row] != 0 &&
			                (edgeTypeId_ == 0 || metadata.typeIds[row] == edgeTypeId_) ? uint8_t{1} : uint8_t{0};
		}

		auto relationshipColumns = relationshipProperties.empty()
				? std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>{}
				: propertyLoader.loadColumns(metadata, selected, relationshipProperties);

		if (!predicateKernel.empty()) {
			for (size_t row = 0; row < metadata.size(); ++row) {
				if (selected[row] == 0) {
					continue;
				}
				for (const auto &predicate : config_.edgePredicates) {
					std::optional<PropertyValue> actual;
					auto columnIt = relationshipColumns.find(predicate.propertyKey);
					if (columnIt != relationshipColumns.end()) { // ZYX_COV_EXCL_LINE: predicate properties are included in requiredRelationshipProperties().
						const auto &column = columnIt->second;
						if (row < column.size()) { // ZYX_COV_EXCL_LINE: property columns preserve metadata batch row cardinality.
							actual = column[row];
						}
					}
					if (!predicateKernel.matchesValue(actual, predicate)) {
						selected[row] = 0;
						break;
					}
				}
			}
		}

		RecordBatch output;
		output.reserve(std::min(metadata.size(), remainingLimit()));
		for (size_t row = 0; row < metadata.size() && output.size() < remainingLimit(); ++row) {
			if (selected[row] == 0) {
				continue;
			}
			const int64_t targetId = targetNodeId(metadata, row);
			if (!acceptsTargetNode(targetId)) {
				continue;
			}
			std::unordered_map<std::string, PropertyValue> targetProperties;
			const bool needsTargetProperties = std::any_of(projections_.begin(), projections_.end(), [](const auto &item) {
				return item.source == RelationshipProjectionSource::RPS_TARGET_NODE;
			});
			if (needsTargetProperties) {
				targetProperties = dm_->getNodeProperties(targetId);
			}
			output.push_back(makeRecord(metadata, row, relationshipColumns, targetProperties));
		}
		emittedRows_ += output.size();

		if (!output.empty()) {
			if (traceEnabled) {
				debug::PerfTrace::addDuration("relationship_projection_scan", elapsedNs(start));
			}
			return output;
		}
	}

	if (traceEnabled) {
		debug::PerfTrace::addDuration("relationship_projection_scan", elapsedNs(start));
	}
	return std::nullopt;
}

void RelationshipProjectionScanOperator::close() {
	candidateIds_.clear();
	hasCandidateIds_ = false;
	candidateIndex_ = 0;
	nextEdgeId_ = 1;
	emittedRows_ = 0;
}

void RelationshipProjectionScanOperator::setOutputLimitHint(size_t limit) {
	if (!limit_.has_value() || limit < *limit_) {
		limit_ = limit;
	}
}

std::vector<std::string> RelationshipProjectionScanOperator::getOutputVariables() const {
	std::vector<std::string> variables;
	variables.reserve(projections_.size());
	for (const auto &projection : projections_) {
		variables.push_back(projection.alias);
	}
	return variables;
}

std::string RelationshipProjectionScanOperator::toString() const {
	std::ostringstream oss;
	oss << "RelationshipProjectionScan(" << config_.edgeType;
	if (limit_.has_value()) {
		oss << " LIMIT " << *limit_;
	}
	oss << ")";
	return oss.str();
}

size_t RelationshipProjectionScanOperator::remainingLimit() const {
	if (!limit_.has_value()) {
		return std::numeric_limits<size_t>::max();
	}
	return emittedRows_ < *limit_ ? *limit_ - emittedRows_ : 0;
}

size_t RelationshipProjectionScanOperator::nextCandidateBatchSize() const {
	const size_t remaining = candidateIds_.size() - candidateIndex_;
	if (limit_.has_value()) {
		return std::min(remaining, std::max<size_t>(remainingLimit() * 4, DEFAULT_BATCH_SIZE / 2));
	}
	return std::min(remaining, size_t{8192});
}

size_t RelationshipProjectionScanOperator::nextRangeBatchSize() const {
	const auto remainingEdges = maxEdgeId_ >= nextEdgeId_
			? static_cast<size_t>(static_cast<uint64_t>(maxEdgeId_ - nextEdgeId_ + 1))
			: size_t{0}; // ZYX_COV_EXCL_LINE: loadNextMetadataBatch checks nextEdgeId_ before calling this helper.
	if (limit_.has_value()) {
		return std::min(remainingEdges, std::max<size_t>(remainingLimit() * 8, DEFAULT_BATCH_SIZE));
	}
	return std::min(remainingEdges, size_t{8192});
}

std::vector<std::string> RelationshipProjectionScanOperator::requiredRelationshipProperties() const {
	std::vector<std::string> properties;
	for (const auto &predicate : config_.edgePredicates) {
		addUnique(properties, predicate.propertyKey);
	}
	for (const auto &projection : projections_) {
		if (projection.source == RelationshipProjectionSource::RPS_EDGE) {
			addUnique(properties, projection.property);
		}
	}
	return properties;
}

bool RelationshipProjectionScanOperator::acceptsTargetNode(int64_t nodeId) const {
	if (nodeId <= 0) {
		return false;
	}
	Node node = dm_->getNode(nodeId);
	if (node.getId() == 0 || !node.isActive()) {
		return false;
	}
	for (const int64_t labelId : targetLabelIds_) {
		if (labelId <= 0 || !node.hasLabelId(labelId)) {
			return false;
		}
	}
	return true;
}

int64_t RelationshipProjectionScanOperator::targetNodeId(const RelationshipMetadataBatch &metadata, size_t row) const {
	if (config_.direction == "in") {
		return metadata.sourceNodeIds[row];
	}
	return metadata.targetNodeIds[row];
}

std::optional<RelationshipMetadataBatch> RelationshipProjectionScanOperator::loadNextMetadataBatch() {
	if (hasCandidateIds_) {
		if (candidateIndex_ >= candidateIds_.size()) {
			return std::nullopt;
		}
		const size_t begin = candidateIndex_;
		const size_t end = std::min(candidateIds_.size(), begin + nextCandidateBatchSize());
		candidateIndex_ = end;
		return loadCandidateMetadata(begin, end);
	}

	if (nextEdgeId_ > maxEdgeId_) {
		return std::nullopt;
	}
	const size_t batchSize = nextRangeBatchSize();
	if (batchSize == 0) { // ZYX_COV_EXCL_LINE: nextEdgeId_ > maxEdgeId_ is checked before computing the range batch.
		return std::nullopt;
	}
	const int64_t beginId = nextEdgeId_;
	const int64_t endId = std::min<int64_t>(maxEdgeId_, beginId + static_cast<int64_t>(batchSize) - 1);
	nextEdgeId_ = endId + 1;
	return loadRangeMetadata(beginId, endId);
}

RelationshipMetadataBatch RelationshipProjectionScanOperator::loadCandidateMetadata(size_t begin, size_t end) const {
	std::vector<int64_t> ids;
	ids.reserve(end - begin);
	for (size_t index = begin; index < end; ++index) {
		ids.push_back(candidateIds_[index]);
	}
	const auto edges = dm_->getEdgeBatch(ids);
	RelationshipMetadataBatch metadata;
	metadata.reserve(edges.size());
	for (const auto &edge : edges) {
		metadata.appendDefault();
		metadata.setFromEdge(metadata.size() - 1, edge);
	}
	return metadata;
}

RelationshipMetadataBatch RelationshipProjectionScanOperator::loadRangeMetadata(int64_t beginId, int64_t endId) const {
	RelationshipMetadataColumnLoader metadataLoader(dm_);
	if (auto metadata = metadataLoader.loadRange(beginId, endId)) {
		return *metadata;
	}

	const auto edges = dm_->getEdgesInRange(beginId, endId, static_cast<size_t>(endId - beginId + 1));
	RelationshipMetadataBatch metadata;
	metadata.reserve(edges.size());
	for (const auto &edge : edges) {
		metadata.appendDefault();
		metadata.setFromEdge(metadata.size() - 1, edge);
	}
	return metadata;
}

Record RelationshipProjectionScanOperator::makeRecord(
		const RelationshipMetadataBatch &metadata,
		size_t row,
		const std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &relationshipColumns,
		const std::unordered_map<std::string, PropertyValue> &targetProperties) const {
	Record record;
	for (const auto &projection : projections_) {
		PropertyValue value;
		if (projection.source == RelationshipProjectionSource::RPS_EDGE) {
			auto columnIt = relationshipColumns.find(projection.property);
			if (columnIt != relationshipColumns.end()) { // ZYX_COV_EXCL_LINE: edge projections are included in requiredRelationshipProperties().
				const auto &column = columnIt->second;
				if (row < column.size()) { // ZYX_COV_EXCL_LINE: property columns preserve metadata batch row cardinality.
					if (column[row].has_value()) {
						value = *column[row];
					}
				}
			}
		} else if (auto valueIt = targetProperties.find(projection.property); valueIt != targetProperties.end()) {
			value = valueIt->second;
		}
		record.setValue(projection.alias, std::move(value));
	}
	(void) metadata;
	return record;
}

} // namespace graph::query::execution::operators
