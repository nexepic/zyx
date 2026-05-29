#include "graph/query/execution/NodeBatchLoader.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodePropertyColumnLoader.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		std::optional<Node> loadNodeWithTrace(const std::shared_ptr<storage::DataManager> &dm, int64_t nodeId) {
			if (debug::PerfTrace::isEnabled()) {
				const auto start = Clock::now();
				Node node = dm->getNode(nodeId);
				debug::PerfTrace::addDuration("node_scan.load_nodes", elapsedNs(start));
				return node;
			}
			return dm->getNode(nodeId);
		}

		bool isDenseEnoughForBulkLoad(const std::vector<int64_t> &candidateIds, size_t begin, size_t end) {
			static constexpr size_t BULK_NODE_LOAD_THRESHOLD = 4096;
			static constexpr int64_t MAX_RANGE_TO_ROW_FACTOR = 4;

			const size_t rowCount = end - begin;
			if (rowCount < BULK_NODE_LOAD_THRESHOLD) {
				return false;
			}

			auto [minIt, maxIt] = std::minmax_element(candidateIds.begin() + begin, candidateIds.begin() + end);
			if (*minIt <= 0) {
				return false;
			}
			const auto span = static_cast<uint64_t>(*maxIt - *minIt + 1);
			const auto maxAllowedSpan = static_cast<uint64_t>(rowCount) * MAX_RANGE_TO_ROW_FACTOR;
			return span <= maxAllowedSpan;
		}

		bool canUseBulkNodeLoad(const std::shared_ptr<storage::DataManager> &dm,
			                        const std::vector<int64_t> &candidateIds,
			                        size_t begin,
			                        size_t end) {
			return dm->hasPreadSupport() && !dm->hasUnsavedChanges() && dm->getCurrentSnapshot() == nullptr &&
			       isDenseEnoughForBulkLoad(candidateIds, begin, end);
		}

		std::optional<std::vector<Node>> bulkLoadNodesAligned(
				const std::shared_ptr<storage::DataManager> &dm,
				const std::vector<int64_t> &candidateIds,
				size_t begin,
				size_t end) {
			if (!canUseBulkNodeLoad(dm, candidateIds, begin, end)) {
				return std::nullopt;
			}

			const bool traceEnabled = debug::PerfTrace::isEnabled();
			const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

			auto [minIt, maxIt] = std::minmax_element(candidateIds.begin() + begin, candidateIds.begin() + end);
			const int64_t minId = *minIt;
			const int64_t maxId = *maxIt;
			auto loaded = dm->bulkLoadEntities<Node>(minId, maxId);

			std::unordered_map<int64_t, Node> byId;
			byId.reserve(loaded.size());
			for (auto &node : loaded) {
				byId.emplace(node.getId(), std::move(node));
			}

			if (byId.empty()) {
				return std::nullopt;
			}

			std::vector<Node> aligned;
			aligned.reserve(end - begin);
			for (size_t index = begin; index < end; ++index) {
				auto nodeIt = byId.find(candidateIds[index]);
				aligned.push_back(nodeIt != byId.end() ? std::move(nodeIt->second) : Node{});
			}

			if (traceEnabled) {
				const auto elapsed = elapsedNs(start);
				debug::PerfTrace::addDuration("node_scan.load_nodes", elapsed);
				debug::PerfTrace::addDuration("node_scan.bulk_load_nodes", elapsed);
			}
			return aligned;
		}

		bool matchesMetadataLabels(const NodeMetadataBatch &metadataBatch,
		                           size_t row,
		                           const std::vector<int64_t> &labelIds) {
			for (const int64_t labelId : labelIds) {
				if (!metadataBatch.hasLabelId(row, labelId)) {
					return false;
				}
			}
			return true;
		}
	} // namespace

	NodeBatchLoader::NodeBatchLoader(std::shared_ptr<storage::DataManager> dm,
	                                 concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	NodeColumnBatch NodeBatchLoader::load(const std::vector<int64_t> &candidateIds,
	                                      size_t begin,
	                                      size_t end,
	                                      const NodeScanConfig &config,
	                                      const NodeScanRequirements &requirements) const {
		const size_t clampedEnd = std::min(end, candidateIds.size());

		NodeColumnBatch batch;
		if (begin >= clampedEnd) {
			return batch;
		}

		const size_t rowCount = clampedEnd - begin;
		batch.nodeIds.reserve(rowCount);
		batch.selected.reserve(rowCount);
		std::vector<Node> loadedNodes;
		loadedNodes.reserve(rowCount);

		const bool needsPropertyColumns = requirements.materialization == NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
		if (requirements.needsFullNode()) {
			batch.materializedNodes.reserve(rowCount);
		}

		std::vector<int64_t> requiredLabelIds;
		if (requirements.needsLabels) {
			requiredLabelIds.reserve(config.labels.size());
			for (const auto &label : config.labels) {
				const int64_t labelId = dm_->resolveTokenId(label);
				requiredLabelIds.push_back(labelId == 0 ? -1 : labelId);
			}
		}

		std::optional<NodeMetadataBatch> metadataBatch;
		if (requirements.materialization == NodeMaterializationMode::NSM_SELECTED_PROPERTIES) {
			NodeMetadataColumnLoader metadataLoader(dm_);
			metadataBatch = metadataLoader.loadBatch(candidateIds, begin, clampedEnd);
		}

		auto bulkNodes = metadataBatch.has_value() ?
		                 std::optional<std::vector<Node>>{} :
		                 bulkLoadNodesAligned(dm_, candidateIds, begin, clampedEnd);
		const bool usingMetadataBatch = metadataBatch.has_value();
		const bool usingBulkNodes = bulkNodes.has_value();

		for (size_t index = begin; index < clampedEnd; ++index) {
			const int64_t nodeId = candidateIds[index];
			std::optional<Node> maybeNode;
			const size_t row = index - begin;
			if (!usingMetadataBatch && !usingBulkNodes) {
				maybeNode = loadNodeWithTrace(dm_, nodeId);
			}

			bool selected = false;
			if (usingMetadataBatch) {
				selected = metadataBatch->isValid(row);
				if (requirements.needsActiveCheck && metadataBatch->active[row] == 0) {
					selected = false;
				}
			} else {
				Node &node = usingBulkNodes ? (*bulkNodes)[row] : *maybeNode;
				selected = node.getId() != 0;
				if (requirements.needsActiveCheck && !node.isActive()) {
					selected = false;
				}
			}

			if (requirements.needsLabels) {
				if (debug::PerfTrace::isEnabled()) {
					const auto labelStart = Clock::now();
					if (selected) {
						const bool labelsMatch = usingMetadataBatch ?
						                         matchesMetadataLabels(*metadataBatch, row, requiredLabelIds) :
						                         matchesLabels(usingBulkNodes ? (*bulkNodes)[row] : *maybeNode, requiredLabelIds);
						if (!labelsMatch) {
							selected = false;
						}
					}
					debug::PerfTrace::addDuration("node_scan.label_check", elapsedNs(labelStart));
				} else if (selected) {
					const bool labelsMatch = usingMetadataBatch ?
					                         matchesMetadataLabels(*metadataBatch, row, requiredLabelIds) :
					                         matchesLabels(usingBulkNodes ? (*bulkNodes)[row] : *maybeNode, requiredLabelIds);
					if (!labelsMatch) {
						selected = false;
					}
				}
			}

			std::unordered_map<std::string, PropertyValue> properties;
			if (selected && requirements.needsFullNode()) {
				if (debug::PerfTrace::isEnabled()) {
					const auto propStart = Clock::now();
					properties = dm_->getNodeProperties(nodeId);
					debug::PerfTrace::addDuration("node_scan.load_properties", elapsedNs(propStart));
				} else {
					properties = dm_->getNodeProperties(nodeId);
				}
			}

			batch.nodeIds.push_back(nodeId);
			batch.selected.push_back(selected ? uint8_t{1} : uint8_t{0});
			if (!usingMetadataBatch) {
				Node &node = usingBulkNodes ? (*bulkNodes)[row] : *maybeNode;
				loadedNodes.push_back(node);

				if (requirements.needsFullNode() && selected) {
					node.setProperties(std::move(properties));
					batch.materializedNodes.push_back(std::move(node));
				}
			}
		}

		if (needsPropertyColumns) {
			NodePropertyColumnLoader propertyLoader(dm_, threadPool_);
			if (debug::PerfTrace::isEnabled()) {
				const auto propStart = Clock::now();
				batch.propertyColumns = usingMetadataBatch ?
				                        propertyLoader.loadColumns(*metadataBatch, batch.selected, requirements.requiredProperties) :
				                        propertyLoader.loadColumns(loadedNodes, batch.selected, requirements.requiredProperties);
				debug::PerfTrace::addDuration("node_scan.load_properties", elapsedNs(propStart));
			} else {
				batch.propertyColumns = usingMetadataBatch ?
				                        propertyLoader.loadColumns(*metadataBatch, batch.selected, requirements.requiredProperties) :
				                        propertyLoader.loadColumns(loadedNodes, batch.selected, requirements.requiredProperties);
			}
		}

		return batch;
	}

	bool NodeBatchLoader::matchesLabels(const Node &node, const std::vector<int64_t> &labelIds) const {
		for (const int64_t labelId : labelIds) {
			if (labelId <= 0 || !node.hasLabelId(labelId)) {
				return false;
			}
		}
		return true;
	}

} // namespace graph::query::execution
