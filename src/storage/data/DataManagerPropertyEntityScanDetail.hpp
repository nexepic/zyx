#pragma once

#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <sstream>
#include <type_traits>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {

	namespace {
		class membuf : public std::streambuf {
		public:
			membuf(char *base, size_t size) { this->setg(base, base, base + size); }
			[[nodiscard]] size_t consumed() const { return static_cast<size_t>(this->gptr() - this->eback()); }
		};

		size_t remainingBytes(const char *cursor, const char *end) { return static_cast<size_t>(end - cursor); }

		bool readRawBytes(const char *&cursor, const char *end, void *out, size_t size) {
			if (remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
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
				return false; // ZYX_COV_EXCL_LINE
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
		constexpr size_t kMinParallelPropertyReadTasks = 2;
		constexpr size_t kMinParallelPropertyReadSegments = kMaxCoalescedPropertyReadSegments * 2;

		concurrent::ParallelExecutionDecision decidePropertyEntityScan(
				concurrent::ThreadPool *pool,
				size_t taskCount,
				size_t segmentCount) {
			return concurrent::decideParallelExecution(
					pool,
					{.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					 .partitions = taskCount,
					 .estimatedItems = segmentCount,
					 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
					 .minPartitions = kMinParallelPropertyReadTasks,
					 .minItems = kMinParallelPropertyReadSegments});
		}

		std::vector<char> &propertyEntityScanScratchBuffer(size_t requiredBytes) {
			thread_local std::vector<char> buffer;
			buffer.resize(requiredBytes);
			return buffer;
		}

		bool readPropertyRecordHeader(const char *&cursor, const char *end, PropertyRecordHeader &header) {
			constexpr size_t headerBytes = sizeof(header.propertyId) + sizeof(header.entityId) +
										   sizeof(header.entityType) + sizeof(header.active) +
										   sizeof(header.propertyCount);
			if (remainingBytes(cursor, end) < headerBytes) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (!header.active || header.propertyId == 0) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			return header;
		}

		bool readStringView(const char *&cursor, const char *end, SerializedStringView &out) {
			uint32_t size = 0;
			if (!readPod(cursor, end, size) || remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
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
				return false; // ZYX_COV_EXCL_LINE
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
				return false; // ZYX_COV_EXCL_LINE
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
						return false; // ZYX_COV_EXCL_LINE
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false; // ZYX_COV_EXCL_LINE
						}
					}
					return true;
				}
				case PropertyType::MAP: {
					uint32_t count = 0;
					if (!readPod(cursor, end, count)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipString(cursor, end) || !skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false; // ZYX_COV_EXCL_LINE
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
					return false; // ZYX_COV_EXCL_LINE
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValueFallback(const char *&cursor, const char *end) {
			if (cursor > end) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			try {
				membuf valueBuffer(const_cast<char *>(cursor), static_cast<size_t>(end - cursor));
				std::istream stream(&valueBuffer);
				auto value = utils::Serializer::deserialize<PropertyValue>(stream);
				const size_t consumed = valueBuffer.consumed();
				if (consumed == 0 || static_cast<size_t>(end - cursor) < consumed) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				cursor += consumed;
				return value;
			} catch (...) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValue(const char *&cursor, const char *end) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			switch (type) {
				case PropertyType::NULL_TYPE:
					return PropertyValue();
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::STRING: {
					std::string value;
					if (!readString(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(std::move(value));
				}
				case PropertyType::DATE: {
					TemporalDate value;
					if (!readPod(cursor, end, value.epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DATETIME: {
					TemporalDateTime value;
					if (!readPod(cursor, end, value.epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			std::unordered_map<std::string, PropertyValue> values;
			values.reserve(std::min<size_t>(header->propertyCount, requestedKeys.size()));
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (requestedKeys.contains(key)) {
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					values.emplace(std::move(key), std::move(*value));
				} else if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
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

		struct PropertyOwnerPredicateScanState {
			std::vector<char> readBuffer;
			std::vector<int64_t> ownerIds;
		};

		struct PropertyOwnerValueScanState {
			std::vector<char> readBuffer;
			std::vector<PropertyEntityOwnerValue> values;
		};

		struct PropertyOwnerKeyValueScanState {
			std::vector<char> readBuffer;
			std::vector<PropertyEntityOwnerKeyValue> values;
		};

		struct PropertyPredicateCountScanState {
			std::vector<char> readBuffer;
			PropertyEntityPredicateCountResult count;
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
			work.reserve(std::min(sortedIds.size(), segmentIndex.size()));
			size_t idCursor = 0;
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				const auto &entry = segmentIndex[segment];
				while (idCursor < sortedIds.size() && sortedIds[idCursor] < entry.startId) {
					++idCursor;
				}
				const size_t idBegin = idCursor;
				while (idCursor < sortedIds.size() && sortedIds[idCursor] <= entry.endId) {
					++idCursor;
				}
				if (idBegin != idCursor) {
					work.push_back({segment, idBegin, idCursor});
				}
				if (idCursor == sortedIds.size()) {
					break;
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
				return false; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				auto keyIt = requestedKeyIndices.find(key);
				if (keyIt == requestedKeyIndices.end()) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
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

		bool readSelectedPropertyColumnsOne(
				const char *buf,
				const std::unordered_map<std::string, size_t> &requestedKeyIndices,
				const std::vector<std::vector<std::optional<PropertyValue>> *> &columnTargets,
				size_t row) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return false; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				auto keyIt = requestedKeyIndices.find(key);
				if (keyIt == requestedKeyIndices.end()) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}
				(*columnTargets[keyIt->second])[row] = std::move(*value);
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *value);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<size_t> visitSelectedPropertyValueOne(const char *buf,
															const std::string &requestedKey,
															size_t row,
															const PropertyEntityValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				visitor(row, *value);
				return size_t{1};
			}
			return size_t{0};
		}

		bool ownerFilterContains(std::span<const int64_t> sortedOwnerIds, int64_t ownerId) {
			return sortedOwnerIds.empty() ||
				   std::binary_search(sortedOwnerIds.begin(), sortedOwnerIds.end(), ownerId);
		}

		std::optional<PropertyEntityOwnerValue> readPropertyOwnerValue(
				const char *buf,
				EntityType ownerType,
				const std::string &requestedKey,
				std::span<const int64_t> sortedOwnerIds) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (header->entityType != toUnderlying(ownerType) ||
				!ownerFilterContains(sortedOwnerIds, header->entityId)) {
				return std::nullopt;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return PropertyEntityOwnerValue{header->entityId, std::move(*value)};
			}
			return std::nullopt;
		}

		std::vector<PropertyEntityOwnerKeyValue> readPropertyOwnerKeyValues(
				const char *buf,
				EntityType ownerType,
				std::span<const std::string> requestedKeys,
				std::span<const int64_t> sortedOwnerIds) {
			std::vector<PropertyEntityOwnerKeyValue> values;
			if (requestedKeys.empty()) {
				return values;
			}

			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return {}; // ZYX_COV_EXCL_LINE
			}
			if (header->entityType != toUnderlying(ownerType) ||
				!ownerFilterContains(sortedOwnerIds, header->entityId)) {
				return values;
			}

			values.reserve(std::min<size_t>(header->propertyCount, requestedKeys.size()));
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return {}; // ZYX_COV_EXCL_LINE
				}

				const std::string *matchedKey = nullptr;
				for (const auto &requestedKey: requestedKeys) {
					if (stringViewEquals(key, requestedKey)) {
						matchedKey = &requestedKey;
						break;
					}
				}

				if (matchedKey == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return {}; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return {}; // ZYX_COV_EXCL_LINE
				}
				values.push_back(PropertyEntityOwnerKeyValue{header->entityId, *matchedKey, std::move(*value)});
			}
			return values;
		}

		std::optional<PropertyEntityScalarValue>
		readSerializedPropertyScalarValue(const char *&cursor, const char *end,
										  std::optional<PropertyValue> &fallbackStorage) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			PropertyEntityScalarValue scalar;
			scalar.type = type;
			switch (type) {
				case PropertyType::NULL_TYPE:
					return scalar;
				case PropertyType::BOOLEAN:
					if (!readPod(cursor, end, scalar.boolValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::INTEGER:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::DOUBLE:
					if (!readPod(cursor, end, scalar.doubleValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					scalar.stringValue = std::string_view(value.data, value.size);
					return scalar;
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					scalar.intValue = epochDays;
					return scalar;
				}
				case PropertyType::DATETIME:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::DURATION:
					if (!readPod(cursor, end, scalar.durationValue.months) ||
						!readPod(cursor, end, scalar.durationValue.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, scalar.durationValue.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
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
						return std::nullopt; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				std::optional<PropertyValue> fallbackStorage;
				auto scalar = readSerializedPropertyScalarValue(cursor, end, fallbackStorage);
				if (!scalar.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *scalar);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<size_t> visitSelectedPropertyScalarValueOne(const char *buf,
																  const std::string &requestedKey,
																  size_t row,
																  const PropertyEntityScalarValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				std::optional<PropertyValue> fallbackStorage;
				auto scalar = readSerializedPropertyScalarValue(cursor, end, fallbackStorage);
				if (!scalar.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				visitor(row, *scalar);
				return size_t{1};
			}
			return size_t{0};
		}

		std::optional<bool> serializedPropertyValueEquals(const char *&cursor, const char *end,
														  const CompiledPropertyValue &expected) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (type != expected.type) {
				return false; // ZYX_COV_EXCL_LINE
			}

			switch (type) {
				case PropertyType::NULL_TYPE: // ZYX_COV_EXCL_LINE
					return true; // ZYX_COV_EXCL_LINE
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.boolValue;
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.intValue;
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.doubleValue;
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return expected.stringValue != nullptr && stringViewEquals(value, *expected.stringValue);
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return epochDays == expected.dateValue.epochDays;
				}
				case PropertyType::DATETIME: {
					int64_t epochMillis = 0;
					if (!readPod(cursor, end, epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return epochMillis == expected.dateTimeValue.epochMillis;
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.durationValue;
				}
				case PropertyType::LIST: // ZYX_COV_EXCL_LINE
				case PropertyType::MAP: { // ZYX_COV_EXCL_LINE
					cursor = valueStart;
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return expected.value != nullptr && value.value() == *expected.value;
				}
				default: // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			size_t matchedKeys = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				const auto *expectedEntry = findPredicateExpectation(key, expected);
				if (expectedEntry == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto matches = serializedPropertyValueEquals(cursor, end, expectedEntry->value);
				if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				if (!matches.value()) {
					return false; // ZYX_COV_EXCL_LINE
				}
				++matchedKeys;
				if (matchedKeys == expected.size()) {
					return true;
				}
			}
			return matchedKeys == expected.size();
		}

		std::optional<bool> readPropertyEntitySinglePredicateMatch(const char *buf,
																   const SinglePredicateExpectation &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, *expected.key)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
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
				case PropertyEntityPredicateOp::PEP_NE: // ZYX_COV_EXCL_LINE
					return actual != *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LT: // ZYX_COV_EXCL_LINE
					return actual < *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LE: // ZYX_COV_EXCL_LINE
					return actual <= *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GT: // ZYX_COV_EXCL_LINE
					return actual > *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GE: // ZYX_COV_EXCL_LINE
					return actual >= *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: // ZYX_COV_EXCL_LINE
					return expected.upperValue != nullptr && actual >= *expected.value && // ZYX_COV_EXCL_LINE
						   actual <= *expected.upperValue; // ZYX_COV_EXCL_LINE
			}
			return false; // ZYX_COV_EXCL_LINE
		}

		template<typename T>
		bool typedValueSatisfiesPredicate(const T &actual, const PredicateSpecExpectation &expected) {
			const auto *expectedValue = std::get_if<T>(&expected.value->getVariant());
			if (expectedValue == nullptr) {
				return propertyValueSatisfiesPredicate(PropertyValue(actual), expected);
			}

			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ: // ZYX_COV_EXCL_LINE
					return actual == *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_NE: // ZYX_COV_EXCL_LINE
					return actual != *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LT: // ZYX_COV_EXCL_LINE
					return actual < *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LE: // ZYX_COV_EXCL_LINE
					return actual <= *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GT: // ZYX_COV_EXCL_LINE
					return actual > *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GE: // ZYX_COV_EXCL_LINE
					return actual >= *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: { // ZYX_COV_EXCL_LINE
					if (expected.upperValue == nullptr) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					} // ZYX_COV_EXCL_LINE
					const auto *upperValue = std::get_if<T>(&expected.upperValue->getVariant()); // ZYX_COV_EXCL_LINE
					return upperValue != nullptr && actual >= *expectedValue && actual <= *upperValue; // ZYX_COV_EXCL_LINE
				} // ZYX_COV_EXCL_LINE
			}
			return false; // ZYX_COV_EXCL_LINE
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
						return false; // ZYX_COV_EXCL_LINE
					}
					const auto *upperValue = std::get_if<std::string>(&expected.upperValue->getVariant());
					return upperValue != nullptr && comparison >= 0 && compareStringView(actual, *upperValue) <= 0;
				}
			}
			return false; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			if (type != expected.value->getType()) {
				cursor = valueStart;
				auto actual = readSerializedPropertyValue(cursor, end);
				if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return propertyValueSatisfiesPredicate(*actual, expected);
			}

			switch (type) {
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return stringViewSatisfiesPredicate(value, expected);
				}
				case PropertyType::DATE: {
					TemporalDate value;
					if (!readPod(cursor, end, value.epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DATETIME: {
					TemporalDateTime value;
					if (!readPod(cursor, end, value.epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				default:
					cursor = valueStart;
					auto actual = readSerializedPropertyValue(cursor, end);
					if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
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
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			size_t matchedPredicates = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				const auto *group = findPredicateSpecGroup(key, expectedGroups);
				if (group == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				for (const auto *entry: group->predicates) {
					const char *valueCursor = cursor;
					auto matches = readSerializedPropertyValueSatisfiesPredicate(valueCursor, end, *entry);
					if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					if (!matches.value()) {
						return false; // ZYX_COV_EXCL_LINE
					}
					++matchedPredicates;
				}
				if (matchedPredicates == expectedPredicateCount) {
					return true;
				}
				if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
			}
			return matchedPredicates == expectedPredicateCount;
		}

		std::optional<bool> readPropertyEntitySinglePredicateSpecMatch(
				const char *buf, const PredicateSpecExpectation &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, *expected.key)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				return readSerializedPropertyValueSatisfiesPredicate(cursor, end, expected);
			}
			return false;
		}

		bool canUseDirectOrderedRows(const std::vector<int64_t> &ids,
									 const std::vector<size_t> &rows,
									 size_t rowCount) {
			if (ids.empty() || rows.size() != ids.size() || rowCount == 0) {
				return false;
			}

			int64_t previousId = 0;
			size_t previousRow = 0;
			for (size_t i = 0; i < ids.size(); ++i) {
				if (ids[i] <= 0 || rows[i] >= rowCount || (i > 0 && (ids[i] <= previousId || rows[i] <= previousRow))) {
					return false;
				}
				previousId = ids[i];
				previousRow = rows[i];
			}
			return true;
		}


		template<typename EntityVisitor>
		size_t visitPropertyEntityRowsDirect(const DataManager &dm,
											 const std::vector<int64_t> &ids,
											 const std::vector<size_t> &rows,
											 EntityVisitor &&visitor) {
			if (!dm.hasPreadSupport()) {
				return 0;
			}
			const auto segmentIndexManager = dm.getSegmentIndexManager();
			const auto segmentTracker = dm.getSegmentTracker();
			if (!segmentIndexManager || !segmentTracker) { // ZYX_COV_EXCL_LINE
				return 0; // ZYX_COV_EXCL_LINE
			}

			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work = collectPropertyEntitySegmentWork(
					std::span<const int64_t>(ids.data(), ids.size()), segIndex);
			if (work.empty()) {
				return 0;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, size_t &visited) {
				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					const int64_t id = ids[i];
					const auto slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					visited += visitor(rows[i], entityBuffer);
				}
			};

			const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
			auto groups = buildCoalescedGroups(workSegIndices, segIndex);

			size_t visited = 0;
			auto &readBuffer = propertyEntityScanScratchBuffer(0);
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
					scanPropertyWork(w, header, readBuffer.data(), visited);
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
						scanPropertyWork(w, header, dataBuf, visited);
					}
				}
			}
			return visited;
		}


		template<typename Matcher>
		PropertyEntityPredicateCountResult countPropertyEntityMatches(const DataManager &dm,
																	  const std::vector<int64_t> &ids,
																	  concurrent::ThreadPool *pool,
																	  Matcher &&matchesPredicate) {
			if (ids.empty() || !dm.hasPreadSupport()) {
				return {};
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
				return {};
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
				return {}; // ZYX_COV_EXCL_LINE
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
			if (work.empty()) {
				return {};
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, PropertyEntityPredicateCountResult &targetCount) {
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
						const size_t count = multiplicities.empty() ? size_t{1} : multiplicities[i];
						targetCount.loadedCount += count;
						targetCount.matchedCount += count;
					} else if (matches.has_value()) {
						targetCount.loadedCount += multiplicities.empty() ? size_t{1} : multiplicities[i];
					}
				}
			};

			const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
			PropertyEntityPredicateCountResult count;
			const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
			if (decision.useParallel) { // ZYX_COV_EXCL_LINE
				std::vector<PropertyEntityPredicateCountResult> perWorkCounts(work.size());
				pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
					const auto &task = tasks[taskIndex];
					const auto &group = groups[task.groupIndex];
					const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
					std::vector<char> groupBuf(totalBytes);
					const ssize_t n = dm.preadSegments(groupBuf.data(), task.segCount, task.startOffset);
					if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
						return;
					}

					for (size_t mi = 0; mi < task.memberCount; ++mi) {
						const size_t wi = group.memberIndices[task.memberBegin + mi];
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

				for (const auto &perWorkCount: perWorkCounts) {
					count.loadedCount += perWorkCount.loadedCount;
					count.matchedCount += perWorkCount.matchedCount;
				}
				return count;
			}

			auto &readBuffer = propertyEntityScanScratchBuffer(0);
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

		template<typename Matcher>
		PropertyEntityPredicateMatchResult matchPropertyEntityRows(const DataManager &dm,
																   const std::vector<int64_t> &ids,
																   const std::vector<size_t> &rows, size_t rowCount,
																   concurrent::ThreadPool *pool,
																   PropertyEntityPredicateMatchOptions options,
																   Matcher &&matchesPredicate) {
			PropertyEntityPredicateMatchResult result;
			if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || !dm.hasPreadSupport()) {
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

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			const auto segmentTracker = dm.getSegmentTracker();
			if (!segmentIndexManager || !segmentTracker) { // ZYX_COV_EXCL_LINE
				return result; // ZYX_COV_EXCL_LINE
			}

			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work =
					collectPropertyEntitySegmentWork(std::span<const int64_t>(sortedIds.data(), sortedIds.size()),
													 segIndex);
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

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, std::vector<size_t> &loadedRows,
										std::vector<size_t> *matchedRows, size_t &loadedCount,
										size_t &matchedCount) {
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
					appendPredicateResult(loadedRows, matchedRows, loadedCount, matchedCount, i,
										  matchesPredicate(entityBuffer));
				}
			};

			const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
			const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
			if (decision.useParallel) { // ZYX_COV_EXCL_LINE
				std::vector<std::vector<size_t>> perWorkLoadedRows(options.collectLoadedRows ? work.size() : 0);
				std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
				std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
				std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

				pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
					const auto &task = tasks[taskIndex];
					const auto &group = groups[task.groupIndex];
					const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
					std::vector<char> groupBuf(totalBytes);
					const ssize_t n = dm.preadSegments(groupBuf.data(), task.segCount, task.startOffset);
					if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
						return;
					}

					for (size_t mi = 0; mi < task.memberCount; ++mi) {
						const size_t wi = group.memberIndices[task.memberBegin + mi];
						const auto &w = work[wi];
						const size_t bufferOffset = mi * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, groupBuf.data() + bufferOffset, sizeof(SegmentHeader));
						if (header.used == 0) {
							continue;
						}
						scanPropertyWork(w, header, groupBuf.data() + bufferOffset + sizeof(SegmentHeader),
										 options.collectLoadedRows ? perWorkLoadedRows[wi] : result.loadedRows,
										 options.collectMatchedRows ? &perWorkMatchedRows[wi] : nullptr,
										 perWorkLoadedCounts[wi], perWorkMatchedCounts[wi]);
					}
				});

				for (size_t i = 0; i < work.size(); ++i) {
					result.loadedCount += perWorkLoadedCounts[i];
					result.matchedCount += perWorkMatchedCounts[i];
				}
				if (options.collectLoadedRows) {
					result.loadedRows.reserve(result.loadedCount);
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
				return result;
			}

			if (options.collectLoadedRows) {
				result.loadedRows.reserve(refs.size());
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(refs.size());
			}
			auto &readBuffer = propertyEntityScanScratchBuffer(0);
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
					scanPropertyWork(w, header, readBuffer.data(), result.loadedRows,
									 options.collectMatchedRows ? &result.matchedRows : nullptr, result.loadedCount,
									 result.matchedCount);
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
						scanPropertyWork(w, header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
										 result.loadedRows,
										 options.collectMatchedRows ? &result.matchedRows : nullptr,
										 result.loadedCount, result.matchedCount);
					}
				}
			}
			return result;
		}

		std::vector<PropertyEntityOwnerValue> collectPropertyValuesByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				const std::string &key,
				std::span<const int64_t> sortedOwnerIds,
				concurrent::ThreadPool *pool) {
			std::vector<PropertyEntityOwnerValue> values;
			if (key.empty() || !dm.hasPreadSupport()) {
				return values;
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return values; // ZYX_COV_EXCL_LINE
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			if (segIndex.empty()) {
				return values;
			}

			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(segIndex.size());
			for (size_t index = 0; index < segIndex.size(); ++index) {
				segmentIndices.push_back(index);
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanSegmentInto = [&](const SegmentHeader &header,
									   const char *dataBuf,
									   std::vector<PropertyEntityOwnerValue> &target) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (readSerializedPropertyId(entityBuffer) != expectedId) {
						continue; // ZYX_COV_EXCL_LINE
					}
					auto value = readPropertyOwnerValue(entityBuffer, ownerType, key, sortedOwnerIds);
					if (value.has_value()) {
						target.push_back(std::move(*value));
					}
				}
			};

			const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
			auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
			const size_t segmentCount = totalCoalescedSegments(groups);
			const auto decision = decidePropertyEntityScan(pool, tasks.size(), segmentCount);
			if (decision.useParallel) {
				(void) concurrent::runIndexedPartitions<PropertyOwnerValueScanState>(
						tasks.size(),
						pool,
						{.phase = "property_owner_value_scan.parallel",
						 .workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
						 .estimatedItems = segmentCount,
						 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
						 .minPartitions = kMinParallelPropertyReadTasks,
						 .minItems = kMinParallelPropertyReadSegments},
						[&](size_t taskIndex, PropertyOwnerValueScanState &state) {
							const auto &task = tasks[taskIndex];
							const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
							state.readBuffer.resize(totalBytes);
							const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
							if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
								std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
								return; // ZYX_COV_EXCL_LINE
							}

							for (size_t member = 0; member < task.memberCount; ++member) {
								const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
								SegmentHeader header;
								std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
								scanSegmentInto(
										header,
										state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
										state.values);
							}
							std::vector<char>().swap(state.readBuffer);
						},
						[&](size_t, PropertyOwnerValueScanState &state) {
							values.insert(values.end(),
										  std::make_move_iterator(state.values.begin()),
										  std::make_move_iterator(state.values.end()));
						});
				return values;
			}

			auto &readBuffer = propertyEntityScanScratchBuffer(0);
			for (const auto &group: groups) {
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
					for (size_t member = 0; member < chunkSegments; ++member) {
						const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
						scanSegmentInto(header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), values);
					}
				}
			}
			return values;
			}

			std::vector<PropertyEntityOwnerKeyValue> collectPropertyValuesByOwnerType(
					const DataManager &dm,
					EntityType ownerType,
					const std::vector<std::string> &keys,
					std::span<const int64_t> sortedOwnerIds,
					concurrent::ThreadPool *pool) {
				std::vector<PropertyEntityOwnerKeyValue> values;
				if (keys.empty() || !dm.hasPreadSupport()) {
					return values;
				}

				std::vector<std::string> requestedKeys;
				requestedKeys.reserve(keys.size());
				for (const auto &key: keys) {
					if (key.empty() ||
						std::find(requestedKeys.begin(), requestedKeys.end(), key) != requestedKeys.end()) {
						continue;
					}
					requestedKeys.push_back(key);
				}
				if (requestedKeys.empty()) {
					return values;
				}

				const auto segmentIndexManager = dm.getSegmentIndexManager();
				if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
					return values; // ZYX_COV_EXCL_LINE
				}
				const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
				if (segIndex.empty()) {
					return values;
				}

				std::vector<size_t> segmentIndices;
				segmentIndices.reserve(segIndex.size());
				for (size_t index = 0; index < segIndex.size(); ++index) {
					segmentIndices.push_back(index);
				}

				constexpr size_t entitySize = Property::getTotalSize();
				auto scanSegmentInto = [&](const SegmentHeader &header,
										   const char *dataBuf,
										   std::vector<PropertyEntityOwnerKeyValue> &target) {
					if (header.data_type != Property::typeId || header.used == 0) {
						return;
					}
					for (uint32_t slot = 0; slot < header.used; ++slot) {
						const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
						const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
						if (readSerializedPropertyId(entityBuffer) != expectedId) {
							continue; // ZYX_COV_EXCL_LINE
						}
						auto entityValues = readPropertyOwnerKeyValues(
								entityBuffer, ownerType,
								std::span<const std::string>(requestedKeys.data(), requestedKeys.size()),
								sortedOwnerIds);
						target.insert(target.end(),
									  std::make_move_iterator(entityValues.begin()),
									  std::make_move_iterator(entityValues.end()));
					}
				};

				const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
				auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
				const size_t segmentCount = totalCoalescedSegments(groups);
				const auto decision = decidePropertyEntityScan(pool, tasks.size(), segmentCount);
				if (decision.useParallel) {
					(void) concurrent::runIndexedPartitions<PropertyOwnerKeyValueScanState>(
							tasks.size(),
							pool,
							{.phase = "property_owner_key_value_scan.parallel",
							 .workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
							 .estimatedItems = segmentCount,
							 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
							 .minPartitions = kMinParallelPropertyReadTasks,
							 .minItems = kMinParallelPropertyReadSegments},
							[&](size_t taskIndex, PropertyOwnerKeyValueScanState &state) {
								const auto &task = tasks[taskIndex];
								const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
								state.readBuffer.resize(totalBytes);
								const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
								if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
									std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
									return; // ZYX_COV_EXCL_LINE
								}

								for (size_t member = 0; member < task.memberCount; ++member) {
									const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
									SegmentHeader header;
									std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
									scanSegmentInto(
											header,
											state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
											state.values);
								}
								std::vector<char>().swap(state.readBuffer);
							},
							[&](size_t, PropertyOwnerKeyValueScanState &state) {
								values.insert(values.end(),
											  std::make_move_iterator(state.values.begin()),
											  std::make_move_iterator(state.values.end()));
							});
					return values;
				}

				auto &readBuffer = propertyEntityScanScratchBuffer(0);
				for (const auto &group: groups) {
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
						for (size_t member = 0; member < chunkSegments; ++member) {
							const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
							SegmentHeader header;
							std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
							scanSegmentInto(header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), values);
						}
					}
				}
				return values;
			}

			template<typename Matcher>
			std::vector<int64_t> collectPropertyPredicateOwnerIdsByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				const PropertyEntityOwnerPredicateScanOptions &options,
				concurrent::ThreadPool *pool,
				Matcher &&matchesPredicate) {
			std::vector<int64_t> ownerIds;
			if (!dm.hasPreadSupport() || options.beginOwnerId > options.endOwnerId) {
				return ownerIds;
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return ownerIds; // ZYX_COV_EXCL_LINE
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			if (segIndex.empty()) {
				return ownerIds;
			}

			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(segIndex.size());
			for (size_t index = 0; index < segIndex.size(); ++index) {
				segmentIndices.push_back(index);
			}

			constexpr size_t entitySize = Property::getTotalSize();
			const auto ownerTypeId = toUnderlying(ownerType);
			auto scanSegmentInto = [&](const SegmentHeader &header, const char *dataBuf, std::vector<int64_t> &target) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const char *cursor = entityBuffer;
					const char *end = entityBuffer + Property::TOTAL_PROPERTY_SIZE;
					PropertyRecordHeader propertyHeader;
					if (!readPropertyRecordHeader(cursor, end, propertyHeader) ||
						!propertyHeader.active || propertyHeader.propertyId == 0 ||
						propertyHeader.entityType != ownerTypeId) {
						continue;
					}
					if (propertyHeader.entityId < options.beginOwnerId ||
						propertyHeader.entityId > options.endOwnerId) {
						continue;
					}
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (propertyHeader.propertyId != expectedId) {
						continue;
					}
					auto matches = matchesPredicate(entityBuffer);
					if (matches.has_value() && matches.value()) {
						target.push_back(propertyHeader.entityId);
					}
				}
			};

			const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
			auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
			const size_t segmentCount = totalCoalescedSegments(groups);
			const auto decision = decidePropertyEntityScan(pool, tasks.size(), segmentCount);
			if (decision.useParallel) {
				(void) concurrent::runIndexedPartitions<PropertyOwnerPredicateScanState>(
						tasks.size(),
						pool,
						{.phase = "property_owner_scan.parallel",
						 .workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
						 .estimatedItems = segmentCount,
						 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
						 .minPartitions = kMinParallelPropertyReadTasks,
						 .minItems = kMinParallelPropertyReadSegments},
						[&](size_t taskIndex, PropertyOwnerPredicateScanState &state) {
							const auto &task = tasks[taskIndex];
							const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
							state.readBuffer.resize(totalBytes);
							const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
							if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
								return;
							}

							for (size_t member = 0; member < task.memberCount; ++member) {
								const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
								SegmentHeader header;
								std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
								scanSegmentInto(
										header,
										state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
										state.ownerIds);
							}
							std::vector<char>().swap(state.readBuffer);
						},
						[&](size_t, PropertyOwnerPredicateScanState &state) {
							ownerIds.insert(ownerIds.end(),
											std::make_move_iterator(state.ownerIds.begin()),
											std::make_move_iterator(state.ownerIds.end()));
						});
				if (options.deduplicateOwnerIds && ownerIds.size() > 1) {
					std::sort(ownerIds.begin(), ownerIds.end());
					ownerIds.erase(std::unique(ownerIds.begin(), ownerIds.end()), ownerIds.end());
				}
				return ownerIds;
			}

			auto &readBuffer = propertyEntityScanScratchBuffer(0);
			for (const auto &group: groups) {
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
					for (size_t member = 0; member < chunkSegments; ++member) {
						const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
						scanSegmentInto(header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), ownerIds);
					}
				}
			}

			if (options.deduplicateOwnerIds && ownerIds.size() > 1) {
				std::sort(ownerIds.begin(), ownerIds.end());
				ownerIds.erase(std::unique(ownerIds.begin(), ownerIds.end()), ownerIds.end());
			}
			return ownerIds;
		}

		template<typename Matcher>
		PropertyEntityPredicateCountResult countPropertyEntityMatchesByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				concurrent::ThreadPool *pool,
				Matcher &&matchesPredicate) {
			PropertyEntityPredicateCountResult count;
			if (!dm.hasPreadSupport()) {
				return count;
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return count; // ZYX_COV_EXCL_LINE
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			if (segIndex.empty()) {
				return count;
			}

			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(segIndex.size());
			for (size_t i = 0; i < segIndex.size(); ++i) {
				segmentIndices.push_back(i);
			}

			const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
			constexpr size_t entitySize = Property::getTotalSize();
			const auto ownerTypeId = toUnderlying(ownerType);
			auto scanSegment = [&](const SegmentHeader &header,
								   const char *dataBuf,
								   PropertyEntityPredicateCountResult &target) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const char *cursor = entityBuffer;
					const char *end = entityBuffer + Property::TOTAL_PROPERTY_SIZE;
					PropertyRecordHeader propertyHeader;
					if (!readPropertyRecordHeader(cursor, end, propertyHeader) ||
						!propertyHeader.active || propertyHeader.propertyId == 0 ||
						propertyHeader.entityType != ownerTypeId) {
						continue;
					}
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (propertyHeader.propertyId != expectedId) {
						continue;
					}
					auto matches = matchesPredicate(entityBuffer);
					if (!matches.has_value()) {
						continue;
					}
					++target.loadedCount;
					if (matches.value()) {
						++target.matchedCount;
					}
				}
			};

			auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
			const size_t segmentCount = totalCoalescedSegments(groups);
			const auto decision = decidePropertyEntityScan(pool, tasks.size(), segmentCount);
			if (decision.useParallel) { // ZYX_COV_EXCL_LINE
				(void) concurrent::runIndexedPartitions<PropertyPredicateCountScanState>(
						tasks.size(),
						pool,
						{.phase = "property_predicate_count.parallel",
						 .workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
						 .estimatedItems = segmentCount,
						 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
						 .minPartitions = kMinParallelPropertyReadTasks,
						 .minItems = kMinParallelPropertyReadSegments},
						[&](size_t taskIndex, PropertyPredicateCountScanState &state) {
							const auto &task = tasks[taskIndex];
							const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
							state.readBuffer.resize(totalBytes);
							const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
							if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
								std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
								return; // ZYX_COV_EXCL_LINE
							}
							for (size_t member = 0; member < task.memberCount; ++member) {
								const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
								SegmentHeader header;
								std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
								scanSegment(header, state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
											state.count);
							}
							std::vector<char>().swap(state.readBuffer);
						},
						[&](size_t, PropertyPredicateCountScanState &state) {
							count.loadedCount += state.count.loadedCount;
							count.matchedCount += state.count.matchedCount;
						});
				return count;
			}

			auto &readBuffer = propertyEntityScanScratchBuffer(0);
			for (const auto &group: groups) {
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
					for (size_t member = 0; member < chunkSegments; ++member) {
						const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
						SegmentHeader header;
						std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
						scanSegment(header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), count);
					}
				}
			}
			return count;
		}

	} // namespace

} // namespace graph::storage
