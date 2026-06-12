#include "graph/query/execution/operators/NodeGroupCountScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/StorageScalarValueAdapter.hpp"
#include "graph/query/execution/TypedGroupCounter.hpp"

namespace graph::query::execution::operators {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		void addProfile(const char *phase, Clock::time_point start) {
			if (debug::PerfTrace::isEnabled()) {
				debug::PerfTrace::addDuration(phase, elapsedNs(start));
			}
		}

		std::optional<TypedGroupCounter> countGroupsFromMetadata(
				const std::shared_ptr<storage::DataManager> &dm,
				const NodeCandidateSet &candidateSet,
				const NodeScanConfig &config,
				const NodeScanRequirements &inputRequirements,
				const std::string &groupProperty,
				concurrent::ThreadPool *threadPool,
				const QueryContext *queryContext) {
			const auto requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
			NodeMetadataColumnLoader metadataLoader(dm);
			TypedGroupCounter groups;

			static constexpr size_t kMetadataGroupBatchSize = 65536;
			for (size_t begin = 0; begin < candidateSet.ids.size();) {
				if (queryContext) {
					queryContext->checkGuard();
				}
				const size_t end = std::min(candidateSet.ids.size(), begin + kMetadataGroupBatchSize);

				auto candidates = metadataLoader.collectPropertyCountCandidates(
						candidateSet.ids, begin, end, config, requirements, threadPool);
				if (!candidates.has_value()) {
					return std::nullopt;
				}

				size_t rowsWithGroupValue = 0;
				if (!candidates->propertyEntityIds.empty()) {
					const auto loadStart = Clock::now();
					std::vector<TypedGroupCounter> partitionGroups;
					std::vector<size_t> partitionRowsWithGroupValue;
					(void) dm->bulkVisitPropertyEntityScalarValuesPartitioned(
							candidates->propertyEntityIds,
							candidates->propertyRows,
							candidates->propertyRowCount(),
							groupProperty,
							[&](size_t partitionCount) {
								partitionGroups.resize(partitionCount);
								partitionRowsWithGroupValue.assign(partitionCount, 0);
							},
							[&](size_t partition, size_t row, const storage::PropertyEntityScalarValue &value) {
								if (row < candidates->propertyRowCount()) {
									partitionGroups[partition].addScalar(scalarValueFromStorage(value));
									++partitionRowsWithGroupValue[partition];
								}
							},
							threadPool);
					const auto mergeStart = Clock::now();
					for (size_t partition = 0; partition < partitionGroups.size(); ++partition) {
						groups.mergeFrom(partitionGroups[partition]);
						rowsWithGroupValue += partitionRowsWithGroupValue[partition];
					}
					addProfile("node_scan.group_count.merge_partitions", mergeStart);
					addProfile("node_scan.load_property_entities", loadStart);
				}

				if (!candidates->blobRefs.empty()) {
					const auto fallbackStart = Clock::now();
					for (const auto &fallback : candidates->blobRefs) {
						const auto properties = dm->getNodePropertiesDirect(fallback.toNode());
						if (auto valueIt = properties.find(groupProperty); valueIt != properties.end()) {
							groups.add(valueIt->second);
							++rowsWithGroupValue;
						}
					}
					addProfile("node_scan.load_properties", fallbackStart);
				}

				if (candidates->acceptedRowCount > rowsWithGroupValue) {
					groups.addNull(static_cast<int64_t>(candidates->acceptedRowCount - rowsWithGroupValue));
				}
				begin = end;
			}
			return groups;
		}
	} // namespace

	NodeGroupCountScanOperator::NodeGroupCountScanOperator(
			std::shared_ptr<storage::DataManager> dm,
			std::shared_ptr<indexes::IndexManager> im,
			NodeScanConfig config,
			NodeScanRequirements requirements,
			std::vector<VectorizedPropertyPredicate> predicates,
			std::string groupProperty,
			std::string groupAlias,
			std::string outputAlias,
			std::vector<ExplainAttribute> explainAttributes) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), groupProperty_(std::move(groupProperty)),
		groupAlias_(std::move(groupAlias)), outputAlias_(std::move(outputAlias)),
		explainAttributes_(std::move(explainAttributes)) {}

	void NodeGroupCountScanOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = source.collectWithMetadata(config_);
		emitted_ = false;
	}

	std::optional<RecordBatch> NodeGroupCountScanOperator::next() {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;

		const auto start = Clock::now();
		TypedGroupCounter groups;
		bool countedFromMetadata = false;
		if (predicates_.empty()) {
			auto metadataGroups = countGroupsFromMetadata(
					dm_, candidateSet_, config_, requirements_, groupProperty_, threadPool_, queryContext_);
			countedFromMetadata = metadataGroups.has_value();
			if (countedFromMetadata) {
				groups = std::move(*metadataGroups);
			}
		}

		if (!countedFromMetadata) {
			NodeBatchLoader loader(dm_, threadPool_);
			const auto requirements = relaxSatisfiedCandidateChecks(requirements_, candidateSet_);
			for (size_t begin = 0; begin < candidateSet_.ids.size();) {
				if (queryContext_) {
					queryContext_->checkGuard();
				}
				const size_t batchSize = chooseColumnarNodeBatchSize(
						candidateSet_.ids.size() - begin, threadPool_, PhysicalOperator::DEFAULT_BATCH_SIZE);
				const size_t end = begin + batchSize;
				auto batch = loader.load(candidateSet_.ids, begin, end, config_, requirements);
				applyPredicates(batch, predicates_);

				auto columnIt = batch.propertyColumns.find(groupProperty_);
				const auto *column = columnIt == batch.propertyColumns.end() ? nullptr : &columnIt->second;
				const size_t rowCount = std::min(batch.nodeIds.size(), batch.selected.size());
				for (size_t row = 0; row < rowCount; ++row) {
					if (batch.selected[row] == 0) {
						continue;
					}
					PropertyValue value;
					if (column && row < column->size() && (*column)[row].has_value()) {
						value = *(*column)[row];
					}
					groups.add(value);
				}
				begin = end;
			}
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.group_count", elapsedNs(start));
		}

		RecordBatch output;
		const auto groupCounts = groups.toVector();
		output.reserve(groupCounts.size());
		for (const auto &group : groupCounts) {
			Record record;
			record.setValue(groupAlias_, group.value);
			record.setValue(outputAlias_, PropertyValue(group.count));
			output.push_back(std::move(record));
		}
		return output;
	}

	void NodeGroupCountScanOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		emitted_ = false;
	}

	std::vector<std::string> NodeGroupCountScanOperator::getOutputVariables() const {
		return {groupAlias_, outputAlias_};
	}

	std::string NodeGroupCountScanOperator::toString() const {
		return "NodeGroupCountScan(" + config_.variable + "." + groupProperty_ + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
