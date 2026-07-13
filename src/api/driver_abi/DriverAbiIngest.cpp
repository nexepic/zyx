#include "DriverAbiInternal.hpp"

#include "../DatabaseBulkInternal.hpp"
#include "graph/storage/data/ColumnarBulkInput.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

	constexpr int64_t kMaxBatchRows = 10'000'000;
	constexpr int64_t kMaxSchemaFields = 1'024;
	constexpr size_t kMaxSchemaNodes = 4'096;
	constexpr size_t kMaxSchemaNesting = 64;
	constexpr int64_t kMaxVariableBytes = 1LL << 30;
	constexpr size_t kMaxSchemaTextBytes = 64 * 1024;
	constexpr size_t kMaxTotalSchemaTextBytes = 1 * 1024 * 1024;
	constexpr uint64_t kMaxDecodedMemoryBytes = 512ULL * 1024 * 1024;
	constexpr uint64_t kMapEntryOverheadBytes = sizeof(std::string) + 8 * sizeof(void *);

	enum class ArrowColumnKind {
		ACK_NULL,
		ACK_BOOL,
		ACK_INT8,
		ACK_UINT8,
		ACK_INT16,
		ACK_UINT16,
		ACK_INT32,
		ACK_UINT32,
		ACK_INT64,
		ACK_UINT64,
		ACK_FLOAT32,
		ACK_FLOAT64,
		ACK_UTF8,
		ACK_LARGE_UTF8,
		ACK_LIST,
		ACK_LARGE_LIST,
		ACK_FIXED_SIZE_LIST,
		ACK_STRUCT,
		ACK_MAP,
		ACK_DICTIONARY
	};

	enum class IngestState { IS_ACTIVE, IS_FAILED, IS_COMMITTED, IS_ROLLED_BACK };

	struct ArrowColumnPlan {
		ArrowColumnKind kind = ArrowColumnKind::ACK_NULL;
		ArrowColumnKind dictionaryIndexKind = ArrowColumnKind::ACK_NULL;
		std::string name;
		int64_t fixedSize = 0;
		std::vector<ArrowColumnPlan> children;
		std::unique_ptr<ArrowColumnPlan> dictionary;
	};

	class ArrowInputError final : public std::runtime_error {
	public:
		ArrowInputError(zyx_driver_status_t status, std::string message, int64_t row, std::string fieldPath) :
			std::runtime_error(std::move(message)), status_(status), row_(row), fieldPath_(std::move(fieldPath)) {}

		[[nodiscard]] zyx_driver_status_t status() const noexcept { return status_; }
		[[nodiscard]] int64_t row() const noexcept { return row_; }
		[[nodiscard]] const std::string &fieldPath() const noexcept { return fieldPath_; }

	private:
		zyx_driver_status_t status_;
		int64_t row_;
		std::string fieldPath_;
	};

	class SchemaCompileBudget final {
	public:
		void consumeNode(std::string_view fieldPath) {
			// COSEC: Bound the complete recursive schema, not just each child list, to prevent
			// adversarial Arrow schemas from causing exponential allocation during prepare.
			if (nodes_ == kMaxSchemaNodes) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow schema exceeds the total field limit", -1,
									  std::string(fieldPath));
			}
			++nodes_;
		}

		void consumeText(size_t bytes, std::string_view fieldPath) {
			// COSEC: Account for all copied schema strings so many individually valid names
			// cannot bypass the aggregate preparation-memory limit.
			if (bytes > kMaxTotalSchemaTextBytes - textBytes_) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow schema text exceeds the total byte limit", -1,
									  std::string(fieldPath));
			}
			textBytes_ += bytes;
		}

	private:
		size_t nodes_ = 0;
		size_t textBytes_ = 0;
	};

	class DecodeBudget final {
	public:
		void ensure(uint64_t bytes, int64_t row, std::string_view fieldPath) const {
			// COSEC: Arrow buffers are caller-owned. Bound all materialization performed by
			// this adapter before reserving memory so a valid but hostile batch cannot exhaust
			// the process heap.
			if (bytes > kMaxDecodedMemoryBytes - usedBytes_) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow batch exceeds the decoded-memory limit", row,
									  std::string(fieldPath));
			}
		}

		void consume(uint64_t bytes, int64_t row, std::string_view fieldPath) {
			ensure(bytes, row, fieldPath);
			usedBytes_ += bytes;
		}

		void ensureElements(uint64_t count, uint64_t elementBytes, int64_t row, std::string_view fieldPath) const {
			if (elementBytes != 0 && count > kMaxDecodedMemoryBytes / elementBytes) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow batch exceeds the decoded-memory limit", row,
									  std::string(fieldPath));
			}
			ensure(count * elementBytes, row, fieldPath);
		}

		void consumeElements(uint64_t count, uint64_t elementBytes, int64_t row, std::string_view fieldPath) {
			ensureElements(count, elementBytes, row, fieldPath);
			usedBytes_ += count * elementBytes;
		}

	private:
		uint64_t usedBytes_ = 0;
	};

	std::string boundedCString(const char *value, std::string_view fieldPath) {
		if (value == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, std::string(fieldPath) + " must not be null", -1,
								  std::string(fieldPath));
		}
		const auto *terminator = static_cast<const char *>(std::memchr(value, '\0', kMaxSchemaTextBytes));
		if (terminator == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE,
								  std::string(fieldPath) + " exceeds the maximum supported length", -1,
								  std::string(fieldPath));
		}
		return std::string(value, static_cast<size_t>(terminator - value));
	}

	ArrowColumnKind primitiveKind(std::string_view format, std::string_view fieldPath) {
		if (format == "n")
			return ArrowColumnKind::ACK_NULL;
		if (format == "b")
			return ArrowColumnKind::ACK_BOOL;
		if (format == "c")
			return ArrowColumnKind::ACK_INT8;
		if (format == "C")
			return ArrowColumnKind::ACK_UINT8;
		if (format == "s")
			return ArrowColumnKind::ACK_INT16;
		if (format == "S")
			return ArrowColumnKind::ACK_UINT16;
		if (format == "i")
			return ArrowColumnKind::ACK_INT32;
		if (format == "I")
			return ArrowColumnKind::ACK_UINT32;
		if (format == "l")
			return ArrowColumnKind::ACK_INT64;
		if (format == "L")
			return ArrowColumnKind::ACK_UINT64;
		if (format == "f")
			return ArrowColumnKind::ACK_FLOAT32;
		if (format == "g")
			return ArrowColumnKind::ACK_FLOAT64;
		if (format == "u")
			return ArrowColumnKind::ACK_UTF8;
		if (format == "U")
			return ArrowColumnKind::ACK_LARGE_UTF8;
		throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "unsupported Arrow format '" + std::string(format) + "'", -1,
							  std::string(fieldPath));
	}

	ArrowColumnPlan compileSchema(const ArrowSchema *schema, std::string fieldPath, size_t depth,
								  SchemaCompileBudget &budget) {
		if (depth > kMaxSchemaNesting) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow schema nesting is too deep", -1,
								  std::move(fieldPath));
		}
		if (schema == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow schema child must not be null", -1,
								  std::move(fieldPath));
		}
		budget.consumeNode(fieldPath);
		if (schema->n_children < 0 || schema->n_children > kMaxSchemaFields) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow schema child count is out of range", -1,
								  std::move(fieldPath));
		}

		ArrowColumnPlan plan;
		plan.name = schema->name == nullptr ? std::string{} : boundedCString(schema->name, fieldPath + ".name");
		const std::string format = boundedCString(schema->format, fieldPath + ".format");
		budget.consumeText(plan.name.size(), fieldPath + ".name");
		budget.consumeText(format.size(), fieldPath + ".format");

		if (schema->dictionary != nullptr) {
			if (schema->n_children != 0) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT,
									  "Arrow dictionary index fields must not have children", -1, fieldPath);
			}
			plan.kind = ArrowColumnKind::ACK_DICTIONARY;
			plan.dictionaryIndexKind = primitiveKind(format, fieldPath);
			switch (plan.dictionaryIndexKind) {
				case ArrowColumnKind::ACK_INT8:
				case ArrowColumnKind::ACK_UINT8:
				case ArrowColumnKind::ACK_INT16:
				case ArrowColumnKind::ACK_UINT16:
				case ArrowColumnKind::ACK_INT32:
				case ArrowColumnKind::ACK_UINT32:
				case ArrowColumnKind::ACK_INT64:
				case ArrowColumnKind::ACK_UINT64:
					break;
				default:
					throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow dictionary indices must be integral", -1,
										  fieldPath);
			}
			plan.dictionary = std::make_unique<ArrowColumnPlan>(
					compileSchema(schema->dictionary, fieldPath + ".dictionary", depth + 1, budget));
			return plan;
		}

		if (format == "+s") {
			plan.kind = ArrowColumnKind::ACK_STRUCT;
		} else if (format == "+l") {
			plan.kind = ArrowColumnKind::ACK_LIST;
		} else if (format == "+L") {
			plan.kind = ArrowColumnKind::ACK_LARGE_LIST;
		} else if (format == "+m") {
			plan.kind = ArrowColumnKind::ACK_MAP;
		} else if (format.starts_with("+w:")) {
			plan.kind = ArrowColumnKind::ACK_FIXED_SIZE_LIST;
			const std::string_view sizeText(format.data() + 3, format.size() - 3);
			const auto [end, error] =
					std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), plan.fixedSize);
			if (error != std::errc{} || end != sizeText.data() + sizeText.size() || plan.fixedSize <= 0) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "invalid Arrow fixed-size-list width", -1,
									  fieldPath);
			}
		} else {
			plan.kind = primitiveKind(format, fieldPath);
		}

		const bool expectsOneChild =
				plan.kind == ArrowColumnKind::ACK_LIST || plan.kind == ArrowColumnKind::ACK_LARGE_LIST ||
				plan.kind == ArrowColumnKind::ACK_FIXED_SIZE_LIST || plan.kind == ArrowColumnKind::ACK_MAP;
		if (expectsOneChild && schema->n_children != 1) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow list/map fields require exactly one child", -1,
								  fieldPath);
		}
		if (plan.kind != ArrowColumnKind::ACK_STRUCT && !expectsOneChild && schema->n_children != 0) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow primitive fields must not have children", -1,
								  fieldPath);
		}
		if (schema->n_children > 0 && schema->children == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow schema children must not be null", -1, fieldPath);
		}

		plan.children.reserve(static_cast<size_t>(schema->n_children));
		std::unordered_set<std::string> names;
		names.reserve(static_cast<size_t>(schema->n_children));
		for (int64_t index = 0; index < schema->n_children; ++index) {
			const std::string childPath = fieldPath + ".children[" + std::to_string(index) + "]";
			auto child = compileSchema(schema->children[index], childPath, depth + 1, budget);
			if (plan.kind == ArrowColumnKind::ACK_STRUCT) {
				if (child.name.empty()) {
					throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow struct child names must not be empty", -1,
										  childPath + ".name");
				}
				if (!names.insert(child.name).second) {
					throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow struct child names must be unique", -1,
										  childPath + ".name");
				}
			}
			plan.children.push_back(std::move(child));
		}
		return plan;
	}

	bool resolvesToUtf8(const ArrowColumnPlan &plan) {
		if (plan.kind == ArrowColumnKind::ACK_UTF8 || plan.kind == ArrowColumnKind::ACK_LARGE_UTF8) {
			return true;
		}
		return plan.kind == ArrowColumnKind::ACK_DICTIONARY && plan.dictionary != nullptr &&
			   resolvesToUtf8(*plan.dictionary);
	}

	void validateLogicalSchema(const ArrowColumnPlan &plan, const std::string &fieldPath) {
		if (plan.kind == ArrowColumnKind::ACK_MAP) {
			if (plan.children.size() != 1 || plan.children[0].kind != ArrowColumnKind::ACK_STRUCT ||
				plan.children[0].children.size() != 2) {
				throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow map entries must be a key/value struct", -1,
									  fieldPath);
			}
			if (!resolvesToUtf8(plan.children[0].children[0])) {
				throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow map keys must be UTF-8 strings", -1,
									  fieldPath + ".key");
			}
		}
		for (const auto &child: plan.children) {
			validateLogicalSchema(child, fieldPath + "." + child.name);
		}
		if (plan.dictionary != nullptr) {
			validateLogicalSchema(*plan.dictionary, fieldPath + ".dictionary");
		}
	}

	size_t minimumBufferCount(ArrowColumnKind kind) {
		switch (kind) {
			case ArrowColumnKind::ACK_NULL:
				return 0;
			case ArrowColumnKind::ACK_STRUCT:
			case ArrowColumnKind::ACK_FIXED_SIZE_LIST:
				return 1;
			case ArrowColumnKind::ACK_BOOL:
			case ArrowColumnKind::ACK_INT8:
			case ArrowColumnKind::ACK_UINT8:
			case ArrowColumnKind::ACK_INT16:
			case ArrowColumnKind::ACK_UINT16:
			case ArrowColumnKind::ACK_INT32:
			case ArrowColumnKind::ACK_UINT32:
			case ArrowColumnKind::ACK_INT64:
			case ArrowColumnKind::ACK_UINT64:
			case ArrowColumnKind::ACK_FLOAT32:
			case ArrowColumnKind::ACK_FLOAT64:
			case ArrowColumnKind::ACK_LIST:
			case ArrowColumnKind::ACK_LARGE_LIST:
			case ArrowColumnKind::ACK_MAP:
			case ArrowColumnKind::ACK_DICTIONARY:
				return 2;
			case ArrowColumnKind::ACK_UTF8:
			case ArrowColumnKind::ACK_LARGE_UTF8:
				return 3;
		}
		return 0;
	}

	template<typename T>
	T loadPod(const void *buffer, int64_t index) {
		T value{};
		std::memcpy(&value, static_cast<const uint8_t *>(buffer) + static_cast<size_t>(index) * sizeof(T), sizeof(T));
		return value;
	}

	bool bitIsSet(const void *bitmap, int64_t index) {
		const auto byte = static_cast<const uint8_t *>(bitmap)[static_cast<size_t>(index / 8)];
		return (byte & static_cast<uint8_t>(1U << static_cast<unsigned>(index % 8))) != 0;
	}

	int64_t physicalIndex(const ArrowArray &array, int64_t row, std::string_view fieldPath) {
		if (row < 0 || row >= array.length) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow row index is out of range", row,
								  std::string(fieldPath));
		}
		if (array.offset > (std::numeric_limits<int64_t>::max)() - row) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow row offset overflow", row, std::string(fieldPath));
		}
		return array.offset + row;
	}

	bool isNull(const ArrowArray &array, int64_t row, std::string_view fieldPath) {
		if (array.null_count == 0)
			return false;
		const int64_t index = physicalIndex(array, row, fieldPath);
		if (array.buffers == nullptr || array.n_buffers < 1 || array.buffers[0] == nullptr) {
			if (array.null_count > 0) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow validity bitmap is missing", row,
									  std::string(fieldPath));
			}
			return false;
		}
		return !bitIsSet(array.buffers[0], index);
	}

	int64_t readIntegral(ArrowColumnKind kind, const ArrowArray &array, int64_t row, std::string_view fieldPath) {
		const int64_t index = physicalIndex(array, row, fieldPath);
		const void *data = array.buffers[1];
		switch (kind) {
			case ArrowColumnKind::ACK_INT8:
				return loadPod<int8_t>(data, index);
			case ArrowColumnKind::ACK_UINT8:
				return loadPod<uint8_t>(data, index);
			case ArrowColumnKind::ACK_INT16:
				return loadPod<int16_t>(data, index);
			case ArrowColumnKind::ACK_UINT16:
				return loadPod<uint16_t>(data, index);
			case ArrowColumnKind::ACK_INT32:
				return loadPod<int32_t>(data, index);
			case ArrowColumnKind::ACK_UINT32:
				return loadPod<uint32_t>(data, index);
			case ArrowColumnKind::ACK_INT64:
				return loadPod<int64_t>(data, index);
			case ArrowColumnKind::ACK_UINT64: {
				const uint64_t value = loadPod<uint64_t>(data, index);
				if (value > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
					throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow uint64 value cannot be represented as int64",
										  row, std::string(fieldPath));
				}
				return static_cast<int64_t>(value);
			}
			default:
				throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow value is not integral", row,
									  std::string(fieldPath));
		}
	}

	void validateArray(const ArrowColumnPlan &plan, const ArrowArray *array, int64_t expectedLength,
					   const std::string &fieldPath, size_t depth) {
		if (depth > kMaxSchemaNesting) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow array nesting is too deep", -1, fieldPath);
		}
		if (array == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow array child must not be null", -1, fieldPath);
		}
		if (array->length < 0 || array->length > kMaxBatchRows || array->offset < 0 || array->null_count < -1 ||
			array->null_count > array->length) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow array metadata is out of range", -1, fieldPath);
		}
		// COSEC: Every subsequent buffer lookup derives from offset + length. Validate
		// the addition and pointer-arithmetic range once before touching caller memory.
		if (array->offset > (std::numeric_limits<int64_t>::max)() - array->length) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow array offset and length overflow", -1, fieldPath);
		}
		const auto physicalEnd = static_cast<uint64_t>(array->offset + array->length);
		if (physicalEnd > (std::numeric_limits<size_t>::max)() / sizeof(uint64_t)) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow array offset exceeds the addressable buffer range",
								  -1, fieldPath);
		}
		if (expectedLength >= 0 && array->length != expectedLength) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow child length must match the record batch length",
								  -1, fieldPath);
		}
		const size_t requiredBuffers = minimumBufferCount(plan.kind);
		if (array->n_buffers < static_cast<int64_t>(requiredBuffers) ||
			(requiredBuffers > 0 && array->buffers == nullptr)) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow array has too few buffers", -1, fieldPath);
		}
		const bool hasOffsets = plan.kind == ArrowColumnKind::ACK_UTF8 ||
								plan.kind == ArrowColumnKind::ACK_LARGE_UTF8 ||
								plan.kind == ArrowColumnKind::ACK_LIST ||
								plan.kind == ArrowColumnKind::ACK_LARGE_LIST || plan.kind == ArrowColumnKind::ACK_MAP;
		if (hasOffsets && array->buffers[1] == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow offsets buffer must not be null", -1, fieldPath);
		}
		if (array->length > 0) {
			const bool needsData = requiredBuffers >= 2 && plan.kind != ArrowColumnKind::ACK_LIST &&
								   plan.kind != ArrowColumnKind::ACK_LARGE_LIST &&
								   plan.kind != ArrowColumnKind::ACK_MAP;
			if (needsData && array->buffers[1] == nullptr) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow data buffer must not be null", -1, fieldPath);
			}
		}
		if (plan.kind != ArrowColumnKind::ACK_NULL && array->length > 0 && array->null_count != 0 &&
			(array->buffers == nullptr || array->buffers[0] == nullptr)) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow validity bitmap must not be null", -1, fieldPath);
		}

		if (plan.kind == ArrowColumnKind::ACK_DICTIONARY) {
			if (array->n_children != 0) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow dictionary indices must not have children",
									  -1, fieldPath);
			}
			if (array->dictionary == nullptr || !plan.dictionary) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow dictionary array must not be null", -1,
									  fieldPath);
			}
			validateArray(*plan.dictionary, array->dictionary, -1, fieldPath + ".dictionary", depth + 1);
			return;
		}
		if (array->dictionary != nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT,
								  "Arrow dictionary array is present for a non-dictionary field", -1, fieldPath);
		}

		if (array->n_children != static_cast<int64_t>(plan.children.size())) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT,
								  "Arrow array child count does not match the prepared schema", -1, fieldPath);
		}
		if (!plan.children.empty() && array->children == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow array children must not be null", -1, fieldPath);
		}

		for (size_t index = 0; index < plan.children.size(); ++index) {
			const int64_t childExpected = plan.kind == ArrowColumnKind::ACK_STRUCT ? array->length : -1;
			validateArray(plan.children[index], array->children[index], childExpected,
						  fieldPath + "." + plan.children[index].name, depth + 1);
		}

		if (plan.kind == ArrowColumnKind::ACK_FIXED_SIZE_LIST) {
			const int64_t parentEnd = array->offset + array->length;
			if (parentEnd > (std::numeric_limits<int64_t>::max)() / plan.fixedSize ||
				parentEnd * plan.fixedSize > array->children[0]->length) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow fixed-size-list child is shorter than required",
									  -1, fieldPath);
			}
		}

		if (plan.kind == ArrowColumnKind::ACK_UTF8 || plan.kind == ArrowColumnKind::ACK_LARGE_UTF8 ||
			plan.kind == ArrowColumnKind::ACK_LIST || plan.kind == ArrowColumnKind::ACK_LARGE_LIST ||
			plan.kind == ArrowColumnKind::ACK_MAP) {
			int64_t previous = -1;
			for (int64_t row = 0; row <= array->length; ++row) {
				const int64_t index = array->offset + row;
				const int64_t offset =
						(plan.kind == ArrowColumnKind::ACK_LARGE_UTF8 || plan.kind == ArrowColumnKind::ACK_LARGE_LIST)
								? loadPod<int64_t>(array->buffers[1], index)
								: loadPod<int32_t>(array->buffers[1], index);
				if (offset < 0 || (previous >= 0 && offset < previous)) {
					throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT,
										  "Arrow offsets must be non-negative and monotonic", row == 0 ? 0 : row - 1,
										  fieldPath);
				}
				if ((plan.kind == ArrowColumnKind::ACK_UTF8 || plan.kind == ArrowColumnKind::ACK_LARGE_UTF8) &&
					offset > kMaxVariableBytes) {
					throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow UTF-8 data exceeds the per-batch byte limit",
										  row == 0 ? 0 : row - 1, fieldPath);
				}
				previous = offset;
			}
			if ((plan.kind == ArrowColumnKind::ACK_UTF8 || plan.kind == ArrowColumnKind::ACK_LARGE_UTF8) &&
				previous > 0 && array->buffers[2] == nullptr) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow UTF-8 data buffer must not be null", -1,
									  fieldPath);
			}
			if ((plan.kind == ArrowColumnKind::ACK_LIST || plan.kind == ArrowColumnKind::ACK_LARGE_LIST ||
				 plan.kind == ArrowColumnKind::ACK_MAP) &&
				previous > array->children[0]->length) {
				throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow list offsets exceed the child array length", -1,
									  fieldPath);
			}
		}
	}

	graph::PropertyValue readValue(const ArrowColumnPlan &plan, const ArrowArray &array, int64_t row,
								   const std::string &fieldPath, size_t depth, DecodeBudget &budget);

	graph::PropertyValue readStructValue(const ArrowColumnPlan &plan, const ArrowArray &array, int64_t row,
										 const std::string &fieldPath, size_t depth, DecodeBudget &budget) {
		budget.consume(sizeof(graph::PropertyValue::MapType), row, fieldPath);
		budget.ensureElements(plan.children.size(), kMapEntryOverheadBytes, row, fieldPath);
		graph::PropertyValue::MapType map;
		map.reserve(plan.children.size());
		for (size_t index = 0; index < plan.children.size(); ++index) {
			const auto &child = plan.children[index];
			budget.consume(kMapEntryOverheadBytes + child.name.size(), row, fieldPath);
			map.emplace(child.name,
						readValue(child, *array.children[index], row, fieldPath + "." + child.name, depth + 1, budget));
		}
		return graph::PropertyValue(std::move(map));
	}

	graph::PropertyValue readListValue(const ArrowColumnPlan &plan, const ArrowArray &array, int64_t row,
									   const std::string &fieldPath, size_t depth, DecodeBudget &budget) {
		const int64_t index = physicalIndex(array, row, fieldPath);
		const bool large = plan.kind == ArrowColumnKind::ACK_LARGE_LIST;
		const int64_t begin =
				large ? loadPod<int64_t>(array.buffers[1], index) : loadPod<int32_t>(array.buffers[1], index);
		const int64_t end =
				large ? loadPod<int64_t>(array.buffers[1], index + 1) : loadPod<int32_t>(array.buffers[1], index + 1);
		budget.consume(sizeof(std::vector<graph::PropertyValue>), row, fieldPath);
		budget.ensureElements(static_cast<uint64_t>(end - begin), sizeof(graph::PropertyValue), row, fieldPath);
		std::vector<graph::PropertyValue> list;
		list.reserve(static_cast<size_t>(end - begin));
		const std::string childPath = fieldPath + "[]";
		for (int64_t childRow = begin; childRow < end; ++childRow) {
			list.push_back(readValue(plan.children[0], *array.children[0], childRow, childPath, depth + 1, budget));
		}
		return graph::PropertyValue(std::move(list));
	}

	graph::PropertyValue readMapValue(const ArrowColumnPlan &plan, const ArrowArray &array, int64_t row,
									  const std::string &fieldPath, size_t depth, DecodeBudget &budget) {
		const int64_t index = physicalIndex(array, row, fieldPath);
		const int64_t begin = loadPod<int32_t>(array.buffers[1], index);
		const int64_t end = loadPod<int32_t>(array.buffers[1], index + 1);
		const auto &entriesPlan = plan.children[0];
		const auto &entriesArray = *array.children[0];
		if (entriesPlan.kind != ArrowColumnKind::ACK_STRUCT || entriesPlan.children.size() != 2 ||
			entriesArray.n_children != 2) {
			throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow map entries must be a key/value struct", row,
								  fieldPath);
		}
		const auto entryCount = static_cast<uint64_t>(end - begin);
		budget.consume(sizeof(graph::PropertyValue::MapType), row, fieldPath);
		budget.ensureElements(entryCount, kMapEntryOverheadBytes, row, fieldPath);
		graph::PropertyValue::MapType map;
		map.reserve(static_cast<size_t>(entryCount));
		const std::string keyPath = fieldPath + ".key";
		const std::string valuePath = fieldPath + ".value";
		for (int64_t childRow = begin; childRow < end; ++childRow) {
			auto key =
					readValue(entriesPlan.children[0], *entriesArray.children[0], childRow, keyPath, depth + 1, budget);
			const auto *keyText = std::get_if<std::string>(&key.getVariant());
			if (keyText == nullptr) {
				throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "Arrow map keys must be non-null UTF-8 strings", row,
									  keyPath);
			}
			budget.consume(kMapEntryOverheadBytes + keyText->size(), row, fieldPath);
			auto value = readValue(entriesPlan.children[1], *entriesArray.children[1], childRow, valuePath, depth + 1,
								   budget);
			const auto [_, inserted] = map.emplace(*keyText, std::move(value));
			if (!inserted) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "Arrow map keys must be unique within each row", row,
									  keyPath);
			}
		}
		return graph::PropertyValue(std::move(map));
	}

	graph::PropertyValue readValue(const ArrowColumnPlan &plan, const ArrowArray &array, int64_t row,
								   const std::string &fieldPath, size_t depth, DecodeBudget &budget) {
		if (depth > kMaxSchemaNesting) {
			throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow value nesting is too deep", row, fieldPath);
		}
		budget.consume(sizeof(graph::PropertyValue), row, fieldPath);
		if (plan.kind == ArrowColumnKind::ACK_NULL) {
			return std::monostate{};
		}
		if (isNull(array, row, fieldPath)) {
			return std::monostate{};
		}

		const int64_t index = physicalIndex(array, row, fieldPath);
		switch (plan.kind) {
			case ArrowColumnKind::ACK_NULL:
				return std::monostate{};
			case ArrowColumnKind::ACK_BOOL:
				return bitIsSet(array.buffers[1], index);
			case ArrowColumnKind::ACK_INT8:
			case ArrowColumnKind::ACK_UINT8:
			case ArrowColumnKind::ACK_INT16:
			case ArrowColumnKind::ACK_UINT16:
			case ArrowColumnKind::ACK_INT32:
			case ArrowColumnKind::ACK_UINT32:
			case ArrowColumnKind::ACK_INT64:
			case ArrowColumnKind::ACK_UINT64:
				return readIntegral(plan.kind, array, row, fieldPath);
			case ArrowColumnKind::ACK_FLOAT32:
				return static_cast<double>(loadPod<float>(array.buffers[1], index));
			case ArrowColumnKind::ACK_FLOAT64:
				return loadPod<double>(array.buffers[1], index);
			case ArrowColumnKind::ACK_UTF8:
			case ArrowColumnKind::ACK_LARGE_UTF8: {
				const bool large = plan.kind == ArrowColumnKind::ACK_LARGE_UTF8;
				const int64_t begin =
						large ? loadPod<int64_t>(array.buffers[1], index) : loadPod<int32_t>(array.buffers[1], index);
				const int64_t end = large ? loadPod<int64_t>(array.buffers[1], index + 1)
										  : loadPod<int32_t>(array.buffers[1], index + 1);
				if (begin == end)
					return std::string{};
				budget.consume(static_cast<uint64_t>(end - begin), row, fieldPath);
				return std::string(static_cast<const char *>(array.buffers[2]) + begin,
								   static_cast<size_t>(end - begin));
			}
			case ArrowColumnKind::ACK_LIST:
			case ArrowColumnKind::ACK_LARGE_LIST:
				return readListValue(plan, array, row, fieldPath, depth, budget);
			case ArrowColumnKind::ACK_FIXED_SIZE_LIST: {
				budget.consume(sizeof(std::vector<graph::PropertyValue>), row, fieldPath);
				budget.ensureElements(static_cast<uint64_t>(plan.fixedSize), sizeof(graph::PropertyValue), row,
									  fieldPath);
				std::vector<graph::PropertyValue> list;
				list.reserve(static_cast<size_t>(plan.fixedSize));
				const int64_t parentIndex = physicalIndex(array, row, fieldPath);
				if (parentIndex > (std::numeric_limits<int64_t>::max)() / plan.fixedSize) {
					throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow fixed-size-list offset overflow", row,
										  fieldPath);
				}
				const int64_t begin = parentIndex * plan.fixedSize;
				const std::string childPath = fieldPath + "[]";
				for (int64_t childIndex = 0; childIndex < plan.fixedSize; ++childIndex) {
					list.push_back(readValue(plan.children[0], *array.children[0], begin + childIndex, childPath,
											 depth + 1, budget));
				}
				return graph::PropertyValue(std::move(list));
			}
			case ArrowColumnKind::ACK_STRUCT:
				return readStructValue(plan, array, row, fieldPath, depth, budget);
			case ArrowColumnKind::ACK_MAP:
				return readMapValue(plan, array, row, fieldPath, depth, budget);
			case ArrowColumnKind::ACK_DICTIONARY: {
				const int64_t dictionaryIndex = readIntegral(plan.dictionaryIndexKind, array, row, fieldPath);
				if (dictionaryIndex < 0 || dictionaryIndex >= array.dictionary->length) {
					throw ArrowInputError(ZYX_DRIVER_OUT_OF_RANGE, "Arrow dictionary index is out of range", row,
										  fieldPath);
				}
				return readValue(*plan.dictionary, *array.dictionary, dictionaryIndex, fieldPath, depth + 1, budget);
			}
		}
		throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "unsupported Arrow value", row, fieldPath);
	}

	void validateRootSchema(const ArrowColumnPlan &root) {
		if (root.kind != ArrowColumnKind::ACK_STRUCT || root.children.size() != 3) {
			throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH,
								  "edge schema must be struct<source_id:int64,target_id:int64,properties:struct>", -1,
								  "schema");
		}
		if (root.children[0].name != "source_id" || root.children[0].kind != ArrowColumnKind::ACK_INT64) {
			throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "edge schema field 0 must be source_id:int64", -1,
								  "source_id");
		}
		if (root.children[1].name != "target_id" || root.children[1].kind != ArrowColumnKind::ACK_INT64) {
			throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "edge schema field 1 must be target_id:int64", -1,
								  "target_id");
		}
		if (root.children[2].name != "properties" || root.children[2].kind != ArrowColumnKind::ACK_STRUCT) {
			throw ArrowInputError(ZYX_DRIVER_TYPE_MISMATCH, "edge schema field 2 must be properties:struct", -1,
								  "properties");
		}
	}

	void invalidateIngestors(zyx_driver_ingest_t *ingest);
	void unregisterIngest(zyx_driver_ingest_t *ingest);

} // namespace

