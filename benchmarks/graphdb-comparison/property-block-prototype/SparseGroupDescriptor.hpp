#pragma once

#include "PropertyBlockFieldMembership.hpp"

namespace zyx::benchmark {

// Experimental descriptor only. Its native root must be authenticated and
// selected by an authoritative snapshot route before following child references.
class SparseGroupDescriptor final {
public:
	using Identity = PropertyBlockFieldMembership::SnapshotIdentity;
	struct Column { uint32_t token; graph::storage::LargeValueRef reference; };
	struct Decoded {
		PropertyBlockOwnerSlots slots;
		PropertyBlockFieldMembership directory;
		std::vector<Column> columns;
	};
	static constexpr uint32_t MAGIC = 0x4753505AU;
	static constexpr size_t HEADER = 24U;
	static constexpr const char *COLUMN_MEDIA = "application/vnd.zyx.lab.sparse-property-column";

	static std::vector<std::byte> encode(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockFieldMembership &directory, std::span<const Column> columns,
		Identity identity, size_t maximumFields, size_t maximumBytes) {
		const auto members = directory.encodeAdaptive(slots, identity, maximumBytes);
		static_cast<void>(PropertyBlockFieldMembership::decodeAdaptive(members, slots, identity, maximumFields, maximumBytes));
		if (maximumBytes < HEADER + PropertyBlockOwnerSlots::WIRE_BYTES || members.size() > UINT32_MAX ||
			members.size() > maximumBytes - HEADER - PropertyBlockOwnerSlots::WIRE_BYTES || columns.size() > maximumFields || columns.size() > UINT32_MAX)
			throw std::length_error("Sparse descriptor bound exceeded");
		std::vector<std::byte> bytes(HEADER);
		graph::storage::LittleEndianCodec::writeU32(bytes, 0U, MAGIC);
		graph::storage::LittleEndianCodec::writeU16(bytes, 4U, 2U);
		graph::storage::LittleEndianCodec::writeU32(bytes, 8U, static_cast<uint32_t>(members.size()));
		graph::storage::LittleEndianCodec::writeU32(bytes, 12U, static_cast<uint32_t>(columns.size()));
		graph::storage::LittleEndianCodec::writeU32(bytes, 16U, PropertyBlockOwnerSlots::WIRE_BYTES);
		const auto ownerBytes = slots.encode();
		bytes.insert(bytes.end(), ownerBytes.begin(), ownerBytes.end());
		bytes.insert(bytes.end(), members.begin(), members.end());
		uint32_t previous = 0U;
		for (const auto &column : columns) {
			validateColumn(column, previous); previous = column.token;
			const auto wire = graph::storage::LargeValueRefCodec::encode(column.reference);
			if (maximumBytes - bytes.size() < 8U || wire.size() > maximumBytes - bytes.size() - 8U)
				throw std::length_error("Sparse descriptor reference bound exceeded");
			const size_t offset = bytes.size(); bytes.resize(offset + 8U);
			graph::storage::LittleEndianCodec::writeU32(bytes, offset, column.token);
			graph::storage::LittleEndianCodec::writeU32(bytes, offset + 4U, static_cast<uint32_t>(wire.size()));
			bytes.insert(bytes.end(), wire.begin(), wire.end());
		}
		validateCoverage(slots, directory, columns, identity);
		graph::storage::LittleEndianCodec::writeU32(bytes, 20U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		return bytes;
	}

	static Decoded decode(std::span<const std::byte> bytes, uint64_t block, uint32_t type,
		Identity identity, size_t maximumFields, size_t maximumBytes) {
		using graph::storage::LittleEndianCodec;
		if (bytes.size() < HEADER + PropertyBlockOwnerSlots::WIRE_BYTES || bytes.size() > maximumBytes || maximumFields == 0U ||
			LittleEndianCodec::readU32(bytes, 0U) != MAGIC || (LittleEndianCodec::readU16(bytes, 4U) != 1U && LittleEndianCodec::readU16(bytes, 4U) != 2U) ||
			LittleEndianCodec::readU16(bytes, 6U) != 0U || LittleEndianCodec::readU32(bytes, 16U) != PropertyBlockOwnerSlots::WIRE_BYTES)
			throw std::runtime_error("Invalid sparse descriptor header");
		auto checked = std::vector<std::byte>(bytes.begin(), bytes.end());
		LittleEndianCodec::writeU32(checked, 20U, 0U);
		if (graph::utils::calculateCrc(checked.data(), checked.size()) != LittleEndianCodec::readU32(bytes, 20U))
			throw std::runtime_error("Invalid sparse descriptor checksum");
		const size_t directoryBytes = LittleEndianCodec::readU32(bytes, 8U), count = LittleEndianCodec::readU32(bytes, 12U);
		size_t cursor = HEADER + PropertyBlockOwnerSlots::WIRE_BYTES;
		if (directoryBytes > bytes.size() - cursor || count > maximumFields)
			throw std::runtime_error("Invalid sparse descriptor bounds");
		auto slots = PropertyBlockOwnerSlots::decode(bytes.subspan(HEADER, PropertyBlockOwnerSlots::WIRE_BYTES), block, type);
		const auto directoryWire = bytes.subspan(cursor, directoryBytes);
		if (LittleEndianCodec::readU16(bytes, 4U) == 1U &&
			(directoryWire.size() < 4U || LittleEndianCodec::readU32(directoryWire, 0U) != PropertyBlockFieldMembership::WIRE_MAGIC))
			throw std::runtime_error("Legacy sparse descriptor requires its original field directory");
		auto directory = PropertyBlockFieldMembership::decodeAdaptive(directoryWire, slots, identity, maximumFields, maximumBytes);
		cursor += directoryBytes;
		Decoded result{std::move(slots), std::move(directory), {}};
		if (count > (bytes.size() - cursor) / 8U) throw std::runtime_error("Truncated sparse descriptor references");
		result.columns.reserve(count);
		uint32_t previous = 0U;
		for (size_t index = 0U; index < count; ++index) {
			if (bytes.size() - cursor < 8U) throw std::runtime_error("Truncated sparse descriptor reference");
			const auto token = LittleEndianCodec::readU32(bytes, cursor);
			const size_t size = LittleEndianCodec::readU32(bytes, cursor + 4U); cursor += 8U;
			if (size > bytes.size() - cursor || size > graph::storage::RSS_LARGE_VALUE_REF_HEADER_BYTES + graph::storage::RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES)
				throw std::runtime_error("Invalid sparse descriptor reference length");
			Column column{token, graph::storage::LargeValueRefCodec::decode(bytes.subspan(cursor, size))}; cursor += size;
			validateColumn(column, previous); previous = token;
			result.columns.push_back(std::move(column));
		}
		if (cursor != bytes.size()) throw std::runtime_error("Sparse descriptor trailing bytes");
		validateCoverage(result.slots, result.directory, result.columns, identity);
		return result;
	}
private:
	static void validateCoverage(const PropertyBlockOwnerSlots &slots, const PropertyBlockFieldMembership &directory,
		std::span<const Column> columns, Identity identity) {
		const auto owners = slots.activeOwnersSorted();
		for (const auto owner : owners) {
			for (const auto token : directory.fields(slots, slots.route(owner), identity)) {
				const auto found = std::lower_bound(columns.begin(), columns.end(), token,
					[](const auto &column, uint32_t key) { return column.token < key; });
				if (found == columns.end() || found->token != token)
					throw std::runtime_error("Sparse descriptor omitted a field reference");
			}
		}
		for (const auto &column : columns) {
			bool present = false;
			for (const auto owner : owners) {
				const auto fields = directory.fields(slots, slots.route(owner), identity);
				present = present || std::binary_search(fields.begin(), fields.end(), column.token);
			}
			if (!present) throw std::runtime_error("Sparse descriptor contains an unused field reference");
		}
	}
	static void validateColumn(const Column &column, uint32_t previous) {
		if (column.token == 0U || column.token <= previous || column.reference.kind != graph::storage::LargeValueKind::LVK_BYTES ||
			column.reference.media_type != COLUMN_MEDIA || column.reference.logical_bytes < 48U || column.reference.logical_bytes > 560U)
			throw std::runtime_error("Invalid sparse descriptor column");
	}
};

} // namespace zyx::benchmark
