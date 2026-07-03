#include "graph/query/execution/operators/NodeDistinctCountScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/StorageScalarValueAdapter.hpp"
#include "graph/query/execution/TypedDistinctSet.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"

namespace graph::query::execution::operators {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		std::optional<size_t> countDistinctFromMetadata(const std::shared_ptr<storage::DataManager> &dm,
														const NodeCandidateSet &candidateSet,
														const NodeScanConfig &config,
														const NodeScanRequirements &inputRequirements,
														const std::string &distinctProperty,
														TypedDistinctSet &seen,
														concurrent::ThreadPool *threadPool,
														const QueryContext *queryContext) {
			struct FallbackRow {
				NodeMetadataRow metadata;
			};

			struct MetadataPartitionState {
				std::vector<int64_t> propertyEntityIds;
				std::vector<size_t> propertyRows;
				std::vector<FallbackRow> fallbackRows;

				void reserve(size_t rowCount) {
					propertyEntityIds.reserve(rowCount);
					propertyRows.reserve(rowCount);
					fallbackRows.reserve(rowCount / 8);
				}
			};

			const auto requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
			const NodeMetadataRowFilter rowFilter(dm, config, requirements);
			NodeMetadataColumnLoader metadataLoader(dm);

			static constexpr size_t kMetadataDistinctBatchSize = 65536;
			for (size_t begin = 0; begin < candidateSet.ids.size();) {
				if (queryContext) {
					queryContext->checkGuard();
				}
				const size_t end = std::min(candidateSet.ids.size(), begin + kMetadataDistinctBatchSize);

				std::vector<int64_t> propertyEntityIds;
				std::vector<size_t> propertyRows;
				std::vector<FallbackRow> fallbackRows;
				std::vector<MetadataPartitionState> partitions;

				const bool visited = metadataLoader.visitBatchPartitioned(
						candidateSet.ids, begin, end,
						[&](size_t partitionCount) {
							partitions.resize(partitionCount);
							const size_t rowsPerPartition = std::max<size_t>(1, (end - begin) / partitionCount);
							for (auto &partition : partitions) {
								partition.reserve(rowsPerPartition);
							}
						},
						[&](size_t partition, size_t, const NodeMetadataRow &metadata) {
					if (!rowFilter.accepts(metadata)) {
						return true;
					}
					if (partition >= partitions.size()) { // ZYX_COV_EXCL_LINE: visitBatchPartitioned only emits initialized partition ids.
						return true;
					}
					auto &state = partitions[partition];

					const int64_t propertyEntityId = metadata.propertyEntityId;
					if (propertyEntityId == 0) {
						return true;
					}
					const auto storageType = metadata.propertyStorageType;
					if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
						const size_t propertyRow = state.propertyEntityIds.size();
						state.propertyEntityIds.push_back(propertyEntityId);
						state.propertyRows.push_back(propertyRow);
					} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE: non-zero property refs are persisted as property or blob entities.
						state.fallbackRows.push_back({metadata});
					}
					return true;
				},
				threadPool);
				if (!visited) {
					return std::nullopt;
				}

				size_t propertyRowCount = 0;
				size_t fallbackRowCount = 0;
				for (const auto &partition : partitions) {
					propertyRowCount += partition.propertyEntityIds.size();
					fallbackRowCount += partition.fallbackRows.size();
				}
				propertyEntityIds.reserve(propertyRowCount);
				propertyRows.reserve(propertyRowCount);
				fallbackRows.reserve(fallbackRowCount);
				for (const auto &partition : partitions) {
					const size_t rowOffset = propertyEntityIds.size();
					propertyEntityIds.insert(propertyEntityIds.end(),
											 partition.propertyEntityIds.begin(),
											 partition.propertyEntityIds.end());
					for (const size_t propertyRow : partition.propertyRows) {
						propertyRows.push_back(rowOffset + propertyRow);
					}
					fallbackRows.insert(fallbackRows.end(),
										partition.fallbackRows.begin(),
										partition.fallbackRows.end());
				}

				if (!propertyEntityIds.empty()) { // ZYX_COV_EXCL_LINE: metadata fast-path is entered only when direct property entity rows are available.
					std::vector<TypedDistinctSet> partitionSets;
					(void) dm->bulkVisitPropertyEntityScalarValuesPartitioned(
							propertyEntityIds, propertyRows, propertyEntityIds.size(), distinctProperty,
							[&](size_t partitionCount) {
								partitionSets.resize(partitionCount);
							},
							[&](size_t partition, size_t, const storage::PropertyEntityScalarValue &value) {
								const auto scalar = scalarValueFromStorage(value);
								if (!isNullScalar(scalar)) {
									partitionSets[partition].insertScalar(scalar);
								}
							},
							threadPool);
					for (const auto &partitionSet : partitionSets) {
						seen.mergeFrom(partitionSet);
					}
				}

				for (const auto &fallback : fallbackRows) {
					Node node = fallback.metadata.toNode();
					auto properties = dm->getNodePropertiesDirect(node);
					auto valueIt = properties.find(distinctProperty);
					if (valueIt != properties.end() && !expressions::EvaluationContext::isNull(valueIt->second)) { // ZYX_COV_EXCL_LINE: blob fallback rows originate from the requested non-null property.
						seen.insert(valueIt->second);
					}
				}
				begin = end;
			}
			return seen.size();
		}
	} // namespace

	NodeDistinctCountScanOperator::NodeDistinctCountScanOperator(
			std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im, NodeScanConfig config,
			NodeScanRequirements requirements, std::vector<VectorizedPropertyPredicate> predicates,
			std::string distinctProperty, std::string outputAlias,
			std::vector<ExplainAttribute> explainAttributes) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), distinctProperty_(std::move(distinctProperty)),
		outputAlias_(std::move(outputAlias)), explainAttributes_(std::move(explainAttributes)) {}

	void NodeDistinctCountScanOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = NodeCandidateSet{};
		candidateSet_ = source.collectWithMetadata(config_);
		emitted_ = false;
	}

	std::optional<RecordBatch> NodeDistinctCountScanOperator::next() {
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
					begin = end;
					continue;
				}
				const auto &column = columnIt->second;
				const size_t rowCount = std::min({batch.nodeIds.size(), batch.selected.size(), column.size()});
				for (size_t row = 0; row < rowCount; ++row) {
					if (batch.selected[row] == 0 || !column[row].has_value() ||
						expressions::EvaluationContext::isNull(*column[row])) {
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

	void NodeDistinctCountScanOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		emitted_ = false;
	}

	std::vector<std::string> NodeDistinctCountScanOperator::getOutputVariables() const { return {outputAlias_}; }

	std::string NodeDistinctCountScanOperator::toString() const {
		return "NodeDistinctCountScan(" + config_.variable + "." + distinctProperty_ + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