struct zyx_driver_ingest_t {
	zyx_driver_ingest_t(zyx::Transaction transaction, zyx_driver_db_t *database) :
		txn(std::move(transaction)), owner(database) {}

	zyx::Transaction txn;
	zyx_driver_db_t *owner = nullptr;
	IngestState state = IngestState::IS_ACTIVE;
	std::unordered_set<zyx_driver_edge_ingestor_t *> ingestors;
};

struct zyx_driver_edge_ingestor_t {
	zyx_driver_edge_ingestor_t(zyx_driver_ingest_t *ingestOwner, std::string type, ArrowColumnPlan compiledSchema) :
		owner(ingestOwner), edgeType(std::move(type)), schema(std::move(compiledSchema)) {}

	zyx_driver_ingest_t *owner = nullptr;
	std::string edgeType;
	ArrowColumnPlan schema;
};

namespace {

	zyx_driver_status_t validateActiveIngest(zyx_driver_ingest_t *ingest, zyx_driver_error_t **outError) {
		if (ingest == nullptr) {
			return setError(outError, ZYX_DRIVER_INVALID_ARGUMENT, "ingest must not be null");
		}
		if (ingest->state == IngestState::IS_FAILED) {
			return setError(outError, ZYX_DRIVER_TRANSACTION_ERROR,
							"ingest session has failed and must be rolled back");
		}
		if (ingest->state != IngestState::IS_ACTIVE || !ingest->txn.isActive()) {
			return setError(outError, ZYX_DRIVER_TRANSACTION_ERROR, "ingest session is not active");
		}
		return ZYX_DRIVER_OK;
	}

