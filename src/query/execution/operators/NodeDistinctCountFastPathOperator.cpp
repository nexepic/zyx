#include "graph/query/execution/operators/NodeDistinctCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/TypedDistinctSet.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"

namespace graph::query::execution::operators {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		std::vector<int64_t> resolveLabelIds(const std::shared_ptr<storage::DataManager> &dm,
											 const NodeScanConfig &config,
											 const NodeScanRequirements &requirements) {
			std::vector<int64_t> labelIds;
			if (!requirements.needsLabels) {
				return labelIds;
			}
			labelIds.reserve(config.labels.size());
			for (const auto &label : config.labels) {
				const int64_t labelId = dm->resolveTokenId(label);
				labelIds.push_back(labelId == 0 ? -1 : labelId);
			}
			return labelIds;
		}

		bool rowMatchesLabels(const NodeMetadataBatch &batch, size_t row, const std::vector<int64_t> &labelIds) {
			for (const int64_t labelId : labelIds) {
				if (!batch.hasLabelId(row, labelId)) {
					return false;
				}
			}
			return true;
		}

		std::optional<size_t> countDistinctFromMetadata(const std::shared_ptr<storage::DataManager> &dm,
														const NodeCandidateSet &candidateSet,
														const NodeScanConfig &config,
														const NodeScanRequirements &inputRequirements,
														const std::string &distinctProperty,
														TypedDistinctSet &seen,
														concurrent::ThreadPool *threadPool,
														const QueryContext *queryContext) {
			const auto requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
			const auto labelIds = resolveLabelIds(dm, config, requirements);
			NodeMetadataColumnLoader metadataLoader(dm);

			static constexpr size_t kMetadataDistinctBatchSize = 65536;
			for (size_t begin = 0; begin < candidateSet.ids.size();) {
				if (queryContext) {
					queryContext->checkGuard();
				}
				const size_t end = std::min(candidateSet.ids.size(), begin + kMetadataDistinctBatchSize);
				auto metadata = metadataLoader.loadBatch(candidateSet.ids, begin, end);
				if (!metadata.has_value()) {
					return std::nullopt;
				}

				std::vector<int64_t> propertyEntityIds;
				std::vector<size_t> propertyRows;
				std::vector<size_t> fallbackRows;
				propertyEntityIds.reserve(metadata->size());
				propertyRows.reserve(metadata->size());

				for (size_t row = 0; row < metadata->size(); ++row) {
					if (!metadata->isValid(row)) {
						continue;
					}
					if (requirements.needsActiveCheck && metadata->active[row] == 0) { // ZYX_COV_EXCL_LINE
						continue;
					}
					if (!labelIds.empty() && !rowMatchesLabels(*metadata, row, labelIds)) {
						continue;
					}

					const int64_t propertyEntityId = metadata->propertyEntityIds[row];
					const auto storageType = metadata->propertyStorageTypes[row];
					if (storageType == PropertyStorageType::PROPERTY_ENTITY && propertyEntityId != 0) {
						propertyEntityIds.push_back(propertyEntityId);
						propertyRows.push_back(row);
					} else if (storageType == PropertyStorageType::BLOB_ENTITY && propertyEntityId != 0) {
						fallbackRows.push_back(row);
					}
				}

				if (!propertyEntityIds.empty()) {
					(void) dm->bulkVisitPropertyEntityValues(
							propertyEntityIds, propertyRows, metadata->size(), distinctProperty,
							[&](size_t, const PropertyValue &value) {
								if (!expressions::EvaluationContext::isNull(value)) {
									seen.insert(value);
								}
							},
							threadPool);
				}

				for (const size_t row : fallbackRows) {
					Node node = metadata->toNode(row);
					auto properties = dm->getNodePropertiesDirect(node);
					auto valueIt = properties.find(distinctProperty);
					if (valueIt != properties.end() && !expressions::EvaluationContext::isNull(valueIt->second)) {
						seen.insert(valueIt->second);
					}
				}
				begin = end;
			}
			return seen.size();
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

		TypedDistinctSet seen;
		bool countedFromMetadata = false;
		if (predicates_.empty()) {
			auto metadataCount = countDistinctFromMetadata(dm_, candidateSet_, config_, requirements_, distinctProperty_,
														   seen, threadPool_, queryContext_);
			countedFromMetadata = metadataCount.has_value();
			if (!countedFromMetadata) {
				seen.clear();
			}
		}

		if (!countedFromMetadata) {
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
					seen.insert(*column[row]);
				}
				begin = end;
			}
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
