/**
 * @file IndexBuilder.cpp
 * @author Nexepic
 * @date 2025/6/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/PersistenceManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/PropertyIndexBuildScanner.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/indexes/ScopedNodePropertyKey.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace graph::query::indexes {
namespace {

	struct PreparedNodePropertyIndexSpec {
		std::string propertyKey;
		std::string label;
		std::string physicalKey;
		int64_t scopedLabelId = 0;
		bool buildGlobalProperty = true;
	};

	struct NodePropertyIndexScanGroup {
		bool global = false;
		int64_t scopedLabelId = 0;
		std::vector<size_t> specIndexes;
		std::vector<std::string> requestedKeys;
	};

	struct ActiveIdRangeTask {
		int64_t startId = 0;
		int64_t endId = 0;
	};

	struct ActiveIdCollectState {
		std::vector<int64_t> ids;
	};

	void appendUnique(std::vector<std::string> &values, const std::string &value) {
		if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
			values.push_back(value);
		}
	}

	PropertyIndex::TypedPropertyEntry makeTypedPropertyEntry(
			const storage::PropertyEntityOwnerScalarKeyValue &ownerValue,
			const std::string &physicalKey) {
		PropertyIndex::TypedPropertyEntry entry;
		entry.entityId = ownerValue.ownerId;
		entry.key = physicalKey;
		entry.type = ownerValue.type;
		entry.boolValue = ownerValue.boolValue;
		entry.intValue = ownerValue.intValue;
		entry.doubleValue = ownerValue.doubleValue;
		entry.stringValue = ownerValue.stringValue;
		return entry;
	}

	struct TypedEntryBuildState {
		std::vector<PropertyIndex::TypedPropertyEntry> entries;
		std::vector<std::string> physicalKeys;
	};

	template<typename PhysicalKeyCollector>
	std::vector<PropertyIndex::TypedPropertyEntry> buildTypedPropertyEntries(
			const std::vector<storage::PropertyEntityOwnerScalarKeyValue> &values,
			concurrent::ThreadPool *threadPool,
			std::string_view phase,
			PhysicalKeyCollector &&collector) {
		std::vector<PropertyIndex::TypedPropertyEntry> entries;
		if (values.empty()) {
			return entries;
		}
		entries.reserve(values.size());

		const concurrent::ParallelOperatorOptions options{
				.phase = phase,
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
				.estimatedItems = values.size(),
				.minPartitions = 2,
				.minItems = 4096,
				.minItemsPerWorker = 1024};
		(void) concurrent::ParallelOperatorExecutor::runRangePartitions<TypedEntryBuildState>(
				0,
				values.size(),
				threadPool,
				options,
				[&](const concurrent::ParallelRangePartition &range, TypedEntryBuildState &state) {
					state.entries.reserve(range.size());
					for (size_t index = range.begin; index < range.end; ++index) {
						const auto &ownerValue = values[index];
						state.physicalKeys.clear();
						collector(ownerValue, state.physicalKeys);
						for (const auto &physicalKey: state.physicalKeys) {
							state.entries.push_back(makeTypedPropertyEntry(ownerValue, physicalKey));
						}
					}
				},
				[&](size_t, TypedEntryBuildState &state) {
					entries.insert(
							entries.end(),
							std::make_move_iterator(state.entries.begin()),
							std::make_move_iterator(state.entries.end()));
				});
		return entries;
	}

	std::vector<int64_t> sortedUniqueIds(std::vector<int64_t> ids) {
		std::ranges::sort(ids);
		ids.erase(std::ranges::unique(ids).begin(), ids.end());
		return ids;
	}

	void appendUncheckpointedTailRange(std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t maxAllocatedId) {
		if (maxAllocatedId <= 0) {
			return;
		}

		int64_t maxCoveredId = 0;
		for (const auto &[startId, endId]: ranges) {
			if (endId >= startId) {
				maxCoveredId = (std::max)(maxCoveredId, endId);
			}
		}
		if (maxAllocatedId > maxCoveredId) {
			ranges.emplace_back(maxCoveredId + 1, maxAllocatedId);
		}
	}

	bool rangesContainId(const std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t id) {
		return std::any_of(ranges.begin(), ranges.end(), [id](const auto &range) {
			return range.first <= id && id <= range.second;
		});
	}

	template<typename Entity>
	void appendDirtyEntityPointRanges(
			const storage::DataManager &dataManager,
			std::vector<std::pair<int64_t, int64_t>> &ranges) {
		auto persistence = dataManager.getPersistenceManager();
		if (!persistence) {
			return;
		}

		for (const auto &info: persistence->getAllDirtyInfos<Entity>()) {
			if (!info.backup.has_value()) {
				continue;
			}
			const int64_t id = info.backup->getId();
			if (id <= 0 || rangesContainId(ranges, id)) {
				continue;
			}
			ranges.emplace_back(id, id);
		}

		std::ranges::sort(ranges, [](const auto &lhs, const auto &rhs) {
			if (lhs.first != rhs.first) {
				return lhs.first < rhs.first;
			}
			return lhs.second < rhs.second;
		});
	}

	void appendUniqueId(std::vector<int64_t> &ids, std::unordered_set<int64_t> &seen, int64_t id) {
		if (id > 0 && seen.insert(id).second) {
			ids.push_back(id);
		}
	}

	std::vector<ActiveIdRangeTask> buildActiveIdRangeTasks(
			const std::vector<std::pair<int64_t, int64_t>> &ranges,
			size_t targetIdsPerTask = 4096) {
		std::vector<ActiveIdRangeTask> tasks;
		targetIdsPerTask = std::max<size_t>(1, targetIdsPerTask);
		for (const auto &[startId, endId]: ranges) {
			if (endId < startId) {
				continue;
			}
			for (int64_t taskStart = startId; taskStart <= endId;) {
				const auto remaining = static_cast<uint64_t>(endId) - static_cast<uint64_t>(taskStart) + 1U;
				const auto taskSize = static_cast<int64_t>(std::min<uint64_t>(remaining, targetIdsPerTask));
				const int64_t taskEnd = taskStart + taskSize - 1;
				tasks.push_back({taskStart, taskEnd});
				if (taskEnd == std::numeric_limits<int64_t>::max()) { // ZYX_COV_EXCL_LINE
					break; // ZYX_COV_EXCL_LINE
				}
				taskStart = taskEnd + 1;
			}
		}
		return tasks;
	}

	void appendActiveDirtyNodeIdsByLabel(
			const storage::DataManager &dataManager,
			std::unordered_map<int64_t, std::vector<int64_t>> &idsByLabel) {
		auto persistence = dataManager.getPersistenceManager();
		if (!persistence || idsByLabel.empty()) {
			return;
		}

		std::unordered_map<int64_t, std::unordered_set<int64_t>> seenByLabel;
		seenByLabel.reserve(idsByLabel.size());
		for (const auto &[labelId, ids]: idsByLabel) {
			auto &seen = seenByLabel[labelId];
			seen.reserve(ids.size());
			for (const int64_t id: ids) {
				seen.insert(id);
			}
		}

		for (const auto &info: persistence->getAllDirtyInfos<Node>()) {
			if (!info.backup.has_value()) {
				continue;
			}
			const Node &node = *info.backup;
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}
			for (const int64_t labelId: node.getLabelIds()) {
				auto labelIt = idsByLabel.find(labelId);
				if (labelIt != idsByLabel.end()) {
					appendUniqueId(labelIt->second, seenByLabel[labelId], node.getId());
				}
			}
		}
	}

	std::vector<int64_t> collectActiveNodeIds(
			const storage::DataManager &dataManager,
			const std::vector<std::pair<int64_t, int64_t>> &ranges,
			graph::concurrent::ThreadPool *threadPool) {
		std::vector<int64_t> ids;
		const auto tasks = buildActiveIdRangeTasks(ranges);
		const size_t estimatedItems = std::accumulate(tasks.begin(), tasks.end(), size_t{0}, [](size_t total, const auto &task) {
			return total + static_cast<size_t>(task.endId - task.startId + 1);
		});
		const graph::concurrent::ParallelOperatorOptions options{
				.phase = "index_build.collect_active_nodes",
				.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_STORAGE_SCAN,
				.estimatedItems = estimatedItems,
				.minPartitions = 2,
				.minItems = 4096,
				.minItemsPerWorker = 1024};
		(void) graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<ActiveIdCollectState>(
				tasks.size(),
				threadPool,
				options,
				[&](size_t taskIndex, ActiveIdCollectState &state) {
					const auto &task = tasks[taskIndex];
					state.ids.reserve(static_cast<size_t>(task.endId - task.startId + 1));
					for (int64_t id = task.startId; id <= task.endId; ++id) {
						Node node = dataManager.getNode(id);
						if (node.getId() != 0 && node.isActive()) {
							state.ids.push_back(node.getId());
						}
					}
				},
				[&](size_t, ActiveIdCollectState &state) {
					ids.insert(ids.end(), state.ids.begin(), state.ids.end());
				});
		std::ranges::sort(ids);
		ids.erase(std::ranges::unique(ids).begin(), ids.end());
		return ids;
	}

	std::unordered_map<int64_t, std::vector<int64_t>> collectActiveNodeIdsByLabel(
			const storage::DataManager &dataManager,
			const std::vector<std::pair<int64_t, int64_t>> &ranges,
			const std::vector<int64_t> &labelIds,
			const std::shared_ptr<LabelIndex> &labelIndex) {
		std::unordered_map<int64_t, std::vector<int64_t>> idsByLabel;
		for (const int64_t labelId: labelIds) {
			if (labelId != 0) {
				idsByLabel.try_emplace(labelId);
			}
		}
		if (idsByLabel.empty()) {
			return idsByLabel;
		}

		if (labelIndex && labelIndex->hasPhysicalData()) {
			for (auto &[labelId, ids]: idsByLabel) {
				const auto label = dataManager.resolveTokenName(labelId);
				if (!label.empty()) {
					std::vector<int64_t> validatedIds;
					for (const int64_t candidateId: labelIndex->findNodes(label)) {
						Node node = dataManager.getNode(candidateId);
						if (node.getId() != 0 && node.isActive() && node.hasLabelId(labelId)) {
							validatedIds.push_back(node.getId());
						}
					}
					ids = sortedUniqueIds(std::move(validatedIds));
				}
			}
			appendActiveDirtyNodeIdsByLabel(dataManager, idsByLabel);
			for (auto &[labelId, ids]: idsByLabel) {
				(void) labelId;
				ids = sortedUniqueIds(std::move(ids));
			}
			return idsByLabel;
		}

		for (const auto &[startId, endId]: ranges) {
			for (int64_t id = startId; id <= endId; ++id) {
				Node node = dataManager.getNode(id);
				if (node.getId() == 0 || !node.isActive()) {
					continue;
				}
				for (const int64_t labelId: node.getLabelIds()) {
					if (auto labelIt = idsByLabel.find(labelId); labelIt != idsByLabel.end()) {
						labelIt->second.push_back(node.getId());
					}
				}
			}
		}
		for (auto &[labelId, ids]: idsByLabel) {
			(void) labelId;
			ids = sortedUniqueIds(std::move(ids));
		}
		return idsByLabel;
	}

	std::vector<int64_t> collectActiveEdgeIds(
			const storage::DataManager &dataManager,
			const std::vector<std::pair<int64_t, int64_t>> &ranges,
			graph::concurrent::ThreadPool *threadPool) {
		std::vector<int64_t> ids;
		const auto tasks = buildActiveIdRangeTasks(ranges);
		const size_t estimatedItems = std::accumulate(tasks.begin(), tasks.end(), size_t{0}, [](size_t total, const auto &task) {
			return total + static_cast<size_t>(task.endId - task.startId + 1);
		});
		const graph::concurrent::ParallelOperatorOptions options{
				.phase = "index_build.collect_active_edges",
				.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_STORAGE_SCAN,
				.estimatedItems = estimatedItems,
				.minPartitions = 2,
				.minItems = 4096,
				.minItemsPerWorker = 1024};
		(void) graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<ActiveIdCollectState>(
				tasks.size(),
				threadPool,
				options,
				[&](size_t taskIndex, ActiveIdCollectState &state) {
					const auto &task = tasks[taskIndex];
					state.ids.reserve(static_cast<size_t>(task.endId - task.startId + 1));
					for (int64_t id = task.startId; id <= task.endId; ++id) {
						Edge edge = dataManager.getEdge(id);
						if (edge.getId() != 0 && edge.isActive()) {
							state.ids.push_back(edge.getId());
						}
					}
				},
				[&](size_t, ActiveIdCollectState &state) {
					ids.insert(ids.end(), state.ids.begin(), state.ids.end());
				});
		std::ranges::sort(ids);
		ids.erase(std::ranges::unique(ids).begin(), ids.end());
		return ids;
	}

} // namespace

	IndexBuilder::IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							   std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {
	}

	IndexBuilder::~IndexBuilder() = default;

	bool IndexBuilder::buildNodeLabelIndex() const {
		try {
			auto labelIndex = indexManager_->getNodeIndexManager()->getLabelIndex();
			// Clear existing data to rebuild
			labelIndex->clear();

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processNodeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processNodeBatch(batchIds, labelIndex, nullptr, "");
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildEdgeTypeIndex() const {
		try {
			auto labelIndex = indexManager_->getEdgeIndexManager()->getLabelIndex();
			// Clear existing data to rebuild
			labelIndex->clear();

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processEdgeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processEdgeBatch(batchIds, labelIndex, nullptr, "");
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key) const {
		return buildNodePropertyIndex(key, "");
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key, const std::string &label) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();
			const int64_t scopedLabelId = label.empty() ? 0 : dataManager_->resolveTokenId(label);
			const std::string scopedKey = label.empty() ? "" : makeScopedNodePropertyKey(label, key);
			const bool buildGlobalProperty = label.empty();

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			// clearKey would remove the definition; clearIndexData only resets the tree/data.
			if (buildGlobalProperty) {
				propertyIndex->clearIndexData(key);
			}
			if (!scopedKey.empty()) {
				propertyIndex->createIndex(scopedKey);
				propertyIndex->clearIndexData(scopedKey);
			}

			if (buildNodePropertyIndexFromOwnerScan(propertyIndex, key, scopedLabelId, scopedKey, buildGlobalProperty)) {
				propertyIndex->flush();
				return true;
			}

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey,
										 buildGlobalProperty);
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey, buildGlobalProperty);
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndexes(const std::vector<NodePropertyIndexBuildSpec> &specs) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();
			std::vector<PreparedNodePropertyIndexSpec> prepared;
			prepared.reserve(specs.size());

			for (const auto &spec: specs) {
				if (spec.propertyKey.empty()) {
					continue;
				}

				PreparedNodePropertyIndexSpec preparedSpec;
				preparedSpec.propertyKey = spec.propertyKey;
				preparedSpec.label = spec.label;
				preparedSpec.scopedLabelId = spec.label.empty() ? 0 : dataManager_->resolveTokenId(spec.label);
				preparedSpec.buildGlobalProperty = spec.label.empty();
				preparedSpec.physicalKey = preparedSpec.buildGlobalProperty
												   ? preparedSpec.propertyKey
												   : makeScopedNodePropertyKey(spec.label, spec.propertyKey);

				propertyIndex->createIndex(preparedSpec.physicalKey);
				propertyIndex->clearIndexData(preparedSpec.physicalKey);
				prepared.push_back(std::move(preparedSpec));
			}

			if (prepared.empty()) {
				return true;
			}

			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			if (!scanner.canCollect(EntityType::Node)) {
				for (const auto &spec: prepared) {
					if (!buildNodePropertyIndex(spec.propertyKey, spec.label)) {
						return false;
					}
				}
				return true;
			}

			const bool typedScanCoversAllProperties =
					dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Node);

			std::vector<PropertyIndex::TypedPropertyEntry> propertyEntries;
			std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;

			std::vector<NodePropertyIndexScanGroup> scanGroups;
			std::optional<size_t> globalGroupIndex;
			std::unordered_map<int64_t, size_t> scopedGroupByLabel;
			std::vector<int64_t> neededScopedLabels;

			for (size_t specIndex = 0; specIndex < prepared.size(); ++specIndex) {
				const auto &spec = prepared[specIndex];
				size_t groupIndex = 0;
				if (spec.buildGlobalProperty) {
					if (!globalGroupIndex.has_value()) {
						globalGroupIndex = scanGroups.size();
						scanGroups.push_back(NodePropertyIndexScanGroup{true, 0, {}, {}});
					}
					groupIndex = *globalGroupIndex;
				} else {
					auto [it, inserted] = scopedGroupByLabel.try_emplace(spec.scopedLabelId, scanGroups.size());
					if (inserted) {
						scanGroups.push_back(NodePropertyIndexScanGroup{false, spec.scopedLabelId, {}, {}});
						if (spec.scopedLabelId != 0) {
							neededScopedLabels.push_back(spec.scopedLabelId);
						}
					}
					groupIndex = it->second;
				}

				auto &group = scanGroups[groupIndex];
				group.specIndexes.push_back(specIndex);
				appendUnique(group.requestedKeys, spec.propertyKey);
			}

			const auto nodeRanges = getNodeIdRanges();
			std::vector<int64_t> globalOwnerIds;
			if (globalGroupIndex.has_value()) {
				globalOwnerIds = collectActiveNodeIds(*dataManager_, nodeRanges, storage_->getThreadPool());
			}
			const auto scopedOwnerIdsByLabel = collectActiveNodeIdsByLabel(
					*dataManager_, nodeRanges, sortedUniqueIds(std::move(neededScopedLabels)),
					indexManager_->getNodeIndexManager()->getLabelIndex());

				for (const auto &group: scanGroups) {
				std::span<const int64_t> ownerIds;
				if (group.global) {
					ownerIds = std::span<const int64_t>(globalOwnerIds.data(), globalOwnerIds.size());
				} else {
					auto ownerIt = scopedOwnerIdsByLabel.find(group.scopedLabelId);
					if (ownerIt == scopedOwnerIdsByLabel.end()) {
						continue;
					}
					ownerIds = std::span<const int64_t>(ownerIt->second.data(), ownerIt->second.size());
				}
				if (ownerIds.empty() || group.requestedKeys.empty()) {
					continue;
				}

				std::unordered_map<std::string, std::vector<std::string>> physicalKeysByProperty;
				physicalKeysByProperty.reserve(group.specIndexes.size());
				for (const size_t specIndex: group.specIndexes) {
					const auto &spec = prepared[specIndex];
					physicalKeysByProperty[spec.propertyKey].push_back(spec.physicalKey);
				}

				std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
				{
					debug::ScopedPerfTimer timer("index_build.node_property.typed_scan");
					values = scanner.collect(EntityType::Node, group.requestedKeys, ownerIds, storage_->getThreadPool());
				}

				auto groupEntries = buildTypedPropertyEntries(
						values,
						storage_->getThreadPool(),
						"index_build.node_property.typed_entry_build",
						[&](const storage::PropertyEntityOwnerScalarKeyValue &ownerValue,
							std::vector<std::string> &physicalKeys) {
							if (auto keyIt = physicalKeysByProperty.find(ownerValue.key);
								keyIt != physicalKeysByProperty.end()) {
								physicalKeys.insert(
										physicalKeys.end(), keyIt->second.begin(), keyIt->second.end());
							}
						});
				propertyEntries.insert(
						propertyEntries.end(),
						std::make_move_iterator(groupEntries.begin()),
						std::make_move_iterator(groupEntries.end()));

				if (!typedScanCoversAllProperties) {
					debug::ScopedPerfTimer timer("index_build.node_property.fallback_blob_or_inline");
					for (const int64_t id: ownerIds) {
						Node node = dataManager_->getNode(id);
						if (node.getId() == 0 || !node.isActive() ||
							node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
							continue;
						}
						const auto properties = dataManager_->getNodeProperties(node.getId());
						if (properties.empty()) {
							continue;
						}
						for (const auto &[propertyKey, physicalKeys]: physicalKeysByProperty) {
							auto propIt = properties.find(propertyKey);
							if (propIt == properties.end()) {
								continue;
							}
							for (const auto &physicalKey: physicalKeys) {
								fallbackEntries.emplace_back(node.getId(), physicalKey, propIt->second);
							}
						}
					}
				}
			}

			if (!propertyEntries.empty()) {
				debug::ScopedPerfTimer timer("index_build.node_property.typed_insert");
				propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
			}
			if (!fallbackEntries.empty()) {
				propertyIndex->addPropertiesBatch(fallbackEntries);
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	// Specialized builder for a single edge property
	bool IndexBuilder::buildEdgePropertyIndex(const std::string &key) const {
		try {
			auto propertyIndex = indexManager_->getEdgeIndexManager()->getPropertyIndex();

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			propertyIndex->clearIndexData(key);

			if (buildEdgePropertyIndexFromOwnerScan(propertyIndex, key)) {
				propertyIndex->flush();
				return true;
			}

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						processEdgeBatch(batchIds, nullptr, propertyIndex, key);
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processEdgeBatch(batchIds, nullptr, propertyIndex, key);
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildEdgePropertyIndexes(const std::vector<std::string> &keys) const {
		try {
			auto propertyIndex = indexManager_->getEdgeIndexManager()->getPropertyIndex();
			std::vector<std::string> requestedKeys;
			requestedKeys.reserve(keys.size());
			for (const auto &key: keys) {
				if (key.empty() || std::find(requestedKeys.begin(), requestedKeys.end(), key) != requestedKeys.end()) {
					continue;
				}
				propertyIndex->createIndex(key);
				propertyIndex->clearIndexData(key);
				requestedKeys.push_back(key);
			}

			if (requestedKeys.empty()) {
				return true;
			}

			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			if (!scanner.canCollect(EntityType::Edge)) {
				for (const auto &key: requestedKeys) {
					if (!buildEdgePropertyIndex(key)) {
						return false;
					}
				}
				return true;
			}

			const bool typedScanCoversAllProperties =
					dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Edge);

			const auto activeEdgeIds = collectActiveEdgeIds(*dataManager_, getEdgeIdRanges(), storage_->getThreadPool());
			if (activeEdgeIds.empty()) {
				propertyIndex->flush();
				return true;
			}

			std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
			{
				debug::ScopedPerfTimer timer("index_build.edge_property.typed_scan");
				values = scanner.collect(
						EntityType::Edge,
						requestedKeys,
						std::span<const int64_t>(activeEdgeIds.data(), activeEdgeIds.size()),
						storage_->getThreadPool());
			}
			auto propertyEntries = buildTypedPropertyEntries(
					values,
					storage_->getThreadPool(),
					"index_build.edge_property.typed_entry_build",
					[](const storage::PropertyEntityOwnerScalarKeyValue &ownerValue,
					   std::vector<std::string> &physicalKeys) {
						physicalKeys.push_back(ownerValue.key);
					});
			if (!propertyEntries.empty()) {
				debug::ScopedPerfTimer timer("index_build.edge_property.typed_insert");
				propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
			}
			if (!typedScanCoversAllProperties) {
				debug::ScopedPerfTimer timer("index_build.edge_property.fallback_blob_or_inline");
				std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
				fallbackEntries.reserve(requestedKeys.size());
				for (const auto &[startId, endId]: getEdgeIdRanges()) {
					for (int64_t id = startId; id <= endId; ++id) {
						Edge edge = dataManager_->getEdge(id);
						if (edge.getId() == 0 || !edge.isActive() ||
							edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
							continue;
						}
						const auto properties = dataManager_->getEdgeProperties(edge.getId());
						if (properties.empty()) {
							continue;
						}
						for (const auto &key: requestedKeys) {
							auto propIt = properties.find(key);
							if (propIt != properties.end()) {
								fallbackEntries.emplace_back(edge.getId(), key, propIt->second);
							}
						}
					}
				}
				if (!fallbackEntries.empty()) {
					propertyIndex->addPropertiesBatch(fallbackEntries);
				}
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndexFromOwnerScan(
			const std::shared_ptr<PropertyIndex> &propertyIndex,
			const std::string &propertyKey,
			int64_t scopedLabelId,
			const std::string &scopedPropertyKey,
			bool buildGlobalProperty) const {
		if (!propertyIndex || propertyKey.empty() ||
			!storage::PropertyIndexBuildScanner(*dataManager_).canCollect(EntityType::Node)) {
			return false;
		}

		const bool buildScopedEntries = scopedLabelId != 0 && !scopedPropertyKey.empty();
		if (!buildGlobalProperty && !buildScopedEntries) {
			return true;
		}
		const bool typedScanCoversAllProperties =
				dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Node);

		const auto nodeRanges = getNodeIdRanges();
		std::vector<int64_t> ownerIds;
		if (buildGlobalProperty) {
			ownerIds = collectActiveNodeIds(*dataManager_, nodeRanges, storage_->getThreadPool());
		} else {
			auto labelIds = collectActiveNodeIdsByLabel(
					*dataManager_, nodeRanges, std::vector<int64_t>{scopedLabelId},
					indexManager_->getNodeIndexManager()->getLabelIndex());
			if (auto it = labelIds.find(scopedLabelId); it != labelIds.end()) {
				ownerIds = std::move(it->second);
			}
		}
		if (ownerIds.empty()) {
			return true;
		}

		std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
		{
			debug::ScopedPerfTimer timer("index_build.node_property.typed_scan");
			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			values = scanner.collect(
					EntityType::Node,
					std::vector<std::string>{propertyKey},
					std::span<const int64_t>(ownerIds.data(), ownerIds.size()),
					storage_->getThreadPool());
		}

		const std::string &physicalKey = buildGlobalProperty ? propertyKey : scopedPropertyKey;
		auto propertyEntries = buildTypedPropertyEntries(
				values,
				storage_->getThreadPool(),
				"index_build.node_property.typed_entry_build",
				[&](const storage::PropertyEntityOwnerScalarKeyValue &, std::vector<std::string> &physicalKeys) {
					physicalKeys.push_back(physicalKey);
				});
		if (!propertyEntries.empty()) {
			debug::ScopedPerfTimer timer("index_build.node_property.typed_insert");
			propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
		}

		if (!typedScanCoversAllProperties) {
			debug::ScopedPerfTimer timer("index_build.node_property.fallback_blob_or_inline");
			std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
			fallbackEntries.reserve(values.size());
			for (const int64_t id: ownerIds) {
				Node node = dataManager_->getNode(id);
				if (node.getId() == 0 || !node.isActive() ||
					node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
					continue;
				}
				const auto properties = dataManager_->getNodeProperties(node.getId());
				if (auto propIt = properties.find(propertyKey); propIt != properties.end()) {
					fallbackEntries.emplace_back(node.getId(), physicalKey, propIt->second);
				}
			}
			if (!fallbackEntries.empty()) {
				propertyIndex->addPropertiesBatch(fallbackEntries);
			}
		}
		return true;
	}

	bool IndexBuilder::buildEdgePropertyIndexFromOwnerScan(
			const std::shared_ptr<PropertyIndex> &propertyIndex,
			const std::string &propertyKey) const {
		if (!propertyIndex || propertyKey.empty() ||
			!storage::PropertyIndexBuildScanner(*dataManager_).canCollect(EntityType::Edge)) {
			return false;
		}

		const bool typedScanCoversAllProperties =
				dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Edge);

		const auto activeEdgeIds = collectActiveEdgeIds(*dataManager_, getEdgeIdRanges(), storage_->getThreadPool());
		if (activeEdgeIds.empty()) {
			return true;
		}

		std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
		{
			debug::ScopedPerfTimer timer("index_build.edge_property.typed_scan");
			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			values = scanner.collect(
					EntityType::Edge,
					std::vector<std::string>{propertyKey},
					std::span<const int64_t>(activeEdgeIds.data(), activeEdgeIds.size()),
					storage_->getThreadPool());
		}

		auto propertyEntries = buildTypedPropertyEntries(
				values,
				storage_->getThreadPool(),
				"index_build.edge_property.typed_entry_build",
				[&](const storage::PropertyEntityOwnerScalarKeyValue &, std::vector<std::string> &physicalKeys) {
					physicalKeys.push_back(propertyKey);
				});
		if (!propertyEntries.empty()) {
			debug::ScopedPerfTimer timer("index_build.edge_property.typed_insert");
			propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
		}

		if (!typedScanCoversAllProperties) {
			debug::ScopedPerfTimer timer("index_build.edge_property.fallback_blob_or_inline");
			std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
			fallbackEntries.reserve(values.size());
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; ++id) {
					Edge edge = dataManager_->getEdge(id);
					if (edge.getId() == 0 || !edge.isActive() ||
						edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
						continue;
					}
					const auto properties = dataManager_->getEdgeProperties(edge.getId());
					if (auto propIt = properties.find(propertyKey); propIt != properties.end()) {
						fallbackEntries.emplace_back(edge.getId(), propertyKey, propIt->second);
					}
				}
			}
			if (!fallbackEntries.empty()) {
				propertyIndex->addPropertiesBatch(fallbackEntries);
			}
		}
		return true;
	}

	void IndexBuilder::processNodeBatch(const std::vector<int64_t> &nodeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey,
										int64_t scopedLabelId,
										const std::string &scopedPropertyKey,
										bool buildGlobalProperty) const {
		std::unordered_map<int64_t, std::vector<int64_t>> nodeIdsByLabelId;
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propertyEntries;
		if (propertyIndex) {
			propertyEntries.reserve(nodeIds.size());
		}

		for (int64_t id: nodeIds) {
			Node node = dataManager_->getNode(id);
			if (node.getId() == 0 || !node.isActive())
				continue;

			int64_t nodeId = node.getId();

			if (labelIndex) {
				for (const int64_t labelId : node.getLabelIds()) {
					if (labelId != 0) {
						nodeIdsByLabelId[labelId].push_back(nodeId);
					}
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getNodeProperties(nodeId);
				// Index specific property
				if (auto it = properties.find(propertyKey); it != properties.end()) {
					if (buildGlobalProperty) {
						propertyEntries.emplace_back(nodeId, propertyKey, it->second);
					} else if (scopedLabelId != 0 && node.hasLabelId(scopedLabelId) && !scopedPropertyKey.empty()) {
						propertyEntries.emplace_back(nodeId, scopedPropertyKey, it->second);
					}
				}
			}
		}

		if (propertyIndex && !propertyEntries.empty()) {
			propertyIndex->addPropertiesBatch(propertyEntries);
		}

		if (labelIndex && !nodeIdsByLabelId.empty()) {
			std::unordered_map<std::string, std::vector<int64_t>> nodeIdsByLabel;
			nodeIdsByLabel.reserve(nodeIdsByLabelId.size());
			for (auto &[labelId, ids] : nodeIdsByLabelId) {
				auto label = dataManager_->resolveTokenName(labelId);
				if (!label.empty()) {
					nodeIdsByLabel.emplace(std::move(label), std::move(ids));
				}
			}
			if (!nodeIdsByLabel.empty()) {
				labelIndex->addNodesBatch(nodeIdsByLabel);
			}
		}
	}

	void IndexBuilder::processEdgeBatch(const std::vector<int64_t> &edgeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey) const {
		std::unordered_map<int64_t, std::vector<int64_t>> edgeIdsByTypeId;
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propertyEntries;
		if (propertyIndex) {
			propertyEntries.reserve(edgeIds.size());
		}

		for (int64_t id: edgeIds) {
			Edge edge = dataManager_->getEdge(id);
			if (edge.getId() == 0 || !edge.isActive())
				continue;

			int64_t edgeId = edge.getId();

			if (labelIndex) {
				if (edge.getTypeId() != 0) {
					edgeIdsByTypeId[edge.getTypeId()].push_back(edgeId);
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getEdgeProperties(edgeId);
				// Index specific property
				if (auto it = properties.find(propertyKey); it != properties.end()) {
					propertyEntries.emplace_back(edgeId, propertyKey, it->second);
				}
			}
		}

		if (propertyIndex && !propertyEntries.empty()) {
			propertyIndex->addPropertiesBatch(propertyEntries);
		}

		if (labelIndex && !edgeIdsByTypeId.empty()) {
			std::unordered_map<std::string, std::vector<int64_t>> edgeIdsByType;
			edgeIdsByType.reserve(edgeIdsByTypeId.size());
			for (auto &[typeId, ids] : edgeIdsByTypeId) {
				auto type = dataManager_->resolveTokenName(typeId);
				if (!type.empty()) {
					edgeIdsByType.emplace(std::move(type), std::move(ids));
				}
			}
			if (!edgeIdsByType.empty()) {
				labelIndex->addNodesBatch(edgeIdsByType);
			}
		}
	}

	// TODO: Move these to a more appropriate place, like SegmentTracker or DataManager
	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getNodeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all node segments
		auto nodeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Node::typeId);

		for (const auto &segment: nodeSegments) {
			// Create a range from start_id to (start_id + used - 1)
			ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
		}

		if (dataManager_->hasUnsavedChanges()) {
			appendUncheckpointedTailRange(
					ranges, dataManager_->getIdAllocator(EntityType::Node)->getCurrentMaxId());
			appendDirtyEntityPointRanges<Node>(*dataManager_, ranges);
		}

		return ranges;
	}

	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getEdgeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all edge segments
		auto edgeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Edge::typeId);

		for (const auto &segment: edgeSegments) {
			// Create a range from start_id to (start_id + used - 1)
			ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
		}

		if (dataManager_->hasUnsavedChanges()) {
			appendUncheckpointedTailRange(
					ranges, dataManager_->getIdAllocator(EntityType::Edge)->getCurrentMaxId());
			appendDirtyEntityPointRanges<Edge>(*dataManager_, ranges);
		}

		return ranges;
	}

} // namespace graph::query::indexes