	void invalidateIngestors(zyx_driver_ingest_t *ingest) {
		for (auto *ingestor: ingest->ingestors) {
			if (ingestor != nullptr) {
				ingestor->owner = nullptr;
			}
		}
		ingest->ingestors.clear();
	}

	void unregisterIngest(zyx_driver_ingest_t *ingest) {
		if (ingest == nullptr || ingest->owner == nullptr)
			return;
		auto *owner = ingest->owner;
		{
			std::lock_guard lock(owner->mutex);
			owner->active_ingests.erase(ingest);
		}
		ingest->owner = nullptr;
	}

	void poisonIngest(zyx_driver_ingest_t *ingest) {
		if (ingest != nullptr && ingest->state == IngestState::IS_ACTIVE) {
			ingest->state = IngestState::IS_FAILED;
		}
	}

	zyx_driver_status_t catchIngestException(zyx_driver_ingest_t *ingest, zyx_driver_error_t **outError) noexcept {
		poisonIngest(ingest);
		try {
			throw;
		} catch (const ArrowInputError &error) {
			return setErrorAt(outError, error.status(), error.what(), error.row(), error.fieldPath());
		} catch (const graph::storage::BulkInputError &error) {
			return setErrorAt(outError, ZYX_DRIVER_NOT_FOUND, error.what(), static_cast<int64_t>(error.rowIndex()),
							  error.fieldPath());
		} catch (const std::bad_alloc &) {
			return setError(outError, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
		} catch (const std::exception &error) {
			return setError(outError, ZYX_DRIVER_EXECUTION_ERROR, error.what());
		} catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
			return setError(outError, ZYX_DRIVER_INTERNAL_ERROR, "unknown ingest error");
		}
	}

} // namespace

