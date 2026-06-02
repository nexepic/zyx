/**
 * @file DataManager.cpp
 * @author Nexepic
 * @date 2025/7/24
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

#include "graph/storage/data/DataManager.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/BlobChainManager.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "graph/storage/dictionaries/TokenRegistry.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {

	// Thread-local snapshot pointer for read-only transactions
	thread_local const CommittedSnapshot *DataManager::currentSnapshot_ = nullptr;
	thread_local bool DataManager::readOnlyMode_ = false;

	DataManager::DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 IDAllocators allocators, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<StorageIO> storageIO) :
		file_(std::move(file)), fileHeader_(fileHeader), pagePool_(std::make_unique<PageBufferPool>(cacheSize)),
		allocators_(std::move(allocators)), segmentTracker_(std::move(segmentTracker)),
		storageIO_(std::move(storageIO)) {

		persistenceManager_ = std::make_shared<PersistenceManager>();
		segmentIndexManager_ = std::make_shared<SegmentIndexManager>(segmentTracker_);
		segmentTracker_->setSegmentIndexManager(std::weak_ptr(segmentIndexManager_));
	}

	DataManager::~DataManager() { closeFileHandles(); }

	void DataManager::closeFileHandles() {
		// readFd_ is now owned by StorageIO — nothing to close here.
	}

	ssize_t DataManager::preadBytes(void *buf, size_t count, int64_t offset) const {
		if (!storageIO_ || !storageIO_->hasPreadSupport())
			return -1;
		return static_cast<ssize_t>(storageIO_->readAt(static_cast<uint64_t>(offset), buf, count));
	}

	ssize_t DataManager::preadSegments(void *buf, size_t segmentCount, uint64_t startSegmentOffset,
									   SegmentReadCachePolicy cachePolicy) const {
		if (buf == nullptr) {
			return -1;
		}
		if (segmentCount == 0) {
			return 0;
		}

		const size_t totalBytes = segmentCount * TOTAL_SEGMENT_SIZE;
		auto *out = static_cast<uint8_t *>(buf);
		const bool readThrough = cachePolicy == SegmentReadCachePolicy::SRCP_READ_THROUGH;
		if (readThrough && pagePool_ && pagePool_->capacity() > 0) {
			if (pagePool_->copyContiguousPages(startSegmentOffset, segmentCount, out, TOTAL_SEGMENT_SIZE)) {
				return static_cast<ssize_t>(totalBytes);
			}
		}

		const ssize_t read = preadBytes(buf, totalBytes, static_cast<int64_t>(startSegmentOffset));
		if (readThrough && read >= static_cast<ssize_t>(totalBytes) && pagePool_ && pagePool_->capacity() > 0) {
			pagePool_->putContiguousPages(startSegmentOffset, segmentCount, out, TOTAL_SEGMENT_SIZE);
		}
		return read;
	}

	// Helper streambuf that wraps an existing memory buffer for zero-copy deserialization.
	// This lets us pread() into a stack buffer, then deserialize via the existing istream API.
	namespace {
		class membuf : public std::streambuf {
		public:
			membuf(char *base, size_t size) { this->setg(base, base, base + size); }
			[[nodiscard]] size_t consumed() const { return static_cast<size_t>(this->gptr() - this->eback()); }
		};

		size_t remainingBytes(const char *cursor, const char *end) { return static_cast<size_t>(end - cursor); }

		bool readRawBytes(const char *&cursor, const char *end, void *out, size_t size) {
			if (remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false;
			}
			std::memcpy(out, cursor, size);
			cursor += size;
			return true;
		}

		template<typename T>
		bool readPod(const char *&cursor, const char *end, T &out) {
			static_assert(std::is_trivial_v<T>, "readPod can only read trivial values");
			return readRawBytes(cursor, end, &out, sizeof(T));
		}

		template<typename T>
		void readUncheckedPod(const char *&cursor, T &out) {
			static_assert(std::is_trivial_v<T>, "readUncheckedPod can only read trivial values");
			std::memcpy(&out, cursor, sizeof(T));
			cursor += sizeof(T);
		}

		bool readString(const char *&cursor, const char *end, std::string &out) {
			uint32_t size = 0;
			if (!readPod(cursor, end, size) || remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false;
			}
			out.assign(cursor, cursor + size);
			cursor += size;
			return true;
		}

		struct SerializedStringView {
			const char *data = nullptr;
			uint32_t size = 0;
		};

		struct PropertyRecordHeader {
			int64_t propertyId = 0;
			int64_t entityId = 0;
			uint32_t entityType = 0;
			bool active = false;
			uint32_t propertyCount = 0;
		};

		constexpr size_t kMaxCoalescedPropertyReadSegments = 16;

		int64_t readSerializedRelationshipId(const char *buf) {
			int64_t edgeId = 0;
			std::memcpy(&edgeId, buf + offsetof(Edge::Metadata, id), sizeof(edgeId));
			return edgeId;
		}

		int64_t readSerializedRelationshipTypeId(const char *buf) {
			int64_t typeId = 0;
			std::memcpy(&typeId, buf + offsetof(Edge::Metadata, typeId), sizeof(typeId));
			return typeId;
		}

		bool readSerializedRelationshipActive(const char *buf) {
			bool active = false;
			std::memcpy(&active, buf + offsetof(Edge::Metadata, isActive), sizeof(active));
			return active;
		}

		int64_t readSerializedRelationshipPropertyEntityId(const char *buf) {
			int64_t propertyEntityId = 0;
			std::memcpy(&propertyEntityId, buf + offsetof(Edge::Metadata, propertyEntityId), sizeof(propertyEntityId));
			return propertyEntityId;
		}

		PropertyStorageType readSerializedRelationshipPropertyStorageType(const char *buf) {
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + offsetof(Edge::Metadata, propertyStorageType), sizeof(storageType));
			return static_cast<PropertyStorageType>(storageType);
		}

		std::optional<RelationshipTypeSegmentStats> parseRelationshipSegmentTypeStats(uint64_t segmentOffset,
																					  const SegmentHeader &header,
																					  const char *data,
																					  bool includePropertyCandidates) {
			if (header.data_type != Edge::typeId || header.used == 0 || data == nullptr) {
				return std::nullopt;
			}

			constexpr size_t entitySize = Edge::getTotalSize();
			RelationshipTypeSegmentStats stats;
			stats.segmentOffset = segmentOffset;
			stats.startId = header.start_id;
			stats.endId = header.start_id + static_cast<int64_t>(header.used) - 1;
			stats.used = header.used;
			stats.inactiveCount = header.inactive_count;
			stats.hasPropertyCandidates = includePropertyCandidates;
			if (includePropertyCandidates) {
				stats.activePropertyEntityIds.reserve(header.used);
				stats.activeBlobEdgeIds.reserve(
						header.inactive_count < header.used ? header.used - header.inactive_count : 0);
			}
			for (uint32_t slot = 0; slot < header.used; ++slot) {
				const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
				const char *edgeBuf = data + static_cast<size_t>(slot) * entitySize;
				if (readSerializedRelationshipId(edgeBuf) != expectedId || !readSerializedRelationshipActive(edgeBuf)) {
					continue;
				}
				const int64_t typeId = readSerializedRelationshipTypeId(edgeBuf);
				++stats.activeCount;
				++stats.activeCountByType[typeId];

				if (!includePropertyCandidates) {
					continue;
				}
				const int64_t propertyEntityId = readSerializedRelationshipPropertyEntityId(edgeBuf);
				if (propertyEntityId == 0) {
					continue;
				}
				const auto storageType = readSerializedRelationshipPropertyStorageType(edgeBuf);
				if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
					stats.activePropertyEntityIds.push_back(propertyEntityId);
					stats.activePropertyEntityIdsByType[typeId].push_back(propertyEntityId);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
					stats.activeBlobEdgeIds.push_back(expectedId);
					stats.activeBlobEdgeIdsByType[typeId].push_back(expectedId);
				}
			}
			return stats;
		}

		void appendRelationshipPropertyCandidates(RelationshipPropertyCandidateStats &target,
												  std::span<const int64_t> propertyEntityIds,
												  std::span<const int64_t> blobEdgeIds) {
			auto reserveAdditional = [](std::vector<int64_t> &values, size_t additional) {
				const size_t desired = values.size() + additional;
				if (desired <= values.capacity()) {
					return;
				}
				values.reserve(std::max(desired, values.capacity() * 2));
			};
			reserveAdditional(target.propertyEntityIds, propertyEntityIds.size());
			target.propertyEntityIds.insert(target.propertyEntityIds.end(), propertyEntityIds.begin(),
											propertyEntityIds.end());
			reserveAdditional(target.fallbackEdgeIds, blobEdgeIds.size());
			target.fallbackEdgeIds.insert(target.fallbackEdgeIds.end(), blobEdgeIds.begin(), blobEdgeIds.end());
		}

		template<typename EntityType>
		std::vector<DirtyEntityInfo<EntityType>>
		filterDirtyInfosByType(std::vector<DirtyEntityInfo<EntityType>> allInfos,
							   const std::vector<EntityChangeType> &types) {
			if (types.size() == 3) {
				return allInfos;
			}

			std::vector<DirtyEntityInfo<EntityType>> result;
			result.reserve(allInfos.size());
			for (const auto &info: allInfos) {
				const bool typeMatch = std::any_of(types.begin(), types.end(),
												   [&](EntityChangeType type) { return info.changeType == type; });
				if (typeMatch) {
					result.push_back(info);
				}
			}
			return result;
		}

		bool readPropertyRecordHeader(const char *&cursor, const char *end, PropertyRecordHeader &header) {
			constexpr size_t headerBytes = sizeof(header.propertyId) + sizeof(header.entityId) +
										   sizeof(header.entityType) + sizeof(header.active) +
										   sizeof(header.propertyCount);
			if (remainingBytes(cursor, end) < headerBytes) { // ZYX_COV_EXCL_LINE
				return false;
			}
			readUncheckedPod(cursor, header.propertyId);
			readUncheckedPod(cursor, header.entityId);
			readUncheckedPod(cursor, header.entityType);
			readUncheckedPod(cursor, header.active);
			readUncheckedPod(cursor, header.propertyCount);
			return true;
		}

		std::optional<PropertyRecordHeader> readActivePropertyRecordHeader(const char *&cursor, const char *end) {
			PropertyRecordHeader header;
			if (!readPropertyRecordHeader(cursor, end, header)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			if (!header.active || header.propertyId == 0) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			return header;
		}

		bool readStringView(const char *&cursor, const char *end, SerializedStringView &out) {
			uint32_t size = 0;
			if (!readPod(cursor, end, size) || remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false;
			}
			out = {cursor, size};
			cursor += size;
			return true;
		}

		bool stringViewEquals(const SerializedStringView &view, const std::string &value) {
			return view.size == value.size() &&
				   (view.size == 0 || std::memcmp(view.data, value.data(), view.size) == 0); // ZYX_COV_EXCL_LINE
		}

		bool skipBytes(const char *&cursor, const char *end, size_t size) {
			if (remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false;
			}
			cursor += size;
			return true;
		}

		bool skipString(const char *&cursor, const char *end) {
			uint32_t size = 0;
			return readPod(cursor, end, size) && skipBytes(cursor, end, size); // ZYX_COV_EXCL_LINE
		}

		bool skipPropertyValue(const char *&cursor, const char *end) {
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return false;
			}

			switch (type) {
				case PropertyType::NULL_TYPE:
					return true;
				case PropertyType::BOOLEAN:
					return skipBytes(cursor, end, sizeof(bool));
				case PropertyType::INTEGER:
					return skipBytes(cursor, end, sizeof(int64_t));
				case PropertyType::DOUBLE:
					return skipBytes(cursor, end, sizeof(double));
				case PropertyType::STRING:
					return skipString(cursor, end);
				case PropertyType::LIST: {
					uint32_t count = 0;
					if (!readPod(cursor, end, count)) { // ZYX_COV_EXCL_LINE
						return false;
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false;
						}
					}
					return true;
				}
				case PropertyType::MAP: {
					uint32_t count = 0;
					if (!readPod(cursor, end, count)) { // ZYX_COV_EXCL_LINE
						return false;
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipString(cursor, end) || !skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false;
						}
					}
					return true;
				}
				case PropertyType::DATE:
					return skipBytes(cursor, end, sizeof(int32_t));
				case PropertyType::DATETIME:
					return skipBytes(cursor, end, sizeof(int64_t));
				case PropertyType::DURATION:
					return skipBytes(cursor, end, sizeof(int64_t) * 3);
				default: // ZYX_COV_EXCL_LINE
					return false;
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValueFallback(const char *&cursor, const char *end) {
			if (cursor > end) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			try {
				membuf valueBuffer(const_cast<char *>(cursor), static_cast<size_t>(end - cursor));
				std::istream stream(&valueBuffer);
				auto value = utils::Serializer::deserialize<PropertyValue>(stream);
				const size_t consumed = valueBuffer.consumed();
				if (consumed == 0 || static_cast<size_t>(end - cursor) < consumed) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
				cursor += consumed;
				return value;
			} catch (...) {
				return std::nullopt;
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValue(const char *&cursor, const char *end) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}

			switch (type) {
				case PropertyType::NULL_TYPE:
					return PropertyValue();
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::STRING: {
					std::string value;
					if (!readString(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(std::move(value));
				}
				case PropertyType::DATE: {
					TemporalDate value;
					if (!readPod(cursor, end, value.epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::DATETIME: {
					TemporalDateTime value;
					if (!readPod(cursor, end, value.epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return PropertyValue(value);
				}
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					cursor = valueStart;
					return readSerializedPropertyValueFallback(cursor, end);
			}
		}

		std::optional<std::unordered_map<std::string, PropertyValue>>
		readSelectedPropertyValues(const char *buf, const std::unordered_set<std::string> &requestedKeys) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			std::unordered_map<std::string, PropertyValue> values;
			values.reserve(std::min<size_t>(header->propertyCount, requestedKeys.size()));
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				if (requestedKeys.contains(key)) {
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					values.emplace(std::move(key), std::move(*value));
				} else if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
			}
			return values;
		}

		struct PropertyEntityRowRef {
			int64_t id = 0;
			size_t row = 0;
		};

		struct PropertyEntitySegmentWork {
			size_t segmentIndex = 0;
			size_t idBegin = 0;
			size_t idEnd = 0;
		};

		struct CompiledPropertyValue {
			const PropertyValue *value = nullptr;
			PropertyType type = PropertyType::UNKNOWN;
			bool boolValue = false;
			int64_t intValue = 0;
			double doubleValue = 0.0;
			const std::string *stringValue = nullptr;
			TemporalDate dateValue;
			TemporalDateTime dateTimeValue;
			TemporalDuration durationValue;
		};

		CompiledPropertyValue compilePropertyValue(const PropertyValue &value) {
			CompiledPropertyValue compiled;
			compiled.value = &value;
			compiled.type = value.getType();
			const auto &variant = value.getVariant();
			switch (compiled.type) {
				case PropertyType::BOOLEAN:
					compiled.boolValue = std::get<bool>(variant);
					break;
				case PropertyType::INTEGER:
					compiled.intValue = std::get<int64_t>(variant);
					break;
				case PropertyType::DOUBLE:
					compiled.doubleValue = std::get<double>(variant);
					break;
				case PropertyType::STRING:
					compiled.stringValue = &std::get<std::string>(variant);
					break;
				case PropertyType::DATE:
					compiled.dateValue = std::get<TemporalDate>(variant);
					break;
				case PropertyType::DATETIME:
					compiled.dateTimeValue = std::get<TemporalDateTime>(variant);
					break;
				case PropertyType::DURATION:
					compiled.durationValue = std::get<TemporalDuration>(variant);
					break;
				default:
					break;
			}
			return compiled;
		}

		struct PredicateExpectation {
			const std::string *key = nullptr;
			CompiledPropertyValue value;
		};

		struct SinglePredicateExpectation {
			const std::string *key = nullptr;
			CompiledPropertyValue value;
		};

		struct PredicateSpecExpectation {
			const std::string *key = nullptr;
			const PropertyValue *value = nullptr;
			const PropertyValue *upperValue = nullptr;
			PropertyEntityPredicateOp op = PropertyEntityPredicateOp::PEP_EQ;
		};

		struct PredicateSpecGroup {
			const std::string *key = nullptr;
			std::vector<const PredicateSpecExpectation *> predicates;
		};

		int64_t readSerializedPropertyId(const char *buf) {
			int64_t id = 0;
			std::memcpy(&id, buf, sizeof(int64_t));
			return id;
		}

		std::vector<PropertyEntitySegmentWork>
		collectPropertyEntitySegmentWork(std::span<const int64_t> sortedIds,
										 const std::vector<SegmentIndexManager::SegmentIndex> &segmentIndex) {
			std::vector<PropertyEntitySegmentWork> work;
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segmentIndex[segment].startId);
				auto hi = std::upper_bound(lo, sortedIds.end(), segmentIndex[segment].endId);
				if (lo != hi) {
					work.push_back({segment, static_cast<size_t>(lo - sortedIds.begin()),
									static_cast<size_t>(hi - sortedIds.begin())});
				}
			}
			return work;
		}

		std::vector<size_t>
		collectPropertyEntityWorkSegmentIndices(const std::vector<PropertyEntitySegmentWork> &work) {
			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(work.size());
			for (const auto &entry: work) {
				segmentIndices.push_back(entry.segmentIndex);
			}
			return segmentIndices;
		}

		bool readSelectedPropertyColumns(const char *buf,
										 const std::unordered_map<std::string, size_t> &requestedKeyIndices,
										 const std::vector<std::vector<std::optional<PropertyValue>> *> &columnTargets,
										 const std::vector<PropertyEntityRowRef> &refs, size_t refBegin,
										 size_t refEnd) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return false;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return false;
				}

				auto keyIt = requestedKeyIndices.find(key);
				if (keyIt == requestedKeyIndices.end()) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return false;
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return false;
				}

				auto &column = *columnTargets[keyIt->second];
				if (refEnd == refBegin + 1) {
					column[refs[refBegin].row] = std::move(*value);
				} else {
					for (size_t ref = refBegin; ref < refEnd; ++ref) {
						column[refs[ref].row] = *value;
					}
				}
			}
			return true;
		}

		std::optional<size_t> visitSelectedPropertyValue(const char *buf, const std::string &requestedKey,
														 const std::vector<PropertyEntityRowRef> &refs, size_t refBegin,
														 size_t refEnd, const PropertyEntityValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *value);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<PropertyEntityScalarValue>
		readSerializedPropertyScalarValue(const char *&cursor, const char *end,
										  std::optional<PropertyValue> &fallbackStorage) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}

			PropertyEntityScalarValue scalar;
			scalar.type = type;
			switch (type) {
				case PropertyType::NULL_TYPE:
					return scalar;
				case PropertyType::BOOLEAN:
					if (!readPod(cursor, end, scalar.boolValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return scalar;
				case PropertyType::INTEGER:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return scalar;
				case PropertyType::DOUBLE:
					if (!readPod(cursor, end, scalar.doubleValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return scalar;
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					scalar.stringValue = std::string_view(value.data, value.size);
					return scalar;
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					scalar.intValue = epochDays;
					return scalar;
				}
				case PropertyType::DATETIME:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return scalar;
				case PropertyType::DURATION:
					if (!readPod(cursor, end, scalar.durationValue.months) ||
						!readPod(cursor, end, scalar.durationValue.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, scalar.durationValue.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return scalar;
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					cursor = valueStart;
					fallbackStorage = readSerializedPropertyValue(cursor, end);
					if (!fallbackStorage.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					scalar.type = fallbackStorage->getType();
					scalar.fallbackValue = &*fallbackStorage;
					return scalar;
			}
		}

		std::optional<size_t> visitSelectedPropertyScalarValue(const char *buf, const std::string &requestedKey,
															   const std::vector<PropertyEntityRowRef> &refs,
															   size_t refBegin, size_t refEnd,
															   const PropertyEntityScalarValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					continue;
				}

				std::optional<PropertyValue> fallbackStorage;
				auto scalar = readSerializedPropertyScalarValue(cursor, end, fallbackStorage);
				if (!scalar.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *scalar);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<bool> serializedPropertyValueEquals(const char *&cursor, const char *end,
														  const CompiledPropertyValue &expected) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			if (type != expected.type) {
				return false;
			}

			switch (type) {
				case PropertyType::NULL_TYPE: // ZYX_COV_EXCL_LINE
					return true;
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return value == expected.boolValue;
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return value == expected.intValue;
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return value == expected.doubleValue;
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return expected.stringValue != nullptr && stringViewEquals(value, *expected.stringValue);
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return epochDays == expected.dateValue.epochDays;
				}
				case PropertyType::DATETIME: {
					int64_t epochMillis = 0;
					if (!readPod(cursor, end, epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return epochMillis == expected.dateTimeValue.epochMillis;
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return value == expected.durationValue;
				}
				case PropertyType::LIST: // ZYX_COV_EXCL_LINE
				case PropertyType::MAP: { // ZYX_COV_EXCL_LINE
					cursor = valueStart;
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return expected.value != nullptr && value.value() == *expected.value;
				}
				default: // ZYX_COV_EXCL_LINE
					return std::nullopt;
			}
		}

		const PredicateExpectation *findPredicateExpectation(const SerializedStringView &key,
															 const std::vector<PredicateExpectation> &expected) {
			for (const auto &entry: expected) {
				if (stringViewEquals(key, *entry.key)) {
					return &entry;
				}
			}
			return nullptr;
		}

		std::optional<bool> readPropertyEntityPredicateMatch(const char *buf,
															 const std::vector<PredicateExpectation> &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			size_t matchedKeys = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				const auto *expectedEntry = findPredicateExpectation(key, expected);
				if (expectedEntry == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					continue;
				}

				auto matches = serializedPropertyValueEquals(cursor, end, expectedEntry->value);
				if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
				if (!matches.value()) {
					return false;
				}
				++matchedKeys;
			}
			return matchedKeys == expected.size();
		}

		std::optional<bool> readPropertyEntitySinglePredicateMatch(const char *buf,
																   const SinglePredicateExpectation &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				if (!stringViewEquals(key, *expected.key)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					continue;
				}

				return serializedPropertyValueEquals(cursor, end, expected.value);
			}
			return false;
		}

		bool propertyValueSatisfiesPredicate(const PropertyValue &actual, const PredicateSpecExpectation &expected) {
			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ:
					return actual == *expected.value;
				case PropertyEntityPredicateOp::PEP_NE:
					return actual != *expected.value;
				case PropertyEntityPredicateOp::PEP_LT:
					return actual < *expected.value;
				case PropertyEntityPredicateOp::PEP_LE:
					return actual <= *expected.value;
				case PropertyEntityPredicateOp::PEP_GT:
					return actual > *expected.value;
				case PropertyEntityPredicateOp::PEP_GE:
					return actual >= *expected.value;
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED:
					return expected.upperValue != nullptr && actual >= *expected.value &&
						   actual <= *expected.upperValue;
			}
			return false;
		}

		template<typename T>
		bool typedValueSatisfiesPredicate(const T &actual, const PredicateSpecExpectation &expected) {
			const auto *expectedValue = std::get_if<T>(&expected.value->getVariant());
			if (expectedValue == nullptr) {
				return propertyValueSatisfiesPredicate(PropertyValue(actual), expected);
			}

			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ:
					return actual == *expectedValue;
				case PropertyEntityPredicateOp::PEP_NE:
					return actual != *expectedValue;
				case PropertyEntityPredicateOp::PEP_LT:
					return actual < *expectedValue;
				case PropertyEntityPredicateOp::PEP_LE:
					return actual <= *expectedValue;
				case PropertyEntityPredicateOp::PEP_GT:
					return actual > *expectedValue;
				case PropertyEntityPredicateOp::PEP_GE:
					return actual >= *expectedValue;
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: {
					if (expected.upperValue == nullptr) {
						return false;
					}
					const auto *upperValue = std::get_if<T>(&expected.upperValue->getVariant());
					return upperValue != nullptr && actual >= *expectedValue && actual <= *upperValue;
				}
			}
			return false;
		}

		int compareStringView(const SerializedStringView &view, const std::string &value) {
			const size_t commonSize = std::min<size_t>(view.size, value.size());
			const int commonCompare =
					commonSize == 0 ? 0 : std::memcmp(view.data, value.data(), commonSize); // ZYX_COV_EXCL_LINE
			if (commonCompare != 0) {
				return commonCompare;
			}
			if (view.size == value.size()) {
				return 0;
			}
			return view.size < value.size() ? -1 : 1;
		}

		bool stringViewSatisfiesPredicate(const SerializedStringView &actual,
										  const PredicateSpecExpectation &expected) {
			const auto *expectedValue = std::get_if<std::string>(&expected.value->getVariant());
			if (expectedValue == nullptr) {
				return propertyValueSatisfiesPredicate(PropertyValue(std::string(actual.data, actual.size)), expected);
			}
			const int comparison = compareStringView(actual, *expectedValue);
			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ:
					return comparison == 0;
				case PropertyEntityPredicateOp::PEP_NE:
					return comparison != 0;
				case PropertyEntityPredicateOp::PEP_LT:
					return comparison < 0;
				case PropertyEntityPredicateOp::PEP_LE:
					return comparison <= 0;
				case PropertyEntityPredicateOp::PEP_GT:
					return comparison > 0;
				case PropertyEntityPredicateOp::PEP_GE:
					return comparison >= 0;
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: {
					if (expected.upperValue == nullptr) {
						return false;
					}
					const auto *upperValue = std::get_if<std::string>(&expected.upperValue->getVariant());
					return upperValue != nullptr && comparison >= 0 && compareStringView(actual, *upperValue) <= 0;
				}
			}
			return false;
		}

		std::vector<PredicateSpecGroup>
		groupPredicateSpecExpectations(const std::vector<PredicateSpecExpectation> &expected) {
			std::vector<PredicateSpecGroup> groups;
			groups.reserve(expected.size());
			for (const auto &entry: expected) {
				auto groupIt = std::find_if(groups.begin(), groups.end(),
											[&](const PredicateSpecGroup &group) { return *group.key == *entry.key; });
				if (groupIt == groups.end()) {
					PredicateSpecGroup group;
					group.key = entry.key;
					group.predicates.push_back(&entry);
					groups.push_back(std::move(group));
				} else {
					groupIt->predicates.push_back(&entry);
				}
			}
			return groups;
		}

		const PredicateSpecGroup *findPredicateSpecGroup(const SerializedStringView &key,
														 const std::vector<PredicateSpecGroup> &groups) {
			for (const auto &group: groups) {
				if (stringViewEquals(key, *group.key)) {
					return &group;
				}
			}
			return nullptr;
		}

		std::optional<bool> readSerializedPropertyValueSatisfiesPredicate(const char *&cursor, const char *end,
																		  const PredicateSpecExpectation &expected) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}

			if (type != expected.value->getType()) {
				cursor = valueStart;
				auto actual = readSerializedPropertyValue(cursor, end);
				if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
				return propertyValueSatisfiesPredicate(*actual, expected);
			}

			switch (type) {
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return stringViewSatisfiesPredicate(value, expected);
				}
				default:
					cursor = valueStart;
					auto actual = readSerializedPropertyValue(cursor, end);
					if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					return propertyValueSatisfiesPredicate(*actual, expected);
			}
		}

		std::optional<bool> readPropertyEntityPredicateMatch(const char *buf,
															 const std::vector<PredicateSpecGroup> &expectedGroups,
															 size_t expectedPredicateCount) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt;
			}

			size_t matchedPredicates = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}

				const auto *group = findPredicateSpecGroup(key, expectedGroups);
				if (group == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					continue;
				}

				for (const auto *entry: group->predicates) {
					const char *valueCursor = cursor;
					auto matches = readSerializedPropertyValueSatisfiesPredicate(valueCursor, end, *entry);
					if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt;
					}
					if (!matches.value()) {
						return false;
					}
					++matchedPredicates;
				}
				if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt;
				}
			}
			return matchedPredicates == expectedPredicateCount;
		}


		template<typename Matcher>
		size_t countPropertyEntityMatches(const DataManager &dm, const std::vector<int64_t> &ids,
										  concurrent::ThreadPool *pool, Matcher &&matchesPredicate) {
			if (ids.empty() || !dm.hasPreadSupport()) {
				return 0;
			}

			bool useInputOrder = true;
			bool hasPreviousId = false;
			int64_t previousId = 0;
			for (const int64_t id: ids) {
				if (id == 0) {
					useInputOrder = false;
					continue;
				}
				if (hasPreviousId && id <= previousId) {
					useInputOrder = false;
				}
				previousId = id;
				hasPreviousId = true;
			}
			if (!hasPreviousId) {
				return 0;
			}

			std::span<const int64_t> sortedIds;
			std::vector<int64_t> sortedStorage;
			std::vector<size_t> multiplicities;
			if (useInputOrder) {
				sortedIds = std::span<const int64_t>(ids.data(), ids.size());
			} else {
				sortedStorage.reserve(ids.size());
				for (const int64_t id: ids) {
					if (id != 0) {
						sortedStorage.push_back(id);
					}
				}
				std::sort(sortedStorage.begin(), sortedStorage.end());
				multiplicities.reserve(sortedStorage.size());
				size_t write = 0;
				for (size_t read = 0; read < sortedStorage.size();) {
					const int64_t id = sortedStorage[read];
					size_t next = read + 1;
					while (next < sortedStorage.size() && sortedStorage[next] == id) {
						++next;
					}
					sortedStorage[write++] = id;
					multiplicities.push_back(next - read);
					read = next;
				}
				sortedStorage.resize(write);
				sortedIds = std::span<const int64_t>(sortedStorage.data(), sortedStorage.size());
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			const auto segmentTracker = dm.getSegmentTracker();
			if (!segmentIndexManager || !segmentTracker) { // ZYX_COV_EXCL_LINE
				return 0;
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
			if (work.empty()) {
				return 0;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, size_t &targetCount) {
				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					const int64_t id = sortedIds[i];
					const auto slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					const auto matches = matchesPredicate(entityBuffer);
					if (matches.has_value() && matches.value()) {
						targetCount += multiplicities.empty() ? size_t{1} : multiplicities[i];
					}
				}
			};

			const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			size_t count = 0;
			if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
				std::vector<size_t> perWorkCounts(work.size(), 0);
				pool->parallelFor(0, groups.size(), [&](size_t gi) {
					const auto &group = groups[gi];
					const size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
					std::vector<char> groupBuf(totalBytes);
					const ssize_t n = dm.preadSegments(groupBuf.data(), group.segCount, group.startOffset);
					if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
						return;
					}

					for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
						const size_t wi = group.memberIndices[mi];
						const auto &w = work[wi];
						const size_t bufferOffset = mi * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, groupBuf.data() + bufferOffset, sizeof(SegmentHeader));
						if (header.used == 0) {
							continue;
						}
						scanPropertyWork(w, header, groupBuf.data() + bufferOffset + sizeof(SegmentHeader),
										 perWorkCounts[wi]);
					}
				});

				for (const size_t perWorkCount: perWorkCounts) {
					count += perWorkCount;
				}
				return count;
			}

			std::vector<char> readBuffer;
			for (const auto &group: groups) {
				if (group.segCount == 1) {
					const size_t wi = group.memberIndices.front();
					const auto &w = work[wi];
					const auto &seg = segIndex[w.segmentIndex];
					SegmentHeader header = segmentTracker->getSegmentHeaderCopy(seg.segmentOffset);
					if (header.used == 0) {
						continue;
					}
					const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
					readBuffer.resize(dataBytes);
					const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
					const ssize_t n = dm.preadBytes(readBuffer.data(), dataBytes, dataOffset);
					if (n < static_cast<ssize_t>(dataBytes)) {
						continue;
					}
					scanPropertyWork(w, header, readBuffer.data(), count);
					continue;
				}

				for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
					 chunkBegin += kMaxCoalescedPropertyReadSegments) {
					const size_t chunkSegments =
							std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
					const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
					readBuffer.resize(totalBytes);
					const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
					const ssize_t n = dm.preadSegments(readBuffer.data(), chunkSegments, groupOffset);
					if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
						continue;
					}

					for (size_t mi = 0; mi < chunkSegments; ++mi) {
						const size_t wi = group.memberIndices[chunkBegin + mi];
						const auto &w = work[wi];
						const size_t bufferOffset = mi * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
						if (header.used == 0) {
							continue;
						}
						scanPropertyWork(w, header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), count);
					}
				}
			}

			return count;
		}

	} // namespace

	void DataManager::initialize(bool skipSegmentIndexBuild) {
		// Initialize low-level components
		deletionManager_ = std::make_shared<DeletionManager>(shared_from_this(), segmentTracker_, allocators_);
		entityReferenceUpdater_ = std::make_shared<EntityReferenceUpdater>(shared_from_this());
		relationshipTraversal_ = std::make_shared<traversal::RelationshipTraversal>(shared_from_this());

		// Initialize segment indexes (unless pre-built by StorageBootstrap)
		if (!skipSegmentIndexBuild) {
			initializeSegmentIndexes();
		}

		// Initialize entity managers
		initializeManagers();
	}

	void DataManager::setSystemStateManager(const std::shared_ptr<state::SystemStateManager> &systemStateManager) {
		systemStateManager_ = systemStateManager;

		// Now that we have the high-level state manager, we can initialize the token registry
		initializeTokenRegistry(systemStateManager);
	}

	void DataManager::initializeTokenRegistry(std::shared_ptr<state::SystemStateManager> sm) {
		// Pass the correct high-level SystemStateManager to the registry
		tokenRegistry_ = std::make_unique<TokenRegistry>(shared_from_this(), sm);
	}

	void DataManager::initializeSegmentIndexes() const {
		// Initialize all segment indexes at once
		segmentIndexManager_->initialize(fileHeader_.node_segment_head, fileHeader_.edge_segment_head,
										 fileHeader_.property_segment_head, fileHeader_.blob_segment_head,
										 fileHeader_.index_segment_head, fileHeader_.state_segment_head);
	}

	void DataManager::initializeManagers() {
		// Initialize chain managers
		blobChainManager_ = std::make_shared<BlobChainManager>(shared_from_this());
		stateChainManager_ = std::make_shared<StateChainManager>(shared_from_this());

		// Create property manager first as others depend on it
		propertyManager_ = std::make_shared<PropertyManager>(this, deletionManager_);

		// Create entity managers
		nodeManager_ = std::make_shared<NodeManager>(this, deletionManager_);
		edgeManager_ = std::make_shared<EdgeManager>(this, deletionManager_);
		blobManager_ = std::make_shared<BlobManager>(this, blobChainManager_, deletionManager_);
		indexEntityManager_ = std::make_shared<IndexEntityManager>(this, deletionManager_);
		stateManager_ = std::make_shared<StateManager>(this, stateChainManager_, deletionManager_);
	}

	// Observer registration is now inline in DataManager.hpp, delegating to observerManager_.
	// All notification methods are now on EntityObserverManager.

	int64_t DataManager::getOrCreateTokenId(const std::string &name) const {
		if (name.empty())
			return 0;
		if (!tokenRegistry_) {
			throw std::runtime_error("TokenRegistry not initialized. Ensure SystemStateManager is set.");
		}
		return tokenRegistry_->getOrCreateTokenId(name);
	}

	int64_t DataManager::resolveTokenId(const std::string &name) const {
		if (name.empty())
			return 0;
		if (!tokenRegistry_) {
			throw std::runtime_error("TokenRegistry not initialized. Ensure SystemStateManager is set.");
		}
		return tokenRegistry_->resolveTokenId(name);
	}

	std::string DataManager::resolveTokenName(int64_t tokenId) const {
		if (tokenId == 0)
			return "";
		if (!tokenRegistry_) {
			return "";
		}

		return tokenRegistry_->resolveTokenName(tokenId);
	}

	// --- Transaction State Management ---

	// setActiveTransaction / clearActiveTransaction are now inline in DataManager.hpp,
	// delegating to txnContext_.

	// --- Node Operations (delegate to NodeManager) ---

	void DataManager::addNode(Node &node) const {
		guardReadOnly();
		for (const auto &v: observerManager_.getValidators()) {
			v->validateNodeInsert(node, node.getProperties());
		}
		nodeManager_->add(node);
		txnContext_.recordAdd(node);
		observerManager_.notifyNodeAdded(node);
	}

	void DataManager::addNodes(std::vector<Node> &nodes) const {
		guardReadOnly();
		if (nodes.empty())
			return;

		// Phase 1: Validate all nodes before any writes (atomicity)
		for (const auto &node: nodes) {
			for (const auto &v: observerManager_.getValidators()) {
				v->validateNodeInsert(node, node.getProperties());
			}
		}


		// Phase 2: Write
		nodeManager_->addBatch(nodes);
		observerManager_.notifyNodesAdded(nodes);

		txnContext_.recordAdds(nodes);

		for (auto &node: nodes) {
			if (node.getProperties().empty())
				continue;

			propertyManager_->storeProperties(node);
			nodeManager_->update(node);
		}
	}

	void DataManager::updateNode(const Node &node) {
		guardReadOnly();
		Node oldNode = nodeManager_->get(node.getId());
		txnContext_.recordUpdate<Node>(node, oldNode);
		nodeManager_->update(node);
		observerManager_.notifyNodeUpdated(oldNode, node);
	}

	void DataManager::deleteNode(Node &node) const {
		guardReadOnly();
		txnContext_.recordDelete<Node>(node.getId(), [this](int64_t id) { return nodeManager_->get(id); });
		nodeManager_->remove(node);
		observerManager_.notifyNodeDeleted(node);
	}

	Node DataManager::getNode(int64_t id) const { return nodeManager_->get(id); }

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) const {
		return nodeManager_->getBatch(ids);
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return nodeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		addEntityPropertiesImpl<Node>(
				nodeId, properties, *nodeManager_,
				[this](const Node &node, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateNodeUpdate(node, oldProps, newProps);
				},
				[this](const Node &o, const Node &n) { observerManager_.notifyNodeUpdated(o, n); });
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) const {
		removeEntityPropertyImpl<Node>(
				nodeId, key, *nodeManager_,
				[this](const Node &node, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateNodeUpdate(node, oldProps, newProps);
				},
				[this](const Node &o, const Node &n) { observerManager_.notifyNodeUpdated(o, n); });
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) const {
		return nodeManager_->getProperties(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodePropertiesDirect(const Node &node) {
		if (node.getId() == 0 || !node.isActive())
			return {};

		// Start with inline properties (no I/O needed)
		auto allProperties = EntityPropertyTraits<Node>::getProperties(node);

		// Load external properties via direct disk read (bypasses cache)
		if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
			auto storageType = EntityPropertyTraits<Node>::getPropertyStorageType(node);
			auto propertyEntityId = EntityPropertyTraits<Node>::getPropertyEntityId(node);

			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				Property property = loadEntityDirect<Property>(propertyEntityId);
				if (property.getId() != 0) {
					for (const auto &[key, value]: property.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				// Blob chain reads are more complex; fall back to regular path
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value]: blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	std::unordered_map<std::string, PropertyValue>
	DataManager::getNodePropertiesFromMap(const Node &node, const std::unordered_map<int64_t, Property> &propertyMap) {
		if (node.getId() == 0 || !node.isActive())
			return {};

		auto allProperties = EntityPropertyTraits<Node>::getProperties(node);

		if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
			auto storageType = EntityPropertyTraits<Node>::getPropertyStorageType(node);
			auto propertyEntityId = EntityPropertyTraits<Node>::getPropertyEntityId(node);

			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				auto it = propertyMap.find(propertyEntityId);
				if (it != propertyMap.end() && it->second.getId() != 0) {
					for (const auto &[key, value]: it->second.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value]: blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	std::unordered_map<int64_t, Property> DataManager::bulkLoadPropertyEntities(const std::vector<int64_t> &ids,
																				concurrent::ThreadPool *pool) const {
		std::unordered_map<int64_t, Property> result;
		if (ids.empty() || !hasPreadSupport())
			return result;

		// Sort IDs and group by segment for sequential I/O
		std::vector<int64_t> sortedIds(ids);
		if (!std::is_sorted(sortedIds.begin(), sortedIds.end())) {
			std::sort(sortedIds.begin(), sortedIds.end());
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		// Find relevant segments and their ID ranges
		struct SegWork {
			size_t segIdx;
			size_t idBegin, idEnd; // indices into sortedIds
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty())
			return result;

		// Parallel path: coalesce consecutive segments into single large I/O calls
		if (pool && !pool->isSingleThreaded() && work.size() > 1) {
			// Build segment index list from work items
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work)
				workSegIndices.push_back(w.segIdx);

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<std::pair<int64_t, Property>>> perSeg(work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				// Single pread for the entire coalesced group
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes))
					return;

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0)
						continue;

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perSeg[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used)
							continue;
						Property prop = Property::deserializeFromBuffer(dataBuf + slot * entitySize);
						if (prop.isActive())
							local.emplace_back(id, std::move(prop));
					}
				}
			});

			// Merge thread-local results
			size_t totalCount = 0;
			for (const auto &v: perSeg)
				totalCount += v.size();
			result.reserve(totalCount);
			for (auto &v: perSeg) {
				for (auto &[id, prop]: v)
					result[id] = std::move(prop);
			}
		} else {
			// Sequential path
			result.reserve(ids.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0)
					continue;

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes))
					continue;

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used)
						continue;
					Property prop = Property::deserializeFromBuffer(buf.data() + slot * entitySize);
					if (prop.isActive())
						result[id] = std::move(prop);
				}
			}
		}

		return result;
	}

	std::unordered_map<int64_t, std::unordered_map<std::string, PropertyValue>>
	DataManager::bulkLoadPropertyEntityValues(const std::vector<int64_t> &ids, const std::vector<std::string> &keys,
											  concurrent::ThreadPool *pool) const {
		std::unordered_map<int64_t, std::unordered_map<std::string, PropertyValue>> result;
		if (ids.empty() || keys.empty() || !hasPreadSupport()) {
			return result;
		}

		std::vector<int64_t> sortedIds(ids);
		std::sort(sortedIds.begin(), sortedIds.end());
		sortedIds.erase(std::unique(sortedIds.begin(), sortedIds.end()), sortedIds.end());

		std::unordered_set<std::string> requestedKeys;
		requestedKeys.reserve(keys.size());
		for (const auto &key: keys) {
			requestedKeys.insert(key);
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return result;
		}

		auto readPropertyValues =
				[&](const char *entityBuffer) -> std::optional<std::unordered_map<std::string, PropertyValue>> {
			return readSelectedPropertyValues(entityBuffer, requestedKeys);
		};

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<std::pair<int64_t, std::unordered_map<std::string, PropertyValue>>>> perWork(
					work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) {
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perWork[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
							continue;
						}
						auto values = readPropertyValues(dataBuf + slot * entitySize);
						if (values.has_value()) {
							local.emplace_back(id, std::move(*values));
						}
					}
				}
			});

			size_t totalCount = 0;
			for (const auto &values: perWork) {
				totalCount += values.size();
			}
			result.reserve(totalCount);
			for (auto &values: perWork) {
				for (auto &[id, propertyValues]: values) {
					result.emplace(id, std::move(propertyValues));
				}
			}
		} else {
			result.reserve(sortedIds.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					auto values = readPropertyValues(buf.data() + slot * entitySize);
					if (values.has_value()) {
						result.emplace(id, std::move(*values));
					}
				}
			}
		}

		return result;
	}

	std::vector<size_t> DataManager::bulkLoadPropertyEntityColumns(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::vector<std::string> &keys,
			std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
			concurrent::ThreadPool *pool) const {
		std::vector<size_t> loadedRows;
		if (ids.empty() || rows.size() != ids.size() || keys.empty() || rowCount == 0 || !hasPreadSupport()) {
			return loadedRows;
		}

		std::unordered_map<std::string, size_t> requestedKeyIndices;
		std::vector<std::vector<std::optional<PropertyValue>> *> columnTargets;
		requestedKeyIndices.reserve(keys.size());
		columnTargets.reserve(keys.size());
		for (const auto &key: keys) {
			if (requestedKeyIndices.contains(key)) {
				continue;
			}
			auto columnIt = columns.find(key);
			if (columnIt == columns.end() || columnIt->second.size() < rowCount) {
				continue;
			}
			const size_t index = columnTargets.size();
			requestedKeyIndices.emplace(key, index);
			columnTargets.push_back(&columnIt->second);
		}
		if (columnTargets.empty()) {
			return loadedRows;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return loadedRows;
		}

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return loadedRows;
		}

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &localLoadedRows = perWorkLoadedRows[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) { // ZYX_COV_EXCL_LINE
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
						if (readSerializedPropertyId(entityBuffer) != id) {
							continue;
						}
						const auto [refBegin, refEnd] = refRanges[i];
						if (readSelectedPropertyColumns(entityBuffer, requestedKeyIndices, columnTargets, refs,
														refBegin, refEnd)) {
							for (size_t ref = refBegin; ref < refEnd; ++ref) {
								localLoadedRows.push_back(refs[ref].row);
							}
						}
					}
				}
			});

			size_t loadedCount = 0;
			for (const auto &rowsForWork: perWorkLoadedRows) {
				loadedCount += rowsForWork.size();
			}
			loadedRows.reserve(loadedCount);
			for (auto &rowsForWork: perWorkLoadedRows) {
				loadedRows.insert(loadedRows.end(), rowsForWork.begin(), rowsForWork.end());
			}
		} else {
			loadedRows.reserve(refs.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					const auto [refBegin, refEnd] = refRanges[i];
					if (readSelectedPropertyColumns(entityBuffer, requestedKeyIndices, columnTargets, refs, refBegin,
													refEnd)) {
						for (size_t ref = refBegin; ref < refEnd; ++ref) {
							loadedRows.push_back(refs[ref].row);
						}
					}
				}
			}
		}

		return loadedRows;
	}

	size_t DataManager::bulkVisitPropertyEntityValues(const std::vector<int64_t> &ids, const std::vector<size_t> &rows,
													  size_t rowCount, const std::string &key,
													  const PropertyEntityValueVisitor &visitor,
													  concurrent::ThreadPool *pool) const {
		(void) pool;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || key.empty() || !visitor ||
			!hasPreadSupport()) {
			return 0;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return 0;
		}

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();
		const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
		if (work.empty()) {
			return 0;
		}

		size_t visited = 0;
		auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
									const char *dataBuf) {
			for (size_t i = w.idBegin; i < w.idEnd; ++i) {
				const int64_t id = sortedIds[i];
				const auto slot = static_cast<uint32_t>(id - header.start_id);
				if (slot >= header.used) {
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) {
					continue;
				}
				const auto [refBegin, refEnd] = refRanges[i];
				auto visitCount = visitSelectedPropertyValue(entityBuffer, key, refs, refBegin, refEnd, visitor);
				if (visitCount.has_value()) {
					visited += *visitCount;
				}
			}
		};

		const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
		auto groups = buildCoalescedGroups(workSegIndices, segIndex);
		std::vector<char> readBuffer;
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}
				scanPropertyWork(w, header, readBuffer.data());
				continue;
			}

			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kMaxCoalescedPropertyReadSegments) {
				const size_t chunkSegments =
						std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = preadSegments(readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue;
				}

				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t wi = group.memberIndices[chunkBegin + member];
					const auto &w = work[wi];
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}
					const char *dataBuf = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(w, header, dataBuf);
				}
			}
		}
		return visited;
	}

	size_t DataManager::bulkVisitPropertyEntityScalarValues(const std::vector<int64_t> &ids,
															const std::vector<size_t> &rows, size_t rowCount,
															const std::string &key,
															const PropertyEntityScalarValueVisitor &visitor,
															concurrent::ThreadPool *pool) const {
		(void) pool;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || key.empty() || !visitor ||
			!hasPreadSupport()) {
			return 0;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return 0;
		}

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();
		const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
		if (work.empty()) {
			return 0;
		}

		size_t visited = 0;
		auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
									const char *dataBuf) {
			for (size_t i = w.idBegin; i < w.idEnd; ++i) {
				const int64_t id = sortedIds[i];
				const auto slot = static_cast<uint32_t>(id - header.start_id);
				if (slot >= header.used) {
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) {
					continue;
				}
				const auto [refBegin, refEnd] = refRanges[i];
				auto visitCount = visitSelectedPropertyScalarValue(entityBuffer, key, refs, refBegin, refEnd, visitor);
				if (visitCount.has_value()) {
					visited += *visitCount;
				}
			}
		};

		const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
		auto groups = buildCoalescedGroups(workSegIndices, segIndex);
		std::vector<char> readBuffer;
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}
				scanPropertyWork(w, header, readBuffer.data());
				continue;
			}

			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kMaxCoalescedPropertyReadSegments) {
				const size_t chunkSegments =
						std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = preadSegments(readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue;
				}

				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t wi = group.memberIndices[chunkBegin + member];
					const auto &w = work[wi];
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}
					const char *dataBuf = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(w, header, dataBuf);
				}
			}
		}
		return visited;
	}

	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicates(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::unordered_map<std::string, PropertyValue> &expected, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		PropertyEntityPredicateMatchResult result;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || expected.empty() || !hasPreadSupport()) {
			return result;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return result;
		}

		std::vector<PredicateExpectation> predicateExpectations;
		predicateExpectations.reserve(expected.size());
		for (const auto &[key, value]: expected) {
			predicateExpectations.push_back({&key, compilePropertyValue(value)});
		}
		const bool useSinglePredicate = predicateExpectations.size() == 1;
		const SinglePredicateExpectation singlePredicate =
				useSinglePredicate ? SinglePredicateExpectation{predicateExpectations.front().key,
																predicateExpectations.front().value}
								   : SinglePredicateExpectation{};

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return result;
		}

		auto appendPredicateResult = [&](std::vector<size_t> &loadedRows, std::vector<size_t> *matchedRows,
										 size_t &loadedCount, size_t &matchedCount, size_t idIndex,
										 std::optional<bool> matches) {
			if (!matches.has_value()) {
				return;
			}
			const auto [refBegin, refEnd] = refRanges[idIndex];
			for (size_t ref = refBegin; ref < refEnd; ++ref) {
				++loadedCount;
				if (options.collectLoadedRows) {
					loadedRows.push_back(refs[ref].row);
				}
				if (matches.value()) {
					++matchedCount;
					if (matchedRows != nullptr) {
						matchedRows->push_back(refs[ref].row);
					}
				}
			}
		};

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());
			std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
			std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
			std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
						if (readSerializedPropertyId(entityBuffer) != id) {
							continue;
						}
						appendPredicateResult(
								perWorkLoadedRows[wi], options.collectMatchedRows ? &perWorkMatchedRows[wi] : nullptr,
								perWorkLoadedCounts[wi], perWorkMatchedCounts[wi], i,
								useSinglePredicate
										? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
										: readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations));
					}
				}
			});

			size_t loadedCount = 0;
			for (size_t i = 0; i < work.size(); ++i) {
				loadedCount += perWorkLoadedCounts[i];
				result.matchedCount += perWorkMatchedCounts[i];
			}
			result.loadedCount = loadedCount;
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(loadedCount);
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(result.matchedCount);
			}
			for (size_t i = 0; i < work.size(); ++i) {
				if (options.collectLoadedRows) {
					result.loadedRows.insert(result.loadedRows.end(), perWorkLoadedRows[i].begin(),
											 perWorkLoadedRows[i].end());
				}
				if (options.collectMatchedRows) {
					result.matchedRows.insert(result.matchedRows.end(), perWorkMatchedRows[i].begin(),
											  perWorkMatchedRows[i].end());
				}
			}
		} else {
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(refs.size());
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(refs.size());
			}
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					appendPredicateResult(
							result.loadedRows, options.collectMatchedRows ? &result.matchedRows : nullptr,
							result.loadedCount, result.matchedCount, i,
							useSinglePredicate ? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
											   : readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations));
				}
			}
		}

		return result;
	}

	size_t
	DataManager::bulkCountPropertyEntityPredicates(const std::vector<int64_t> &ids,
												   const std::unordered_map<std::string, PropertyValue> &expected,
												   concurrent::ThreadPool *pool) const {
		if (expected.empty()) {
			return 0;
		}

		std::vector<PredicateExpectation> predicateExpectations;
		predicateExpectations.reserve(expected.size());
		for (const auto &[key, value]: expected) {
			predicateExpectations.push_back({&key, compilePropertyValue(value)});
		}
		const bool useSinglePredicate = predicateExpectations.size() == 1;
		const SinglePredicateExpectation singlePredicate =
				useSinglePredicate ? SinglePredicateExpectation{predicateExpectations.front().key,
																predicateExpectations.front().value}
								   : SinglePredicateExpectation{};
		return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
			return useSinglePredicate ? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
									  : readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations);
		});
	}

	size_t DataManager::bulkCountPropertyEntityPredicateSpecs(const std::vector<int64_t> &ids,
															  const std::vector<PropertyEntityPredicate> &predicates,
															  concurrent::ThreadPool *pool) const {
		if (predicates.empty()) {
			return 0;
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);
		return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
			return readPropertyEntityPredicateMatch(entityBuffer, predicateGroups, predicateExpectations.size());
		});
	}


	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicateSpecs(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::vector<PropertyEntityPredicate> &predicates, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		PropertyEntityPredicateMatchResult result;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || predicates.empty() || !hasPreadSupport()) {
			return result;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return result;
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return result;
		}

		auto appendPredicateResult = [&](std::vector<size_t> &loadedRows, std::vector<size_t> *matchedRows,
										 size_t &loadedCount, size_t &matchedCount, size_t idIndex,
										 std::optional<bool> matches) {
			if (!matches.has_value()) {
				return;
			}
			const auto [refBegin, refEnd] = refRanges[idIndex];
			for (size_t ref = refBegin; ref < refEnd; ++ref) {
				++loadedCount;
				if (options.collectLoadedRows) {
					loadedRows.push_back(refs[ref].row);
				}
				if (matches.value()) {
					++matchedCount;
					if (matchedRows != nullptr) {
						matchedRows->push_back(refs[ref].row);
					}
				}
			}
		};

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());
			std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
			std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
			std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
						if (readSerializedPropertyId(entityBuffer) != id) {
							continue;
						}
						appendPredicateResult(perWorkLoadedRows[wi],
											  options.collectMatchedRows ? &perWorkMatchedRows[wi] : nullptr,
											  perWorkLoadedCounts[wi], perWorkMatchedCounts[wi], i,
											  readPropertyEntityPredicateMatch(entityBuffer, predicateGroups,
																			   predicateExpectations.size()));
					}
				}
			});

			size_t loadedCount = 0;
			for (size_t i = 0; i < work.size(); ++i) {
				loadedCount += perWorkLoadedCounts[i];
				result.matchedCount += perWorkMatchedCounts[i];
			}
			result.loadedCount = loadedCount;
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(loadedCount);
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(result.matchedCount);
			}
			for (size_t i = 0; i < work.size(); ++i) {
				if (options.collectLoadedRows) {
					result.loadedRows.insert(result.loadedRows.end(), perWorkLoadedRows[i].begin(),
											 perWorkLoadedRows[i].end());
				}
				if (options.collectMatchedRows) {
					result.matchedRows.insert(result.matchedRows.end(), perWorkMatchedRows[i].begin(),
											  perWorkMatchedRows[i].end());
				}
			}
		} else {
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(refs.size());
			}
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					appendPredicateResult(result.loadedRows, options.collectMatchedRows ? &result.matchedRows : nullptr,
										  result.loadedCount, result.matchedCount, i,
										  readPropertyEntityPredicateMatch(entityBuffer, predicateGroups,
																		   predicateExpectations.size()));
				}
			}
		}

		return result;
	}

	// --- Edge Operations (delegate to EdgeManager) ---

	void DataManager::addEdge(Edge &edge) const {
		guardReadOnly();
		for (const auto &v: observerManager_.getValidators()) {
			v->validateEdgeInsert(edge, edge.getProperties());
		}
		edgeManager_->add(edge);
		txnContext_.recordAdd(edge);
		observerManager_.notifyEdgeAdded(edge);
	}

	void DataManager::addEdges(std::vector<Edge> &edges) const {
		guardReadOnly();
		if (edges.empty())
			return;

		// Phase 1: Validate all edges before any writes (atomicity)
		for (const auto &edge: edges) {
			for (const auto &v: observerManager_.getValidators()) {
				v->validateEdgeInsert(edge, edge.getProperties());
			}
		}


		// 1. Assign IDs and initial persistence
		edgeManager_->addBatch(edges);

		// 2. Indexing (needs IDs + Props)
		observerManager_.notifyEdgesAdded(edges);

		txnContext_.recordAdds(edges);

		// 3. Property Storage Handling
		for (auto &edge: edges) {
			if (edge.getProperties().empty())
				continue;

			// Links external properties using the ID assigned in Step 1
			propertyManager_->storeProperties(edge);

			if (EntityPropertyTraits<Edge>::hasPropertyEntity(edge)) { // ZYX_COV_EXCL_LINE
				// Persist the modification (link to external prop)
				edgeManager_->update(edge);
			}
		}
	}

	void DataManager::updateEdge(const Edge &edge) {
		guardReadOnly();
		Edge oldEdge = edgeManager_->get(edge.getId());
		txnContext_.recordUpdate<Edge>(edge, oldEdge);
		edgeManager_->update(edge);
		observerManager_.notifyEdgeUpdated(oldEdge, edge);
	}

	void DataManager::deleteEdge(Edge &edge) const {
		guardReadOnly();
		txnContext_.recordDelete<Edge>(edge.getId(), [this](int64_t id) { return edgeManager_->get(id); });
		edgeManager_->remove(edge);
		observerManager_.notifyEdgeDeleted(edge);
	}

	Edge DataManager::getEdge(int64_t id) const { return edgeManager_->get(id); }

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) const {
		return edgeManager_->getBatch(ids);
	}

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return edgeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addEdgeProperties(int64_t edgeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		addEntityPropertiesImpl<Edge>(
				edgeId, properties, *edgeManager_,
				[this](const Edge &edge, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateEdgeUpdate(edge, oldProps, newProps);
				},
				[this](const Edge &o, const Edge &n) { observerManager_.notifyEdgeUpdated(o, n); });
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) const {
		removeEntityPropertyImpl<Edge>(
				edgeId, key, *edgeManager_,
				[this](const Edge &edge, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateEdgeUpdate(edge, oldProps, newProps);
				},
				[this](const Edge &o, const Edge &n) { observerManager_.notifyEdgeUpdated(o, n); });
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) const {
		return edgeManager_->getProperties(edgeId);
	}

	std::vector<Edge> DataManager::findEdgesByNode(int64_t nodeId, const std::string &direction) const {
		if (direction == "out") {
			return relationshipTraversal_->getOutgoingEdges(nodeId);
		} else if (direction == "in") {
			return relationshipTraversal_->getIncomingEdges(nodeId);
		} else { // "both" is the default
			return relationshipTraversal_->getAllConnectedEdges(nodeId);
		}
	}

	// --- Property Operation Templates ---

	template<typename EntityType, typename ManagerType>
	void DataManager::addEntityPropertiesImpl(
			int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties, ManagerType &manager,
			std::function<void(const EntityType &, const std::unordered_map<std::string, PropertyValue> &,
							   const std::unordered_map<std::string, PropertyValue> &)>
					validate,
			std::function<void(const EntityType &, const EntityType &)> notify) const {
		guardReadOnly();
		EntityType oldEntity = manager.get(entityId);
		auto existingProps = manager.getProperties(entityId);
		oldEntity.setProperties(existingProps);

		auto mergedProps = existingProps;
		for (const auto &[key, val]: properties) {
			mergedProps[key] = val;
		}
		validate(oldEntity, existingProps, mergedProps);

		observerManager_.setSuppressNotifications(true);
		manager.addProperties(entityId, properties);
		observerManager_.setSuppressNotifications(false);

		EntityType newEntity = manager.get(entityId);
		auto newProps = manager.getProperties(entityId);
		newEntity.setProperties(newProps);
		notify(oldEntity, newEntity);
	}

	template<typename EntityType, typename ManagerType>
	void DataManager::removeEntityPropertyImpl(
			int64_t entityId, const std::string &key, ManagerType &manager,
			std::function<void(const EntityType &, const std::unordered_map<std::string, PropertyValue> &,
							   const std::unordered_map<std::string, PropertyValue> &)>
					validate,
			std::function<void(const EntityType &, const EntityType &)> notify) const {
		guardReadOnly();
		EntityType oldEntity = manager.get(entityId);
		auto existingProps = manager.getProperties(entityId);
		oldEntity.setProperties(existingProps);

		auto removedProps = existingProps;
		removedProps.erase(key);
		validate(oldEntity, existingProps, removedProps);

		observerManager_.setSuppressNotifications(true);
		manager.removeProperty(entityId, key);
		observerManager_.setSuppressNotifications(false);

		EntityType newEntity = manager.get(entityId);
		auto newProps = manager.getProperties(entityId);
		newEntity.setProperties(newProps);
		notify(oldEntity, newEntity);
	}

	// --- Property Entity Operations ---

	void DataManager::addPropertyEntity(Property &property) const { propertyManager_->add(property); }

	void DataManager::updatePropertyEntity(const Property &property) const { propertyManager_->update(property); }

	void DataManager::deleteProperty(Property &property) const { propertyManager_->remove(property); }

	Property DataManager::getProperty(int64_t id) const { return propertyManager_->get(id); }

	// --- Blob Operations ---

	void DataManager::addBlobEntity(Blob &blob) const { blobManager_->add(blob); }

	void DataManager::updateBlobEntity(const Blob &blob) const { blobManager_->update(blob); }

	void DataManager::deleteBlob(Blob &blob) const { blobManager_->remove(blob); }

	Blob DataManager::getBlob(int64_t id) const { return blobManager_->get(id); }

	// --- Index Operations ---

	void DataManager::addIndexEntity(Index &index) const { indexEntityManager_->add(index); }

	void DataManager::updateIndexEntity(const Index &index) const { indexEntityManager_->update(index); }

	void DataManager::deleteIndex(Index &index) const { indexEntityManager_->remove(index); }

	Index DataManager::getIndex(int64_t id) const { return indexEntityManager_->get(id); }

	// --- State Operations ---

	void DataManager::addStateEntity(State &state) const { stateManager_->add(state); }

	void DataManager::updateStateEntity(const State &state) const { stateManager_->update(state); }

	void DataManager::deleteState(State &state) const { stateManager_->remove(state); }

	State DataManager::getState(int64_t id) const { return stateManager_->get(id); }

	State DataManager::findStateByKey(const std::string &key) const { return stateManager_->findByKey(key); }

	void DataManager::addStateProperties(const std::string &stateKey,
										 const std::unordered_map<std::string, PropertyValue> &properties,
										 bool useBlobStorage) const {
		// 1. Capture OLD state (Snapshot)
		const State oldState = stateManager_->findByKey(stateKey);

		// 2. Perform the update (This effectively replaces the chain)
		// Pass the flag to StateManager
		stateManager_->addStateProperties(stateKey, properties, useBlobStorage);

		// 3. Capture NEW state
		const State newState = stateManager_->findByKey(stateKey);

		// 4. Notify Listeners
		observerManager_.notifyStateUpdated(oldState, newState);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getStateProperties(const std::string &stateKey) const {
		return stateManager_->getStateProperties(stateKey);
	}

	void DataManager::removeState(const std::string &stateKey) const { stateManager_->removeState(stateKey); }

	// --- Template Method Implementations ---

	template<typename EntityType>
	void DataManager::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
	}

	template<typename EntityType>
	EntityType DataManager::getEntity(int64_t id) {
		return EntityTraits<EntityType>::get(this, id);
	}

	template<typename EntityType>
	void DataManager::updateEntity(const EntityType &entity) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			updateNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			updateEdge(entity);
		}
		// Note: Property, Blob, Index, State updates use specialized methods directly
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::getEntitiesInRange(int64_t startId, int64_t endId, size_t limit) {
		std::vector<EntityType> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Reserve memory to avoid reallocations.
		result.reserve((std::min) (static_cast<size_t>(endId - startId + 1), limit));

		// This set will keep track of IDs that have already been processed (from memory)
		// to avoid adding them again from the disk-based load.
		std::unordered_set<int64_t> processedIds;

		// --- PASS 1: Populate from Memory (PersistenceManager dirty entries) ---
		// This pass ensures data consistency by prioritizing in-memory changes over stale disk data.

		for (int64_t currentId = startId; currentId <= endId; ++currentId) {
			if (result.size() >= limit) {
				break;
			}

			// Check PersistenceManager first (Highest Priority)
			auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(currentId);

			if (dirtyInfo.has_value()) {
				processedIds.insert(currentId);

				if (dirtyInfo->changeType != EntityChangeType::CHANGE_DELETED &&
					dirtyInfo->backup.has_value()) { // ZYX_COV_EXCL_LINE: dirty registry always stores backups for
													 // non-delete range reads
					result.push_back(*dirtyInfo->backup);
				}
			}
		}

		// If we have already reached the limit just from memory, we can return early.
		if (result.size() >= limit) {
			return result;
		}

		// --- PASS 2: Load remaining entities from Disk using Segment-based reads ---
		// This pass leverages data locality for performance by reading from segments in bulk.
		// It will skip any entities that were already loaded from memory in Pass 1.

		// Find all segments that overlap with the requested ID range.
		auto entityType = EntityType::typeId;
		auto segments = segmentTracker_->getSegmentsByType(entityType);

		for (const auto &header: segments) {
			// Calculate the intersection between the segment's ID range and the user's requested range.
			int64_t segmentStartId = header.start_id;
			int64_t segmentEndId = header.start_id + header.used - 1;

			int64_t intersectStart = (std::max) (startId, segmentStartId);
			int64_t intersectEnd = (std::min) (endId, segmentEndId);

			if (intersectStart > intersectEnd) {
				continue; // No overlap with this segment.
			}

			// Load a batch of entities from this segment within the intersecting range.
			size_t needed = limit - result.size();
			std::vector<EntityType> segmentEntities =
					loadEntitiesFromSegment<EntityType>(header.file_offset, intersectStart, intersectEnd, needed);

			for (const EntityType &entity: segmentEntities) {
				// IMPORTANT: Only add the entity if it was not already processed from memory.
				if (!processedIds.contains(entity.getId())) { // ZYX_COV_EXCL_LINE: segment loader range is already
															  // filtered against dirty entries in tests
					result.push_back(entity);

					// Add the newly loaded entity to the cache for future queries.
					addToCache(entity);

					if (result.size() >= limit) {
						break;
					}
				}
			}

			if (result.size() >= limit) {
				break; // Stop iterating through segments if limit is reached.
			}
		}

		return result;
	}

	template<typename EntityType>
	uint64_t DataManager::findSegmentForEntityId(int64_t id) const {
		uint32_t type = EntityType::typeId;
		return segmentIndexManager_->findSegmentForId(type, id);
	}

	// --- Entity Loading from Disk ---

	template<typename EntityType>
	std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		return readEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::readEntityFromDisk(const int64_t fileOffset) const {
		EntityType entity;

		if (hasPreadSupport()) { // ZYX_COV_EXCL_LINE: non-pread fallback is platform defensive and not hit on POSIX CI
			// Thread-safe path: pread() is atomic and needs no synchronization
			constexpr size_t entitySize = EntityType::getTotalSize();
			char buf[entitySize];
			ssize_t n = preadBytes(buf, entitySize, fileOffset);
			if (n < static_cast<ssize_t>(
							entitySize)) // ZYX_COV_EXCL_LINE: short pread requires corrupt/truncated segment file
				return std::nullopt;
			membuf mb(buf, entitySize);
			std::istream stream(&mb);
			entity = EntityType::deserialize(stream);
		} else {
			// Legacy path: requires external synchronization
			file_->seekg(fileOffset);
			entity = EntityType::deserialize(*file_);
		}

		if (!entity.isActive()) { // ZYX_COV_EXCL_LINE: inactive entities are filtered by segment metadata before direct
								  // reads
			return std::nullopt;
		}

		return entity;
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::findAndReadEntity(int64_t id) const {
		// Find segment for this entity type and ID
		uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);

		if (segmentOffset == 0) {
			return std::nullopt; // Segment not found
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate position of entity within segment
		uint64_t relativePosition = id - header.start_id;
		if (relativePosition >=
			header.used) { // ZYX_COV_EXCL_LINE: segment index lookup returns matching ranges for public reads
			return std::nullopt; // ID is out of range for this segment
		}

		// Calculate file offset for this entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														relativePosition * EntityType::getTotalSize());

		if (hasPreadSupport()) {
			// pread path: skip bitmap check — readEntityFromDisk checks isActive() on the
			// deserialized entity itself. This avoids a shared_lock on SegmentTracker per read.
			return readEntityFromDisk<EntityType>(entityOffset);
		}

		// fstream path: use bitmap to avoid unnecessary disk seek
		if (!segmentTracker_->isEntityActive(segmentOffset, relativePosition)) {
			return std::nullopt;
		}

		return readEntityFromDisk<EntityType>(entityOffset);
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		std::vector<EntityType> result;
		if (segmentOffset == 0 || limit == 0) {
			return result;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate effective start and end positions
		uint64_t effectiveStartId = (std::max) (startId, header.start_id);
		uint64_t effectiveEndId = (std::min) (endId, header.start_id + header.used - 1);

		if (effectiveStartId > effectiveEndId) {
			return result;
		}

		// Calculate offsets
		uint64_t startOffset = effectiveStartId - header.start_id;
		uint64_t count = (std::min) (effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

		// Reserve space for the maximum possible entities
		result.reserve(count);

		// Calculate starting file offset
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														startOffset * EntityType::getTotalSize());

		// Read entities (only active entities are returned)
		for (uint64_t i = 0; i < count; ++i) {
			auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
			if (entityOpt.has_value()) {
				result.push_back(entityOpt.value());
			}

			// Move to next entity
			entityOffset += EntityType::getTotalSize();
		}

		return result;
	}

	// --- Entity Memory Operations ---

	template<typename EntityType>
	void DataManager::setEntityDirty(const DirtyEntityInfo<EntityType> &info) {
		if (info.backup.has_value() && info.backup->getId() == 0) {
			return;
		}
		persistenceManager_->upsert(info);
	}

	template<typename EntityType>
	std::optional<DirtyEntityInfo<EntityType>> DataManager::getDirtyInfo(int64_t id) {
		return persistenceManager_->getDirtyInfo<EntityType>(id);
	}

	bool DataManager::hasUnsavedChanges() const { return persistenceManager_->hasUnsavedChanges(); }

	FlushSnapshot DataManager::prepareFlushSnapshot() const { return persistenceManager_->createSnapshot(); }

	FlushSnapshotView DataManager::prepareFlushSnapshotView() const {
		return persistenceManager_->createSnapshotView();
	}

	void DataManager::commitFlushSnapshot() const { persistenceManager_->commitSnapshot(); }

	void DataManager::setMaxDirtyEntities(size_t max) const { persistenceManager_->setMaxDirtyEntities(max); }

	void DataManager::setAutoFlushCallback(std::function<void()> cb) const {
		persistenceManager_->setAutoFlushCallback(std::move(cb));
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (txnContext_.isActive())
			return; // Suppress auto-flush during active transaction
		persistenceManager_->checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) {
		return filterDirtyInfosByType(persistenceManager_->getAllDirtyInfos<EntityType>(), types);
	}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) const {
		return filterDirtyInfosByType(persistenceManager_->getAllDirtyInfos<EntityType>(), types);
	}

	template<typename T>
	T make_inactive() {
		T entity;
		entity.markInactive();
		return entity;
	}

	// Helper: deserialize an entity from a cached page's raw bytes.
	// Returns default entity (id==0) if the entity is not in range or inactive.
	template<typename EntityType>
	EntityType deserializeEntityFromPage(const Page &page, int64_t id) {
		const auto *header = reinterpret_cast<const SegmentHeader *>(page.data.data());

		int64_t relativePosition = id - header->start_id;

		constexpr size_t entitySize = EntityType::getTotalSize();
		size_t entityOffset = SEGMENT_HEADER_SIZE + static_cast<size_t>(relativePosition) * entitySize;

		membuf mb(const_cast<char *>(reinterpret_cast<const char *>(page.data.data() + entityOffset)), entitySize);
		std::istream stream(&mb);
		return EntityType::deserialize(stream);
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// Check if we're in a read-only transaction with a snapshot
		const auto *snapshot = currentSnapshot_;
		if (snapshot != nullptr) {
			return getEntityWithSnapshot<EntityType>(id, snapshot);
		}

		// 1. Check dirty info via PersistenceManager (write transaction or no-txn context)
		{
			auto dirtyInfo = getDirtyInfo<EntityType>(id);

			if (dirtyInfo.has_value()) {
				if (dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED) {
					return make_inactive<EntityType>();
				}
				if (dirtyInfo->backup.has_value()) {
					return *dirtyInfo->backup;
				}
			}
		}

		// 2. Try PageBufferPool (segment-level cache)
		{
			uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);
			if (segmentOffset != 0) {
				// Check pool first
				const Page *page = pagePool_->getPage(segmentOffset);
				if (page != nullptr) {
					EntityType entity = deserializeEntityFromPage<EntityType>(*page, id);
					if (entity.getId() != 0 && entity.isActive()) {
						return entity;
					}
					return make_inactive<EntityType>();
				}

				// Page pool miss — read full segment from disk, populate pool
				if (hasPreadSupport()) {
					std::vector<uint8_t> segData(TOTAL_SEGMENT_SIZE);
					ssize_t n = preadBytes(segData.data(), TOTAL_SEGMENT_SIZE, static_cast<int64_t>(segmentOffset));
					if (n >= static_cast<ssize_t>(TOTAL_SEGMENT_SIZE)) {
						pagePool_->putPage(segmentOffset, std::vector<uint8_t>(segData));
						EntityType entity =
								deserializeEntityFromPage<EntityType>(Page{segmentOffset, std::move(segData)}, id);
						if (entity.getId() != 0 && entity.isActive()) {
							return entity;
						}
						return make_inactive<EntityType>();
					}
				}
			}
		}

		// 3. Fallback: load single entity from disk (handles segment index miss gracefully)
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			return entity;
		}

		return make_inactive<EntityType>();
	}

	// Helper: get the snapshot map for a given entity type
	namespace {
		template<typename EntityType>
		const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &
		getSnapshotMap(const CommittedSnapshot &snapshot);

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Node>> &
		getSnapshotMap<Node>(const CommittedSnapshot &snapshot) {
			return snapshot.nodes;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &
		getSnapshotMap<Edge>(const CommittedSnapshot &snapshot) {
			return snapshot.edges;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Property>> &
		getSnapshotMap<Property>(const CommittedSnapshot &snapshot) {
			return snapshot.properties;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &
		getSnapshotMap<Blob>(const CommittedSnapshot &snapshot) {
			return snapshot.blobs;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Index>> &
		getSnapshotMap<Index>(const CommittedSnapshot &snapshot) {
			return snapshot.indexes;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<State>> &
		getSnapshotMap<State>(const CommittedSnapshot &snapshot) {
			return snapshot.states;
		}
	} // namespace

	template<typename EntityType>
	EntityType DataManager::getEntityWithSnapshot(int64_t id, const CommittedSnapshot *snapshot) {
		// 1. Check snapshot dirty state
		const auto &snapshotMap = getSnapshotMap<EntityType>(*snapshot);
		auto it = snapshotMap.find(id);
		if (it != snapshotMap.end()) {
			const auto &info = it->second;
			if (info.changeType == EntityChangeType::CHANGE_DELETED) {
				return make_inactive<EntityType>();
			}
			if (info.backup.has_value()) {
				return *info.backup;
			}
		}

		// 2. Try PageBufferPool (segment-level cache)
		{
			uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);
			if (segmentOffset != 0) {
				const Page *page = pagePool_->getPage(segmentOffset);
				if (page != nullptr) {
					EntityType entity = deserializeEntityFromPage<EntityType>(*page, id);
					if (entity.getId() != 0 && entity.isActive()) {
						return entity;
					}
					return make_inactive<EntityType>();
				}

				if (hasPreadSupport()) {
					std::vector<uint8_t> segData(TOTAL_SEGMENT_SIZE);
					ssize_t n = preadBytes(segData.data(), TOTAL_SEGMENT_SIZE, static_cast<int64_t>(segmentOffset));
					if (n >= static_cast<ssize_t>(TOTAL_SEGMENT_SIZE)) {
						pagePool_->putPage(segmentOffset, std::vector<uint8_t>(segData));
						EntityType entity =
								deserializeEntityFromPage<EntityType>(Page{segmentOffset, std::move(segData)}, id);
						if (entity.getId() != 0 && entity.isActive()) {
							return entity;
						}
						return make_inactive<EntityType>();
					}
				}
			}
		}

		// 3. Fallback: load single entity from disk
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			return entity;
		}
		return make_inactive<EntityType>();
	}

	// Explicit instantiations for getEntityWithSnapshot
	template Node DataManager::getEntityWithSnapshot<Node>(int64_t, const CommittedSnapshot *);
	template Edge DataManager::getEntityWithSnapshot<Edge>(int64_t, const CommittedSnapshot *);
	template Property DataManager::getEntityWithSnapshot<Property>(int64_t, const CommittedSnapshot *);
	template Blob DataManager::getEntityWithSnapshot<Blob>(int64_t, const CommittedSnapshot *);
	template Index DataManager::getEntityWithSnapshot<Index>(int64_t, const CommittedSnapshot *);
	template State DataManager::getEntityWithSnapshot<State>(int64_t, const CommittedSnapshot *);

	template<typename EntityType>
	EntityType DataManager::loadEntityDirect(int64_t id) {
		// 1. Check dirty info (uncommitted changes must be visible)
		auto dirtyInfo = getDirtyInfo<EntityType>(id);
		if (dirtyInfo.has_value()) {
			if (dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED)
				return make_inactive<EntityType>();
			if (dirtyInfo->backup.has_value())
				return *dirtyInfo->backup;
		}

		// 2. Read directly from disk via pread (no cache, no locks)
		return EntityTraits<EntityType>::loadFromDisk(this, id);
	}

	// Explicit instantiations for loadEntityDirect
	template Node DataManager::loadEntityDirect<Node>(int64_t);
	template Edge DataManager::loadEntityDirect<Edge>(int64_t);
	template Property DataManager::loadEntityDirect<Property>(int64_t);
	template Blob DataManager::loadEntityDirect<Blob>(int64_t);
	template Index DataManager::loadEntityDirect<Index>(int64_t);
	template State DataManager::loadEntityDirect<State>(int64_t);

	template<typename EntityType>
	std::vector<EntityType> DataManager::bulkLoadEntities(int64_t filterStartId, int64_t filterEndId) const {
		if (!hasPreadSupport())
			return {}; // Requires pread support

		// Get sorted segment list for this entity type
		const auto &segIndex = EntityTraits<EntityType>::getSegmentIndex(this);
		constexpr size_t entitySize = EntityType::getTotalSize();

		std::vector<EntityType> result;
		result.reserve(segIndex.size() * 4); // Rough estimate

		for (const auto &seg: segIndex) {
			// Skip segments entirely outside the ID range
			if (seg.endId < filterStartId || seg.startId > filterEndId)
				continue;

			// Get segment header (cached in memory, no lock needed)
			SegmentHeader header = segmentTracker_->getSegmentHeader(seg.segmentOffset);
			if (header.used == 0)
				continue;

			// Read entire segment data area in one pread syscall
			size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
			std::vector<char> buf(dataBytes);
			auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
			ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
			if (n < static_cast<ssize_t>(dataBytes))
				continue;

			// Deserialize all entities from the buffer
			for (uint32_t i = 0; i < header.used; ++i) {
				int64_t entityId = header.start_id + i;
				if (entityId < filterStartId || entityId > filterEndId)
					continue;

				membuf mb(buf.data() + i * entitySize, entitySize);
				std::istream stream(&mb);
				EntityType entity = EntityType::deserialize(stream);

				if (entity.isActive()) {
					result.push_back(std::move(entity));
				}
			}
		}

		return result;
	}

	// Explicit instantiations for bulkLoadEntities
	template std::vector<Node> DataManager::bulkLoadEntities<Node>(int64_t, int64_t) const;
	template std::vector<Edge> DataManager::bulkLoadEntities<Edge>(int64_t, int64_t) const;
	template std::vector<Property> DataManager::bulkLoadEntities<Property>(int64_t, int64_t) const;

	// --- Loading Entities from Disk ---

	Node DataManager::loadNodeFromDisk(const int64_t id) const {
		return findAndReadEntity<Node>(id).value_or(make_inactive<Node>());
	}
	Edge DataManager::loadEdgeFromDisk(const int64_t id) const {
		return findAndReadEntity<Edge>(id).value_or(make_inactive<Edge>());
	}
	Property DataManager::loadPropertyFromDisk(const int64_t id) const {
		return findAndReadEntity<Property>(id).value_or(make_inactive<Property>());
	}
	Blob DataManager::loadBlobFromDisk(const int64_t id) const {
		return findAndReadEntity<Blob>(id).value_or(make_inactive<Blob>());
	}
	Index DataManager::loadIndexFromDisk(const int64_t id) const {
		return findAndReadEntity<Index>(id).value_or(make_inactive<Index>());
	}
	State DataManager::loadStateFromDisk(const int64_t id) const {
		return findAndReadEntity<State>(id).value_or(make_inactive<State>());
	}

	// --- Rollback Template Helpers ---

	template<typename EntityType, typename ManagerType>
	void DataManager::rollbackAddedEntry(const wal::UndoEntry &entry, ManagerType &manager,
										 std::function<void(const EntityType &)> notifyDeleted) const {
		try {
			EntityType entity = manager.get(entry.entityId);
			notifyDeleted(entity);
		} catch (...) {
		}
	}

	template<typename EntityType, typename ManagerType>
	void DataManager::rollbackModifiedEntry(
			const wal::UndoEntry &entry, ManagerType &manager,
			std::function<void(const EntityType &, const EntityType &)> notifyUpdated) const {
		if (entry.beforeImage.empty())
			return;
		try {
			std::string imgStr(entry.beforeImage.begin(), entry.beforeImage.end());
			std::istringstream iss(imgStr, std::ios::binary);
			EntityType oldEntity =
					utils::FixedSizeSerializer::deserializeWithFixedSize<EntityType>(iss, EntityType::getTotalSize());
			EntityType currentEntity = manager.get(entry.entityId);
			notifyUpdated(currentEntity, oldEntity);
		} catch (...) {
		}
	}

	template<typename EntityType>
	void DataManager::rollbackDeletedEntry(const wal::UndoEntry &entry,
										   std::function<void(const EntityType &)> notifyAdded) const {
		if (entry.beforeImage.empty())
			return;
		try {
			std::string imgStr(entry.beforeImage.begin(), entry.beforeImage.end());
			std::istringstream iss(imgStr, std::ios::binary);
			EntityType oldEntity =
					utils::FixedSizeSerializer::deserializeWithFixedSize<EntityType>(iss, EntityType::getTotalSize());
			notifyAdded(oldEntity);
		} catch (...) {
		}
	}

	// --- Transaction Rollback ---

	void DataManager::rollbackActiveTransaction() {
		persistenceManager_->setTransactionActive(true);

		const auto &entries = txnContext_.undoLog().entries();
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			auto entityType = static_cast<EntityType>(it->entityType);
			bool isNode = (entityType == EntityType::Node);
			bool isEdge = (entityType == EntityType::Edge);

			switch (it->changeType) {
				case wal::UndoChangeType::UNDO_ADDED:
					if (isNode) {
						rollbackAddedEntry<Node>(*it, *nodeManager_,
												 [this](const Node &n) { observerManager_.notifyNodeDeleted(n); });
					} else if (isEdge) {
						rollbackAddedEntry<Edge>(*it, *edgeManager_,
												 [this](const Edge &e) { observerManager_.notifyEdgeDeleted(e); });
					}
					break;
				case wal::UndoChangeType::UNDO_MODIFIED:
					if (isNode) {
						rollbackModifiedEntry<Node>(*it, *nodeManager_, [this](const Node &c, const Node &o) {
							observerManager_.notifyNodeUpdated(c, o);
						});
					} else if (isEdge) {
						rollbackModifiedEntry<Edge>(*it, *edgeManager_, [this](const Edge &c, const Edge &o) {
							observerManager_.notifyEdgeUpdated(c, o);
						});
					}
					break;
				case wal::UndoChangeType::UNDO_DELETED:
					if (isNode) {
						rollbackDeletedEntry<Node>(*it, [this](const Node &n) { observerManager_.notifyNodeAdded(n); });
					} else if (isEdge) {
						rollbackDeletedEntry<Edge>(*it, [this](const Edge &e) { observerManager_.notifyEdgeAdded(e); });
					}
					break;
			}
		}

		persistenceManager_->clearAll();
		persistenceManager_->setTransactionActive(false);
		clearCache();
	}

	// --- Cache and Transaction Management ---

	void DataManager::clearCache() const {
		pagePool_->clear();
		clearRelationshipSegmentTypeStats();
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::buildRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
												   bool includePropertyCandidates) const {
		if (!hasPreadSupport() || header.data_type != Edge::typeId || header.used == 0) {
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
		std::vector<char> buf(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader));
		const ssize_t read = preadBytes(buf.data(), dataBytes, dataOffset);
		if (read < static_cast<ssize_t>(dataBytes)) {
			return std::nullopt;
		}

		return parseRelationshipSegmentTypeStats(segmentOffset, header, buf.data(), includePropertyCandidates);
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::getRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
												 bool includePropertyCandidates) const {
		if (auto cached = getCachedRelationshipSegmentTypeStats(segmentOffset, header, includePropertyCandidates)) {
			return cached;
		}

		auto stats = buildRelationshipSegmentTypeStats(segmentOffset, header, includePropertyCandidates);
		if (!stats.has_value()) {
			return std::nullopt;
		}

		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		auto &cached = relationshipSegmentTypeStats_[segmentOffset];
		cached = std::move(*stats);
		return cached;
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::getCachedRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
													   bool requirePropertyCandidates) const {
		const int64_t expectedEndId =
				header.used == 0 ? header.start_id - 1 : header.start_id + static_cast<int64_t>(header.used) - 1;
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		auto it = relationshipSegmentTypeStats_.find(segmentOffset);
		if (it != relationshipSegmentTypeStats_.end() && it->second.startId == header.start_id &&
			it->second.endId == expectedEndId && it->second.used == header.used &&
			it->second.inactiveCount == header.inactive_count &&
			(!requirePropertyCandidates || it->second.hasPropertyCandidates)) {
			return it->second;
		}
		return std::nullopt;
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::cachedRelationshipTypeSegmentStats(uint64_t segmentOffset) const {
		if (segmentOffset == 0) {
			return std::nullopt;
		}
		SegmentHeader header{};
		try {
			header = segmentTracker_->getSegmentHeaderCopy(segmentOffset);
		} catch (const std::exception &) {
			return std::nullopt;
		}
		if (header.data_type != Edge::typeId) {
			return std::nullopt;
		}
		return getCachedRelationshipSegmentTypeStats(segmentOffset, header);
	}

	std::optional<RelationshipPropertyCandidateStats>
	DataManager::collectRelationshipPropertyCandidatesFromSegmentStats(int64_t beginId, int64_t endId,
																	   int64_t typeId) const {
		if (!hasPreadSupport() || beginId <= 0 || endId < beginId || hasUnsavedChanges()) {
			return std::nullopt;
		}
		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr &&
			(!snapshot->edges.empty() || !snapshot->properties.empty() || !snapshot->blobs.empty())) {
			return std::nullopt;
		}

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		RelationshipPropertyCandidateStats candidates;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId || entry.startId > endId) {
				continue;
			}

			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			const int64_t segmentFirst = std::max<int64_t>(entry.startId, header.start_id);
			const int64_t segmentLast =
					std::min<int64_t>(entry.endId, header.start_id + static_cast<int64_t>(header.used) - 1);
			const int64_t first = std::max<int64_t>(beginId, segmentFirst);
			const int64_t last = std::min<int64_t>(endId, segmentLast);
			if (first > last) {
				continue;
			}
			if (first != segmentFirst || last != segmentLast) {
				return std::nullopt;
			}

			auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header, true);
			if (!stats.has_value()) {
				return std::nullopt;
			}
			if (typeId == 0) {
				candidates.matchedEdges += static_cast<size_t>(stats->activeCount);
				appendRelationshipPropertyCandidates(candidates, stats->activePropertyEntityIds,
													 stats->activeBlobEdgeIds);
				continue;
			}

			if (auto countIt = stats->activeCountByType.find(typeId); countIt != stats->activeCountByType.end()) {
				candidates.matchedEdges += static_cast<size_t>(countIt->second);
			}
			if (auto propertyIt = stats->activePropertyEntityIdsByType.find(typeId);
				propertyIt != stats->activePropertyEntityIdsByType.end()) {
				appendRelationshipPropertyCandidates(candidates, propertyIt->second, std::span<const int64_t>{});
			}
			if (auto blobIt = stats->activeBlobEdgeIdsByType.find(typeId);
				blobIt != stats->activeBlobEdgeIdsByType.end()) {
				appendRelationshipPropertyCandidates(candidates, std::span<const int64_t>{}, blobIt->second);
			}
		}
		return candidates;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::getRelationshipTypeTotalStats() const {
		if (!hasPreadSupport()) {
			return std::nullopt;
		}

		if (auto cached = getCachedRelationshipTypeTotalStats()) {
			return cached;
		}

		auto stats = buildRelationshipTypeTotalStats();
		if (!stats.has_value()) {
			return std::nullopt;
		}

		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipTypeTotalStats_ = std::move(*stats);
		return relationshipTypeTotalStats_;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::getCachedRelationshipTypeTotalStats() const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		const int64_t firstId = segmentIndex.empty() ? int64_t{0} : segmentIndex.front().startId;
		const int64_t lastId = segmentIndex.empty() ? int64_t{-1} : segmentIndex.back().endId;
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		if (relationshipTypeTotalStats_.has_value() &&
			relationshipTypeTotalStats_->segmentCount == segmentIndex.size() &&
			relationshipTypeTotalStats_->firstId == firstId && relationshipTypeTotalStats_->lastId == lastId) {
			return relationshipTypeTotalStats_;
		}
		return std::nullopt;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::buildRelationshipTypeTotalStats() const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		RelationshipTypeTotalStats total;
		total.segmentCount = segmentIndex.size();
		if (segmentIndex.empty()) {
			return total;
		}
		total.firstId = segmentIndex.front().startId;
		total.lastId = segmentIndex.back().endId;

		for (const auto &entry: segmentIndex) {
			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header);
			if (!stats.has_value()) {
				return std::nullopt;
			}
			total.activeCount += stats->activeCount;
			for (const auto &[typeId, count]: stats->activeCountByType) {
				total.activeCountByType[typeId] += count;
			}
		}
		return total;
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeFromTotalStats(int64_t beginId, int64_t endId,
																			 int64_t typeId) const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		if (!segmentIndex.empty() && (beginId > segmentIndex.front().startId || endId < segmentIndex.back().endId)) {
			return std::nullopt;
		}

		auto totalStats = getRelationshipTypeTotalStats();
		if (!totalStats.has_value()) {
			return std::nullopt;
		}
		if (totalStats->segmentCount > 0 && (beginId > totalStats->firstId || endId < totalStats->lastId)) {
			return std::nullopt;
		}

		int64_t baseCount = totalStats->activeCount;
		if (typeId != 0) {
			baseCount = 0;
			if (auto it = totalStats->activeCountByType.find(typeId); it != totalStats->activeCountByType.end()) {
				baseCount = it->second;
			}
		}

		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr) {
			return applyRelationshipTypeCountSnapshotOverlay(baseCount, beginId, endId, typeId, snapshot->edges);
		}
		auto edgeOverlay = getDirtyEntityInfos<Edge>(
				{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
		return applyRelationshipTypeCountOverlay(baseCount, beginId, endId, typeId, edgeOverlay);
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeInSegmentWindow(uint64_t segmentOffset,
																			  const SegmentHeader &header,
																			  int64_t firstId, int64_t lastId,
																			  int64_t typeId) const {
		if (!hasPreadSupport() || header.data_type != Edge::typeId || header.used == 0 || firstId > lastId) {
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const auto firstSlot = static_cast<uint32_t>(firstId - header.start_id);
		const auto lastSlot = static_cast<uint32_t>(lastId - header.start_id);
		if (lastSlot >= header.used || firstSlot > lastSlot) {
			return std::nullopt;
		}

		const size_t rowCount = static_cast<size_t>(lastSlot - firstSlot + 1);
		const size_t dataBytes = rowCount * entitySize;
		std::vector<char> buf(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) +
													 static_cast<uint64_t>(firstSlot) * entitySize);
		const ssize_t read = preadBytes(buf.data(), dataBytes, dataOffset);
		if (read < static_cast<ssize_t>(dataBytes)) {
			return std::nullopt;
		}

		int64_t count = 0;
		for (uint32_t slot = firstSlot; slot <= lastSlot; ++slot) {
			const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
			const char *edgeBuf = buf.data() + static_cast<size_t>(slot - firstSlot) * entitySize;
			if (readSerializedRelationshipId(edgeBuf) == expectedId && readSerializedRelationshipActive(edgeBuf) &&
				(typeId == 0 || readSerializedRelationshipTypeId(edgeBuf) == typeId)) {
				++count;
			}
		}
		return count;
	}

	std::optional<bool> DataManager::persistedEdgeMatchesType(int64_t edgeId, int64_t typeId) const {
		if (!hasPreadSupport() || edgeId <= 0) {
			return std::nullopt;
		}

		const uint64_t segmentOffset = findSegmentForEntityId<Edge>(edgeId);
		if (segmentOffset == 0) {
			return false;
		}

		SegmentHeader header{};
		const ssize_t headerRead = preadBytes(&header, sizeof(SegmentHeader), static_cast<int64_t>(segmentOffset));
		if (headerRead < static_cast<ssize_t>(sizeof(SegmentHeader)) || header.data_type != Edge::typeId) {
			return std::nullopt;
		}

		const int64_t slot = edgeId - header.start_id;
		if (slot < 0 || slot >= static_cast<int64_t>(header.used)) {
			return false;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		std::vector<char> buf(entitySize);
		const auto dataOffset =
				static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) + static_cast<uint64_t>(slot) * entitySize);
		const ssize_t read = preadBytes(buf.data(), entitySize, dataOffset);
		if (read < static_cast<ssize_t>(entitySize)) {
			return std::nullopt;
		}

		return readSerializedRelationshipId(buf.data()) == edgeId && readSerializedRelationshipActive(buf.data()) &&
			   (typeId == 0 || readSerializedRelationshipTypeId(buf.data()) == typeId);
	}

	std::optional<int64_t>
	DataManager::applyRelationshipTypeCountOverlay(int64_t baseCount, int64_t beginId, int64_t endId, int64_t typeId,
												   std::span<const DirtyEntityInfo<Edge>> edgeOverlay) const {
		int64_t total = baseCount;
		for (const auto &info: edgeOverlay) {
			if (!info.backup.has_value()) {
				continue;
			}
			const Edge &edge = *info.backup;
			const int64_t edgeId = edge.getId();
			if (edgeId < beginId || edgeId > endId) {
				continue;
			}

			if (info.changeType != EntityChangeType::CHANGE_ADDED) {
				auto persistedMatch = persistedEdgeMatchesType(edgeId, typeId);
				if (!persistedMatch.has_value()) {
					return std::nullopt;
				}
				total -= *persistedMatch ? int64_t{1} : int64_t{0};
			}

			if (info.changeType != EntityChangeType::CHANGE_DELETED && edge.isActive() &&
				(typeId == 0 || edge.getTypeId() == typeId)) {
				++total;
			}
		}
		return total;
	}

	std::optional<int64_t> DataManager::applyRelationshipTypeCountSnapshotOverlay(
			int64_t baseCount, int64_t beginId, int64_t endId, int64_t typeId,
			const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &edgeOverlay) const {
		std::vector<DirtyEntityInfo<Edge>> overlay;
		overlay.reserve(edgeOverlay.size());
		for (const auto &[edgeId, info]: edgeOverlay) {
			(void) edgeId;
			overlay.push_back(info);
		}
		return applyRelationshipTypeCountOverlay(baseCount, beginId, endId, typeId, overlay);
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeFromSegmentStats(int64_t beginId, int64_t endId,
																			   int64_t typeId) const {
		if (!hasPreadSupport() || beginId <= 0 || endId < beginId) {
			return std::nullopt;
		}
		if (auto count = countActiveEdgesByTypeFromTotalStats(beginId, endId, typeId)) {
			return count;
		}
		const auto *snapshot = getCurrentSnapshot();

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		int64_t total = 0;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId || entry.startId > endId) {
				continue;
			}

			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			const int64_t segmentFirst = std::max<int64_t>(entry.startId, header.start_id);
			const int64_t segmentLast =
					std::min<int64_t>(entry.endId, header.start_id + static_cast<int64_t>(header.used) - 1);
			const int64_t first = std::max<int64_t>(beginId, segmentFirst);
			const int64_t last = std::min<int64_t>(endId, segmentLast);
			if (first > last) {
				continue;
			}

			if (first == segmentFirst && last == segmentLast) {
				auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header);
				if (!stats.has_value()) {
					return std::nullopt;
				}
				if (typeId == 0) {
					total += stats->activeCount;
				} else if (auto it = stats->activeCountByType.find(typeId); it != stats->activeCountByType.end()) {
					total += it->second;
				}
				continue;
			}

			auto partial = countActiveEdgesByTypeInSegmentWindow(entry.segmentOffset, header, first, last, typeId);
			if (!partial.has_value()) {
				return std::nullopt;
			}
			total += *partial;
		}
		if (snapshot != nullptr) {
			return applyRelationshipTypeCountSnapshotOverlay(total, beginId, endId, typeId, snapshot->edges);
		}
		auto edgeOverlay = getDirtyEntityInfos<Edge>(
				{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
		return applyRelationshipTypeCountOverlay(total, beginId, endId, typeId, edgeOverlay);
	}

	void DataManager::invalidateRelationshipSegmentTypeStats(std::span<const uint64_t> segmentOffsets) const {
		if (segmentOffsets.empty()) {
			return;
		}
		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipTypeTotalStats_.reset();
		for (uint64_t segmentOffset: segmentOffsets) {
			relationshipSegmentTypeStats_.erase(segmentOffset);
		}
	}

	void DataManager::clearRelationshipSegmentTypeStats() const {
		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipSegmentTypeStats_.clear();
		relationshipTypeTotalStats_.reset();
	}

	void DataManager::invalidateDirtySegments(const FlushSnapshot &snapshot) const {
		std::unordered_set<uint64_t> dirtySegments;

		auto collect = [&](const auto &entityMap, auto findSeg) {
			for (const auto &[id, info]: entityMap) {
				uint64_t seg = findSeg(id);
				if (seg != 0) {
					dirtySegments.insert(seg);
				}
			}
		};

		collect(snapshot.nodes, [&](int64_t id) { return findSegmentForEntityId<Node>(id); });
		collect(snapshot.edges, [&](int64_t id) { return findSegmentForEntityId<Edge>(id); });
		collect(snapshot.properties, [&](int64_t id) { return findSegmentForEntityId<Property>(id); });
		collect(snapshot.blobs, [&](int64_t id) { return findSegmentForEntityId<Blob>(id); });
		collect(snapshot.indexes, [&](int64_t id) { return findSegmentForEntityId<Index>(id); });
		collect(snapshot.states, [&](int64_t id) { return findSegmentForEntityId<State>(id); });

		for (uint64_t seg: dirtySegments) {
			pagePool_->invalidate(seg);
		}
		std::vector<uint64_t> dirtySegmentList(dirtySegments.begin(), dirtySegments.end());
		invalidateRelationshipSegmentTypeStats(dirtySegmentList);
	}

	void DataManager::invalidateDirtySegments(const FlushSnapshotView &snapshot) const {
		std::unordered_set<uint64_t> dirtySegments;

		auto collect = [&](const auto *entityMap, auto findSeg) {
			if (!entityMap) {
				return;
			}
			for (const auto &[id, info]: *entityMap) {
				uint64_t seg = findSeg(id);
				if (seg != 0) {
					dirtySegments.insert(seg);
				}
			}
		};

		collect(snapshot.nodes, [&](int64_t id) { return findSegmentForEntityId<Node>(id); });
		collect(snapshot.edges, [&](int64_t id) { return findSegmentForEntityId<Edge>(id); });
		collect(snapshot.properties, [&](int64_t id) { return findSegmentForEntityId<Property>(id); });
		collect(snapshot.blobs, [&](int64_t id) { return findSegmentForEntityId<Blob>(id); });
		collect(snapshot.indexes, [&](int64_t id) { return findSegmentForEntityId<Index>(id); });
		collect(snapshot.states, [&](int64_t id) { return findSegmentForEntityId<State>(id); });

		for (uint64_t seg: dirtySegments) {
			pagePool_->invalidate(seg);
		}
		std::vector<uint64_t> dirtySegmentList(dirtySegments.begin(), dirtySegments.end());
		invalidateRelationshipSegmentTypeStats(dirtySegmentList);
	}

	void DataManager::invalidateSegments(std::span<const uint64_t> segmentOffsets) const {
		for (uint64_t segmentOffset: segmentOffsets) {
			if (segmentOffset != 0) {
				pagePool_->invalidate(segmentOffset);
			}
		}
		invalidateRelationshipSegmentTypeStats(segmentOffsets);
	}

	template<typename EntityType>
	void DataManager::markEntityDeleted(EntityType &entity) {
		auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(entity.getId());

		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) {
			// Revert ADD by overwriting with explicit DELETED (or just removing, but DELETED is safer for log)
			// Actually, if it was just added in memory and never saved, we can technically ignore it,
			// BUT to be safe in the registry logic, we mark it DELETED.
			// Better yet, update registry to remove it?
			// Simpler: Mark DELETED.
			// persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_DELETED, entity));

			// Remove from persistence manager completely (Undoes the ADD).
			// This prevents an unnecessary DELETE record from being written to the WAL/Disk.
			const int64_t id = entity.getId();
			persistenceManager_->remove<EntityType>(id);
		} else {
			entity.markInactive();
			persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_DELETED, entity));
		}

		EntityTraits<EntityType>::removeFromCache(this, entity.getId());
		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		propertyManager_->removeEntityProperty<EntityType>(entityId, key);
	}

	// --- Template Instantiations ---

	// Node-specific instantiations
	template void DataManager::addToCache<Node>(const Node &entity);
	template Node DataManager::getEntity<Node>(int64_t id);
	template void DataManager::updateEntity<Node>(const Node &entity);
	template std::optional<DirtyEntityInfo<Node>> DataManager::getDirtyInfo<Node>(int64_t);
	template std::vector<Node> DataManager::getEntitiesInRange<Node>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Node>(int64_t id) const;
	template Node DataManager::getEntityFromMemoryOrDisk<Node>(int64_t id);
	template void DataManager::removeEntityProperty<Node>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Node>(Node &entity);
	template std::vector<Node> DataManager::loadEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Node> DataManager::readEntityFromDisk<Node>(int64_t fileOffset) const;
	template std::optional<Node> DataManager::findAndReadEntity<Node>(int64_t id) const;
	template std::vector<Node> DataManager::readEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<Node>(const DirtyEntityInfo<Node> &);

	// Edge-specific instantiations
	template void DataManager::addToCache<Edge>(const Edge &entity);
	template Edge DataManager::getEntity<Edge>(int64_t id);
	template void DataManager::updateEntity<Edge>(const Edge &entity);
	template std::optional<DirtyEntityInfo<Edge>> DataManager::getDirtyInfo<Edge>(int64_t);
	template std::vector<Edge> DataManager::getEntitiesInRange<Edge>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Edge>(int64_t id) const;
	template Edge DataManager::getEntityFromMemoryOrDisk<Edge>(int64_t id);
	template void DataManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Edge>(Edge &entity);
	template std::vector<Edge> DataManager::loadEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Edge> DataManager::readEntityFromDisk<Edge>(int64_t fileOffset) const;
	template std::optional<Edge> DataManager::findAndReadEntity<Edge>(int64_t id) const;
	template std::vector<Edge> DataManager::readEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<Edge>(const DirtyEntityInfo<Edge> &);

	// Property-specific instantiations
	template void DataManager::addToCache<Property>(const Property &entity);
	template std::optional<DirtyEntityInfo<Property>> DataManager::getDirtyInfo<Property>(int64_t);
	template Property DataManager::getEntityFromMemoryOrDisk<Property>(int64_t id);
	template std::vector<Property> DataManager::getEntitiesInRange<Property>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Property>(int64_t id) const;
	template void DataManager::markEntityDeleted<Property>(Property &entity);
	template std::vector<DirtyEntityInfo<Property>>
	DataManager::getDirtyEntityInfos<Property>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Property>>
	DataManager::getDirtyEntityInfos<Property>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<Property>(const DirtyEntityInfo<Property> &);

	// Blob-specific instantiations
	template Blob DataManager::getEntityFromMemoryOrDisk<Blob>(int64_t id);
	template std::optional<DirtyEntityInfo<Blob>> DataManager::getDirtyInfo<Blob>(int64_t);
	template std::vector<Blob> DataManager::getEntitiesInRange<Blob>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<Blob>(const DirtyEntityInfo<Blob> &);

	// Index-specific instantiations
	template Index DataManager::getEntityFromMemoryOrDisk<Index>(int64_t id);
	template std::optional<DirtyEntityInfo<Index>> DataManager::getDirtyInfo<Index>(int64_t);
	template std::vector<Index> DataManager::getEntitiesInRange<Index>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Index>(int64_t id) const;
	template void DataManager::markEntityDeleted<Index>(Index &entity);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<Index>(const DirtyEntityInfo<Index> &);

	// State-specific instantiations
	template State DataManager::getEntityFromMemoryOrDisk<State>(int64_t id);
	template std::optional<DirtyEntityInfo<State>> DataManager::getDirtyInfo<State>(int64_t);
	template std::vector<State> DataManager::getEntitiesInRange<State>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<State>(int64_t id) const;
	template void DataManager::markEntityDeleted<State>(State &entity);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &) const;
	template void DataManager::setEntityDirty<State>(const DirtyEntityInfo<State> &);
} // namespace graph::storage
