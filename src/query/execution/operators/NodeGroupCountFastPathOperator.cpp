#include "graph/query/execution/operators/NodeGroupCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
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
			struct FallbackRow {
				NodeMetadataRow metadata;
			};

			const auto requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
			const NodeMetadataRowFilter rowFilter(dm, config, requirements);
			NodeMetadataColumnLoader metadataLoader(dm);
			TypedGroupCounter groups;

			static constexpr size_t kMetadataGroupBatchSize = 65536;
			for (size_t begin = 0; begin < candidateSet.ids.size();) {
				if (queryContext) {
					queryContext->checkGuard();
				}
				const size_t end = std::min(candidateSet.ids.size(), begin + kMetadataGroupBatchSize);

				std::vector<int64_t> propertyEntityIds;
				std::vector<size_t> propertyRows;
				std::vector<FallbackRow> fallbackRows;
				propertyEntityIds.reserve(end - begin);
				propertyRows.reserve(end - begin);
				fallbackRows.reserve((end - begin) / 8);

				size_t selectedCount = 0;
				const bool visited = metadataLoader.visitBatch(candidateSet.ids, begin, end, [&](size_t, const NodeMetadataRow &metadata) {
					if (!rowFilter.accepts(metadata)) {
						return true;
					}
					const size_t selectedRow = selectedCount;
					++selectedCount;

					const auto storageType = metadata.propertyStorageType;
					const int64_t propertyEntityId = metadata.propertyEntityId;
					if (storageType == PropertyStorageType::PROPERTY_ENTITY && propertyEntityId != 0) {
						propertyEntityIds.push_back(propertyEntityId);
						propertyRows.push_back(selectedRow);
					} else if (storageType == PropertyStorageType::BLOB_ENTITY && propertyEntityId != 0) {
						fallbackRows.push_back({metadata});
					}
					return true;
				});
				if (!visited) {
					return std::nullopt;
				}

				size_t rowsWithGroupValue = 0;
				if (dm && !propertyEntityIds.empty()) {
					const auto loadStart = Clock::now();
					(void) dm->bulkVisitPropertyEntityValues(
							propertyEntityIds,
							propertyRows,
							selectedCount,
							groupProperty,
							[&](size_t row, const PropertyValue &value) {
								if (row < selectedCount) {
									groups.add(value);
									++rowsWithGroupValue;
								}
							},
							threadPool);
					addProfile("node_scan.load_property_entities", loadStart);
				}

				if (dm && !fallbackRows.empty()) {
					const auto fallbackStart = Clock::now();
					for (const auto &fallback : fallbackRows) {
						Node node = fallback.metadata.toNode();
						const auto properties = dm->getNodePropertiesDirect(node);
						if (auto valueIt = properties.find(groupProperty); valueIt != properties.end()) {
							groups.add(valueIt->second);
							++rowsWithGroupValue;
						}
					}
					addProfile("node_scan.load_properties", fallbackStart);
				}

				if (selectedCount > rowsWithGroupValue) {
					groups.add(PropertyValue(), static_cast<int64_t>(selectedCount - rowsWithGroupValue));
				}
				begin = end;
			}
			return groups;
		}
	} // namespace

	NodeGroupCountFastPathOperator::NodeGroupCountFastPathOperator(
			std::shared_ptr<storage::DataManager> dm,
			std::shared_ptr<indexes::IndexManager> im,
			NodeScanConfig config,
			NodeScanRequirements requirements,
			std::vector<VectorizedPropertyPredicate> predicates,
			std::string groupProperty,
			std::string groupAlias,
			std::string outputAlias) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), groupProperty_(std::move(groupProperty)),
		groupAlias_(std::move(groupAlias)), outputAlias_(std::move(outputAlias)) {}

	void NodeGroupCountFastPathOperator::open() {
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = source.collectWithMetadata(config_);
		emitted_ = false;
	}

	std::optional<RecordBatch> NodeGroupCountFastPathOperator::next() {
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
				for (size_t row = 0; row < batch.nodeIds.size(); ++row) {
					if (row >= batch.selected.size() || batch.selected[row] == 0) {
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

	void NodeGroupCountFastPathOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		emitted_ = false;
	}

	std::vector<std::string> NodeGroupCountFastPathOperator::getOutputVariables() const {
		return {groupAlias_, outputAlias_};
	}

	std::string NodeGroupCountFastPathOperator::toString() const {
		return "NodeGroupCountFastPath(" + config_.variable + "." + groupProperty_ + " -> " + outputAlias_ + ")";
	}

} // namespace graph::query::execution::operators