extern "C" {

zyx_driver_status_t zyx_driver_ingest_begin(zyx_driver_db_t *db, zyx_driver_ingest_t **out_ingest,
											zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (out_ingest == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_ingest must not be null");
	}
	*out_ingest = nullptr;
	if (db == nullptr || db->db == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "db must not be null");
	}

	try {
		std::lock_guard lock(db->mutex);
		if (!db->active_txns.empty() || !db->active_ingests.empty() || db->db->hasActiveTransaction()) {
			return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "database has active transactions");
		}
		auto handle = std::make_unique<zyx_driver_ingest_t>(db->db->beginBulkTransaction(), db);
		db->active_ingests.insert(handle.get());
		*out_ingest = handle.release();
		return ZYX_DRIVER_OK;
	} catch (...) {
		return catchTransactionException(out_error);
	}
}

zyx_driver_status_t zyx_driver_ingest_prepare_edges(zyx_driver_ingest_t *ingest, const char *edge_type,
													const ArrowSchema *schema,
													zyx_driver_edge_ingestor_t **out_ingestor,
													zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (out_ingestor == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_ingestor must not be null");
	}
	*out_ingestor = nullptr;
	if (auto status = validateActiveIngest(ingest, out_error); status != ZYX_DRIVER_OK)
		return status;
	if (edge_type == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "edge_type must not be empty");
	}

	try {
		const std::string edgeType = boundedCString(edge_type, "edge_type");
		if (edgeType.empty()) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "edge_type must not be empty", -1, "edge_type");
		}
		SchemaCompileBudget budget;
		auto compiled = compileSchema(schema, "schema", 0, budget);
		validateRootSchema(compiled);
		validateLogicalSchema(compiled, "schema");
		auto handle = std::make_unique<zyx_driver_edge_ingestor_t>(ingest, edgeType, std::move(compiled));
		ingest->ingestors.insert(handle.get());
		*out_ingestor = handle.release();
		return ZYX_DRIVER_OK;
	} catch (const ArrowInputError &error) {
		return setErrorAt(out_error, error.status(), error.what(), error.row(), error.fieldPath());
	} catch (const std::bad_alloc &) {
		return setError(out_error, ZYX_DRIVER_OUT_OF_MEMORY, "out of memory");
	} catch (const std::exception &error) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, error.what());
	} catch (...) { // ZYX_COV_EXCL_LINE: ABI boundary converts unexpected exceptions into status codes.
		return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown schema preparation error");
	}
}

