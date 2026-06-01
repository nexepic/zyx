#include "graph/query/execution/operators/NodeGroupCountFastPathOperator.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodePropertyColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

namespace graph::query::execution::operators {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		struct GroupCount {
			PropertyValue value;
			int64_t count = 0;
		};

		struct GroupHash {
			size_t operator()(const TypedEqualityKey &key) const { return key.hash(); }
		};

		using GroupCounts = std::unordered_map<TypedEqualityKey, GroupCount, GroupHash>;

		void addGroupValue(GroupCounts &groups, PropertyValue value) {
			auto key = TypedEqualityKey::from(value);
			auto [it, inserted] = groups.emplace(std::move(key), GroupCount{value, 0});
			if (!inserted && it->second.value.getType() == PropertyType::NULL_TYPE && value.getType() != PropertyType::NULL_TYPE) {
				it->second.value = std::move(value);
			}
			++it->second.count;
		}

		std::optional<GroupCounts> countGroupsFromMetadata(
				const std::shared_ptr<storage::DataManager> &dm,
				const NodeCandidateSet &candidateSet,
				const NodeScanConfig &config,
				const NodeScanRequirements &inputRequirements,
				const std::string &groupProperty,
				concurrent::ThreadPool *threadPool,
				const QueryContext *queryContext) {
			const auto requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
			const NodeMetadataRowFilter rowFilter(dm, config, requirements);
			NodeMetadataColumnLoader metadataLoader(dm);
			NodePropertyColumnLoader propertyLoader(dm, threadPool);
			GroupCounts groups;

			static constexpr size_t kMetadataGroupBatchSize = 65536;
			for (size_t begin = 0; begin < candidateSet.ids.size();) {
				if (queryContext) {
					queryContext->checkGuard();
				}
				const size_t end = std::min(candidateSet.ids.size(), begin + kMetadataGroupBatchSize);
				auto metadata = metadataLoader.loadBatch(candidateSet.ids, begin, end);
				if (!metadata.has_value()) {
					return std::nullopt;
				}

				std::vector<uint8_t> selected(metadata->size(), 0);
				for (size_t row = 0; row < metadata->size(); ++row) {
					selected[row] = rowFilter.accepts(*metadata, row) ? 1U : 0U;
				}

				auto columns = propertyLoader.loadColumns(*metadata, selected, {groupProperty});
				auto columnIt = columns.find(groupProperty);
				const auto *column = columnIt == columns.end() ? nullptr : &columnIt->second;
				for (size_t row = 0; row < metadata->size(); ++row) {
					if (selected[row] == 0) {
						continue;
					}
					PropertyValue value;
					if (column && row < column->size() && (*column)[row].has_value()) {
						value = *(*column)[row];
					}
					addGroupValue(groups, std::move(value));
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
		GroupCounts groups;
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
					addGroupValue(groups, std::move(value));
				}
				begin = end;
			}
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.group_count", elapsedNs(start));
		}

		RecordBatch output;
		output.reserve(groups.size());
		for (const auto &[_, group] : groups) {
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
