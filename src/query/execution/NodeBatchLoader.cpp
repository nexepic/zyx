#include "graph/query/execution/NodeBatchLoader.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
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

		for (size_t index = begin; index < clampedEnd; ++index) {
			const int64_t nodeId = candidateIds[index];
			auto maybeNode = loadNodeWithTrace(dm_, nodeId);
			Node &node = *maybeNode;

			bool selected = true;
			if (requirements.needsActiveCheck && !node.isActive()) {
				selected = false;
			}

			if (requirements.needsLabels) {
				if (debug::PerfTrace::isEnabled()) {
					const auto labelStart = Clock::now();
					if (selected && !matchesLabels(node, config)) {
						selected = false;
					}
					debug::PerfTrace::addDuration("node_scan.label_check", elapsedNs(labelStart));
				} else if (selected && !matchesLabels(node, config)) {
					selected = false;
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
			loadedNodes.push_back(node);

			if (requirements.needsFullNode() && selected) {
				node.setProperties(std::move(properties));
				batch.materializedNodes.push_back(std::move(node));
			}
		}

		if (needsPropertyColumns) {
			NodePropertyColumnLoader propertyLoader(dm_, threadPool_);
			if (debug::PerfTrace::isEnabled()) {
				const auto propStart = Clock::now();
				batch.propertyColumns = propertyLoader.loadColumns(loadedNodes, batch.selected, requirements.requiredProperties);
				debug::PerfTrace::addDuration("node_scan.load_properties", elapsedNs(propStart));
			} else {
				batch.propertyColumns = propertyLoader.loadColumns(loadedNodes, batch.selected, requirements.requiredProperties);
			}
		}

		return batch;
	}

	bool NodeBatchLoader::matchesLabels(const Node &node, const NodeScanConfig &config) const {
		for (const auto &label : config.labels) {
			const int64_t labelId = dm_->resolveTokenId(label);
			if (labelId == 0 || !node.hasLabelId(labelId)) {
				return false;
			}
		}
		return true;
	}

} // namespace graph::query::execution