zyx_driver_status_t zyx_driver_edge_ingestor_write(zyx_driver_edge_ingestor_t *ingestor, const ArrowArray *record_batch,
												   zyx_driver_id_range_t *out_ids, zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (out_ids != nullptr) {
		out_ids->first_id = 0;
		out_ids->count = 0;
	}
	if (ingestor == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "ingestor must not be null");
	}
	auto *ingest = ingestor->owner;
	if (ingest == nullptr) {
		return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "the owning ingest session is already finalized");
	}
	if (auto status = validateActiveIngest(ingest, out_error); status != ZYX_DRIVER_OK)
		return status;

	try {
		DecodeBudget decodeBudget;
		if (record_batch != nullptr && record_batch->length >= 0 && record_batch->length <= kMaxBatchRows) {
			const uint64_t bytesPerRow =
					2 * sizeof(int64_t) + ingestor->schema.children[2].children.size() * sizeof(graph::PropertyValue);
			decodeBudget.ensureElements(static_cast<uint64_t>(record_batch->length), bytesPerRow, -1, "record_batch");
		}
		validateArray(ingestor->schema, record_batch, -1, "record_batch", 0);
		if (record_batch->n_children != 3 || record_batch->children == nullptr) {
			throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT,
								  "edge record batch must contain source_id, target_id, and properties", -1,
								  "record_batch");
		}

		const int64_t rowCount = record_batch->length;
		const auto &sourcePlan = ingestor->schema.children[0];
		const auto &targetPlan = ingestor->schema.children[1];
		const auto &propertiesPlan = ingestor->schema.children[2];
		const auto &sourceArray = *record_batch->children[0];
		const auto &targetArray = *record_batch->children[1];
		const auto &propertiesArray = *record_batch->children[2];

		std::vector<int64_t> sourceIds;
		std::vector<int64_t> targetIds;
		decodeBudget.consumeElements(static_cast<uint64_t>(rowCount), 2 * sizeof(int64_t), -1, "record_batch");
		sourceIds.reserve(static_cast<size_t>(rowCount));
		targetIds.reserve(static_cast<size_t>(rowCount));

		for (int64_t row = 0; row < rowCount; ++row) {
			if (isNull(*record_batch, row, "record_batch")) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "edge record rows must not be null", row,
									  "record_batch");
			}
			if (isNull(sourceArray, row, "source_id")) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "source_id must not be null", row, "source_id");
			}
			if (isNull(targetArray, row, "target_id")) {
				throw ArrowInputError(ZYX_DRIVER_INVALID_ARGUMENT, "target_id must not be null", row, "target_id");
			}
			sourceIds.push_back(readIntegral(sourcePlan.kind, sourceArray, row, "source_id"));
			targetIds.push_back(readIntegral(targetPlan.kind, targetArray, row, "target_id"));
		}

		std::vector<graph::storage::BulkPropertyColumn> columns;
		decodeBudget.consumeElements(propertiesPlan.children.size(), sizeof(graph::storage::BulkPropertyColumn), -1,
									 "properties");
		columns.reserve(propertiesPlan.children.size());
		for (size_t columnIndex = 0; columnIndex < propertiesPlan.children.size(); ++columnIndex) {
			const auto &propertyPlan = propertiesPlan.children[columnIndex];
			const auto &propertyArray = *propertiesArray.children[columnIndex];
			const std::string propertyPath = "properties." + propertyPlan.name;
			graph::storage::BulkPropertyColumn column;
			column.key = propertyPlan.name;
			decodeBudget.consume(propertyPlan.name.size(), -1, propertyPath);
			decodeBudget.ensureElements(static_cast<uint64_t>(rowCount), sizeof(graph::PropertyValue), -1,
										propertyPath);
			column.values.reserve(static_cast<size_t>(rowCount));
			for (int64_t row = 0; row < rowCount; ++row) {
				if (isNull(propertiesArray, row, "properties")) {
					decodeBudget.consume(sizeof(graph::PropertyValue), row, propertyPath);
					column.values.emplace_back(std::monostate{});
				} else {
					column.values.push_back(readValue(propertyPlan, propertyArray, row, propertyPath, 0, decodeBudget));
				}
			}
			columns.push_back(std::move(column));
		}

		auto ids = zyx::detail::DatabaseBulkInternal::createEdgesColumnar(*ingest->owner->db, ingestor->edgeType,
																		  sourceIds, targetIds, columns);
		if (ids.size() != static_cast<size_t>(rowCount)) {
			throw std::runtime_error("bulk edge writer returned an unexpected ID count");
		}
		if (!ids.empty()) {
			const int64_t firstId = ids.front();
			for (size_t index = 0; index < ids.size(); ++index) {
				if (index > static_cast<size_t>((std::numeric_limits<int64_t>::max)()) ||
					firstId > (std::numeric_limits<int64_t>::max)() - static_cast<int64_t>(index)) {
					throw std::runtime_error("bulk edge ID range overflows int64");
				}
				if (ids[index] != firstId + static_cast<int64_t>(index)) {
					throw std::runtime_error("bulk edge IDs are not contiguous");
				}
			}
			if (out_ids != nullptr) {
				out_ids->first_id = firstId;
				out_ids->count = static_cast<int64_t>(ids.size());
			}
		}
		return ZYX_DRIVER_OK;
	} catch (...) {
		return catchIngestException(ingest, out_error);
	}
}

