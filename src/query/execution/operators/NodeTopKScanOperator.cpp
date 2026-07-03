#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodePropertyColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/StorageScalarValueAdapter.hpp"

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

	NodeTopKScanOperator::NodeTopKScanOperator(std::shared_ptr<storage::DataManager> dm,
													   std::shared_ptr<indexes::IndexManager> im, NodeScanConfig config,
													   NodeScanRequirements requirements,
													   std::vector<VectorizedPropertyPredicate> predicates,
													   std::vector<NodeTopKProjection> projections,
													   std::string sortProperty, bool ascending, int64_t limit,
													   std::vector<ExplainAttribute> explainAttributes) :
		dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)), requirements_(std::move(requirements)),
		predicates_(std::move(predicates)), projections_(std::move(projections)),
		sortProperty_(std::move(sortProperty)), ascending_(ascending), limit_(normalizeLimit(limit)),
		explainAttributes_(std::move(explainAttributes)) {}

	void NodeTopKScanOperator::open() {
		candidateSet_ = NodeCandidateSet{};
		outputRows_.clear();
		currentOutputIndex_ = 0;
		NodeCandidateSource source(dm_, im_);
		candidateSet_ = source.collectWithMetadata(config_);
		built_ = false;
	}

	std::optional<RecordBatch> NodeTopKScanOperator::next() {
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

	void NodeTopKScanOperator::close() {
		candidateSet_ = NodeCandidateSet{};
		outputRows_.clear();
		currentOutputIndex_ = 0;
		built_ = false;
	}

	std::vector<std::string> NodeTopKScanOperator::getOutputVariables() const {
		std::vector<std::string> variables;
		variables.reserve(projections_.size());
		for (const auto &projection: projections_) {
			variables.push_back(projection.alias);
		}
		return variables;
	}

	std::string NodeTopKScanOperator::toString() const {
		return "NodeTopKScan(" + config_.variable + "." + sortProperty_ +
			   (ascending_ ? " ASC LIMIT " : " DESC LIMIT ") + std::to_string(limit_) + ")";
	}

	void NodeTopKScanOperator::buildTopK() {
		const auto start = Clock::now();
		if (limit_ == 0 || projections_.empty()) {
			if (debug::PerfTrace::isEnabled()) {
				debug::PerfTrace::addDuration("node_scan.topk", elapsedNs(start));
			}
			return;
		}

		std::vector<Row> heap;
		heap.reserve(limit_);

		const bool usedMetadataPath = predicates_.empty() && tryBuildTopKFromMetadata(heap);
		if (!usedMetadataPath) {
			NodeBatchLoader loader(dm_, threadPool_);
			const auto requirements = relaxSatisfiedCandidateChecks(makeSelectionRequirements(), candidateSet_);
			for (size_t begin = 0; begin < candidateSet_.ids.size();) {
				if (queryContext_) {
					queryContext_->checkGuard();
				}
				const size_t batchSize = chooseColumnarNodeBatchSize(candidateSet_.ids.size() - begin, threadPool_,
																	 PhysicalOperator::DEFAULT_BATCH_SIZE);
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
					candidate.orderKey = TypedOrderKey::from(candidate.sortKey);
					offerTopK(heap, std::move(candidate));
				}
				begin = end;
			}
		}

		auto finalComparator = [this](const Row &left, const Row &right) {
			return comesBefore(left.orderKey, right.orderKey);
		};
		const auto sortStart = Clock::now();
		std::sort(heap.begin(), heap.end(), finalComparator);
		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.topk.final_sort", elapsedNs(sortStart));
		}
		const auto projectionStart = Clock::now();
		loadProjectionValues(heap);
		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.topk.load_projection", elapsedNs(projectionStart));
		}

		outputRows_.reserve(heap.size());
		for (const auto &row: heap) {
			outputRows_.push_back(makeRecord(row));
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.topk", elapsedNs(start));
		}
	}

	bool NodeTopKScanOperator::tryBuildTopKFromMetadata(std::vector<Row> &heap) const {
		if (!dm_ || candidateSet_.ids.empty()) {
			return true;
		}

		std::vector<Row> localHeap;
		localHeap.reserve(limit_);
		const auto requirements = relaxSatisfiedCandidateChecks(makeSelectionRequirements(), candidateSet_);
		const NodeMetadataRowFilter rowFilter(dm_, config_, requirements);
		NodeMetadataColumnLoader metadataLoader(dm_);
		const bool projectsSortProperty =
				std::any_of(projections_.begin(), projections_.end(),
							[&](const auto &projection) { return projection.property == sortProperty_; });

			struct FallbackRow {
			size_t acceptedRow = 0;
			NodeMetadataRow metadata;
		};

		struct MetadataPartitionState {
			std::vector<int64_t> acceptedNodeIds;
			std::vector<int64_t> acceptedPropertyEntityIds;
			std::vector<PropertyStorageType> acceptedPropertyStorageTypes;
			std::vector<uint8_t> sortKeySeen;
			std::vector<int64_t> propertyEntityIds;
			std::vector<size_t> propertyRows;
			std::vector<FallbackRow> fallbackRows;

			void reserve(size_t rowCount) {
				acceptedNodeIds.reserve(rowCount);
				acceptedPropertyEntityIds.reserve(rowCount);
				acceptedPropertyStorageTypes.reserve(rowCount);
				sortKeySeen.reserve(rowCount);
				propertyEntityIds.reserve(rowCount);
				propertyRows.reserve(rowCount);
			}
		};

		static constexpr size_t kMetadataTopKBatchSize = 65536;
		for (size_t begin = 0; begin < candidateSet_.ids.size();) {
			if (queryContext_) {
				queryContext_->checkGuard();
			}
			const size_t end = std::min(candidateSet_.ids.size(), begin + kMetadataTopKBatchSize);

			std::vector<int64_t> acceptedNodeIds;
			std::vector<int64_t> acceptedPropertyEntityIds;
			std::vector<PropertyStorageType> acceptedPropertyStorageTypes;
			std::vector<uint8_t> sortKeySeen;
			std::vector<int64_t> propertyEntityIds;
			std::vector<size_t> propertyRows;
			std::vector<FallbackRow> fallbackRows;
			std::vector<MetadataPartitionState> partitions;

			const bool visited = metadataLoader.visitBatchPartitioned(
					candidateSet_.ids, begin, end,
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
						const size_t acceptedRow = state.acceptedNodeIds.size();
						state.acceptedNodeIds.push_back(metadata.nodeId);
						state.acceptedPropertyEntityIds.push_back(metadata.propertyEntityId);
						state.acceptedPropertyStorageTypes.push_back(metadata.propertyStorageType);
						state.sortKeySeen.push_back(uint8_t{0});

						const int64_t propertyEntityId = metadata.propertyEntityId;
						if (propertyEntityId == 0) {
							return true;
						}
						const auto storageType = metadata.propertyStorageType;
						if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
							state.propertyEntityIds.push_back(propertyEntityId);
							state.propertyRows.push_back(acceptedRow);
						} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
							state.fallbackRows.push_back({acceptedRow, metadata});
						}
						return true;
					},
					threadPool_);
			if (!visited) {
				return false;
			}

			size_t acceptedRowCount = 0;
			size_t propertyRowCount = 0;
			size_t fallbackRowCount = 0;
			for (const auto &partition : partitions) {
				acceptedRowCount += partition.acceptedNodeIds.size();
				propertyRowCount += partition.propertyEntityIds.size();
				fallbackRowCount += partition.fallbackRows.size();
			}
			acceptedNodeIds.reserve(acceptedRowCount);
			acceptedPropertyEntityIds.reserve(acceptedRowCount);
			acceptedPropertyStorageTypes.reserve(acceptedRowCount);
			sortKeySeen.reserve(acceptedRowCount);
			propertyEntityIds.reserve(propertyRowCount);
			propertyRows.reserve(propertyRowCount);
			fallbackRows.reserve(fallbackRowCount);
			for (const auto &partition : partitions) {
				const size_t acceptedOffset = acceptedNodeIds.size();
				acceptedNodeIds.insert(acceptedNodeIds.end(),
									   partition.acceptedNodeIds.begin(),
									   partition.acceptedNodeIds.end());
				acceptedPropertyEntityIds.insert(acceptedPropertyEntityIds.end(),
												 partition.acceptedPropertyEntityIds.begin(),
												 partition.acceptedPropertyEntityIds.end());
				acceptedPropertyStorageTypes.insert(acceptedPropertyStorageTypes.end(),
													partition.acceptedPropertyStorageTypes.begin(),
													partition.acceptedPropertyStorageTypes.end());
				sortKeySeen.insert(sortKeySeen.end(), partition.sortKeySeen.begin(), partition.sortKeySeen.end());
				propertyEntityIds.insert(propertyEntityIds.end(),
										partition.propertyEntityIds.begin(),
										partition.propertyEntityIds.end());
				for (const size_t propertyRow : partition.propertyRows) {
					propertyRows.push_back(acceptedOffset + propertyRow);
				}
				for (const auto &fallback : partition.fallbackRows) {
					fallbackRows.push_back({acceptedOffset + fallback.acceptedRow, fallback.metadata});
				}
			}

			if (!propertyEntityIds.empty()) {
				std::vector<std::vector<Row>> partitionHeaps;
				(void) dm_->bulkVisitPropertyEntityScalarValuesPartitioned(
						propertyEntityIds, propertyRows, acceptedNodeIds.size(), sortProperty_,
						[&](size_t partitionCount) {
							partitionHeaps.resize(partitionCount);
							for (auto &partitionHeap : partitionHeaps) {
								partitionHeap.reserve(limit_);
							}
						},
						[&](size_t partition, size_t acceptedRow, const storage::PropertyEntityScalarValue &value) {
							if (acceptedRow >= acceptedNodeIds.size()) { // ZYX_COV_EXCL_LINE: rows are supplied from acceptedNodeIds indexes.
								return;
							}
							const auto scalar = scalarValueFromStorage(value);
							Row candidate;
							candidate.nodeId = acceptedNodeIds[acceptedRow];
							candidate.propertyEntityId = acceptedPropertyEntityIds[acceptedRow];
							candidate.propertyStorageType = acceptedPropertyStorageTypes[acceptedRow];
							candidate.metadataLoaded = true;
							if (projectsSortProperty) {
								candidate.sortKey = propertyValueFromScalar(scalar);
							}
							candidate.orderKey = orderKeyFromScalar(scalar);
							sortKeySeen[acceptedRow] = 1;
							offerTopK(partitionHeaps[partition], std::move(candidate));
						},
						threadPool_);
				const auto mergeStart = Clock::now();
				for (auto &partitionHeap : partitionHeaps) {
					for (auto &candidate : partitionHeap) {
						offerTopK(localHeap, std::move(candidate));
					}
				}
				if (debug::PerfTrace::isEnabled()) {
					debug::PerfTrace::addDuration("node_scan.topk.merge_partition_heaps", elapsedNs(mergeStart));
				}
			}

				for (const auto &fallback: fallbackRows) {
					Node node = fallback.metadata.toNode();
					const auto properties = dm_->getNodePropertiesDirect(node);
					if (auto it = properties.find(sortProperty_); it != properties.end()) {
						Row candidate;
						candidate.nodeId = acceptedNodeIds[fallback.acceptedRow];
						candidate.propertyEntityId = fallback.metadata.propertyEntityId;
						candidate.propertyStorageType = fallback.metadata.propertyStorageType;
						candidate.metadataLoaded = true;
						candidate.sortKey = it->second;
						candidate.orderKey = TypedOrderKey::from(it->second);
						sortKeySeen[fallback.acceptedRow] = 1;
						offerTopK(localHeap, std::move(candidate));
					}
			}

			for (size_t row = 0; row < acceptedNodeIds.size(); ++row) {
				if (sortKeySeen[row] != 0) {
					continue;
				}
				Row candidate;
				candidate.nodeId = acceptedNodeIds[row];
				candidate.metadataLoaded = true;
				offerTopK(localHeap, std::move(candidate));
			}
			begin = end;
		}

		heap = std::move(localHeap);
		return true;
	}

	NodeScanRequirements NodeTopKScanOperator::makeSelectionRequirements() const {
		NodeScanRequirements requirements = requirements_;
		requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
		requirements.requiredProperties.clear();
		addRequiredProperty(requirements.requiredProperties, sortProperty_);
		for (const auto &predicate: predicates_) {
			addRequiredProperty(requirements.requiredProperties, predicate.propertyKey);
		}
		return requirements;
	}

	void NodeTopKScanOperator::offerTopK(std::vector<Row> &heap, Row candidate) const {
		auto heapComparator = [this](const Row &left, const Row &right) {
			// comesBefore() is final output order; the heap root is the worst retained row.
			return comesBefore(left.orderKey, right.orderKey);
		};

		if (heap.size() < limit_) {
			heap.push_back(std::move(candidate));
			std::push_heap(heap.begin(), heap.end(), heapComparator);
		} else if (comesBefore(candidate.orderKey, heap.front().orderKey)) {
			std::pop_heap(heap.begin(), heap.end(), heapComparator);
			heap.back() = std::move(candidate);
			std::push_heap(heap.begin(), heap.end(), heapComparator);
		}
	}

	void NodeTopKScanOperator::loadProjectionValues(std::vector<Row> &rows) const {
		if (rows.empty()) {
			return;
		}

		std::vector<std::string> projectionProperties;
		projectionProperties.reserve(projections_.size());
		for (const auto &projection: projections_) {
			if (projection.property != sortProperty_) {
				addRequiredProperty(projectionProperties, projection.property);
			}
		}

		std::vector<std::unordered_map<std::string, PropertyValue>> valuesByRow(rows.size());
		if (!projectionProperties.empty()) {
			std::vector<int64_t> directPropertyIds;
			std::vector<size_t> directRows;
			std::vector<int64_t> fallbackNodeIds;
			std::vector<size_t> fallbackRows;
			std::vector<size_t> blobRows;
			directPropertyIds.reserve(rows.size());
			directRows.reserve(rows.size());
			fallbackNodeIds.reserve(rows.size());
			fallbackRows.reserve(rows.size());

			for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
				const auto &row = rows[rowIndex];
				if (row.metadataLoaded) {
					if (row.propertyStorageType == PropertyStorageType::PROPERTY_ENTITY && row.propertyEntityId != 0) {
						directPropertyIds.push_back(row.propertyEntityId);
						directRows.push_back(rowIndex);
					} else if (row.propertyStorageType == PropertyStorageType::BLOB_ENTITY && row.propertyEntityId != 0) {
						blobRows.push_back(rowIndex);
					}
				} else {
					fallbackNodeIds.push_back(row.nodeId);
					fallbackRows.push_back(rowIndex);
				}
			}

			if (!directPropertyIds.empty()) {
				const auto directValues =
						dm_->bulkLoadPropertyEntityValues(directPropertyIds, projectionProperties, threadPool_);
				for (size_t index = 0; index < directPropertyIds.size(); ++index) {
					auto valuesIt = directValues.find(directPropertyIds[index]);
					if (valuesIt != directValues.end()) {
						valuesByRow[directRows[index]] = valuesIt->second;
					}
				}
			}

			for (const size_t rowIndex: blobRows) {
				Node node;
				auto &metadata = node.getMutableMetadata();
				metadata.id = rows[rowIndex].nodeId;
				metadata.propertyEntityId = rows[rowIndex].propertyEntityId;
				metadata.propertyStorageType = static_cast<uint32_t>(rows[rowIndex].propertyStorageType);
				metadata.isActive = true;
				const auto properties = dm_->getNodePropertiesDirect(node);
				auto &values = valuesByRow[rowIndex];
				for (const auto &property: projectionProperties) {
					if (auto valueIt = properties.find(property); valueIt != properties.end()) {
						values.emplace(property, valueIt->second);
					}
				}
			}

			if (!fallbackNodeIds.empty()) {
				const auto nodes = dm_->getNodeBatch(fallbackNodeIds);
				std::unordered_map<int64_t, size_t> fallbackRowByNodeId;
				fallbackRowByNodeId.reserve(fallbackRows.size());
				for (size_t index = 0; index < fallbackRows.size(); ++index) {
					fallbackRowByNodeId.emplace(fallbackNodeIds[index], fallbackRows[index]);
				}

				// getNodeBatch() filters missing/inactive ids, so map returned nodes back to retained rows by id.
				std::vector<uint8_t> selected(nodes.size(), uint8_t{1});
				NodePropertyColumnLoader propertyLoader(dm_, threadPool_);
				const auto columns = propertyLoader.loadColumns(nodes, selected, projectionProperties);
				for (size_t nodeRow = 0; nodeRow < nodes.size(); ++nodeRow) {
					auto rowIt = fallbackRowByNodeId.find(nodes[nodeRow].getId());
					if (rowIt == fallbackRowByNodeId.end()) {
						continue;
					}
					auto &values = valuesByRow[rowIt->second];
					for (const auto &[key, column]: columns) {
						if (column[nodeRow].has_value()) {
							values.emplace(key, *column[nodeRow]);
						}
					}
				}
			}
		}

		for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
			auto &row = rows[rowIndex];
			row.values.clear();
			row.values.reserve(projections_.size());
			const auto &projectedValues = valuesByRow[rowIndex];
			for (const auto &projection: projections_) {
				if (projection.property == sortProperty_) {
					row.values.push_back(row.sortKey);
				} else {
					const auto valueIt = projectedValues.find(projection.property);
					row.values.push_back(valueIt != projectedValues.end() ? valueIt->second : PropertyValue());
				}
			}
		}
	}

	bool NodeTopKScanOperator::comesBefore(const TypedOrderKey &left, const TypedOrderKey &right) const {
		const int comparison = left.compare(right);
		if (comparison == 0) {
			return false;
		}
		return ascending_ ? comparison < 0 : comparison > 0;
	}

	Record NodeTopKScanOperator::makeRecord(const Row &row) const {
		Record record;
		for (size_t index = 0; index < projections_.size(); ++index) {
			record.setValue(projections_[index].alias, PropertyValue(row.values[index]));
		}
		return record;
	}

	size_t NodeTopKScanOperator::normalizeLimit(int64_t limit) {
		if (limit <= 0) {
			return 0;
		}
		const auto unsignedLimit = static_cast<uint64_t>(limit);
		const auto maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
		return static_cast<size_t>(std::min(unsignedLimit, maxSize));
	}

} // namespace graph::query::execution::operators