zyx_driver_status_t zyx_driver_ingest_commit(zyx_driver_ingest_t *ingest, zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (auto status = validateActiveIngest(ingest, out_error); status != ZYX_DRIVER_OK)
		return status;
	try {
		ingest->txn.commit();
		ingest->state = IngestState::IS_COMMITTED;
		unregisterIngest(ingest);
		invalidateIngestors(ingest);
		return ZYX_DRIVER_OK;
	} catch (...) {
		return catchIngestException(ingest, out_error);
	}
}

zyx_driver_status_t zyx_driver_ingest_rollback(zyx_driver_ingest_t *ingest, zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (ingest == nullptr) {
		return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "ingest must not be null");
	}
	if (ingest->state == IngestState::IS_COMMITTED || ingest->state == IngestState::IS_ROLLED_BACK) {
		return setError(out_error, ZYX_DRIVER_TRANSACTION_ERROR, "ingest session is already finalized");
	}
	try {
		if (ingest->txn.isActive())
			ingest->txn.rollback();
		ingest->state = IngestState::IS_ROLLED_BACK;
		unregisterIngest(ingest);
		invalidateIngestors(ingest);
		return ZYX_DRIVER_OK;
	} catch (...) {
		return catchTransactionException(out_error);
	}
}

zyx_driver_status_t zyx_driver_edge_ingestor_close(zyx_driver_edge_ingestor_t *ingestor,
												   zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (ingestor == nullptr)
		return ZYX_DRIVER_OK;
	if (ingestor->owner != nullptr) {
		ingestor->owner->ingestors.erase(ingestor);
		ingestor->owner = nullptr;
	}
	delete ingestor;
	return ZYX_DRIVER_OK;
}

zyx_driver_status_t zyx_driver_ingest_close(zyx_driver_ingest_t *ingest, zyx_driver_error_t **out_error) {
	clearError(out_error);
	if (ingest == nullptr)
		return ZYX_DRIVER_OK;

	zyx_driver_status_t status = ZYX_DRIVER_OK;
	try {
		if ((ingest->state == IngestState::IS_ACTIVE || ingest->state == IngestState::IS_FAILED) &&
			ingest->txn.isActive()) {
			ingest->txn.rollback();
			ingest->state = IngestState::IS_ROLLED_BACK;
		}
	} catch (...) {
		status = catchTransactionException(out_error);
	}
	unregisterIngest(ingest);
	invalidateIngestors(ingest);
	delete ingest;
	return status;
}

} // extern "C"
