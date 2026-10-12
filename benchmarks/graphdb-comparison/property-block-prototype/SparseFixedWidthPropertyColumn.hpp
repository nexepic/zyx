#pragma once

#include <bit>
#include <optional>
#include <span>
#include <vector>

#include "TypedPropertyBlock.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"

namespace zyx::benchmark {

// Experimental column codec; enclosing owner and snapshot identity is external.
class SparseFixedWidthPropertyColumn final {
public:
	enum class Kind : uint16_t { SFK_INTEGER = 1U, SFK_DOUBLE = 2U, SFK_BOOLEAN = 3U, SFK_NULL_ONLY = 4U };
	static constexpr size_t HEADER_BYTES = 48U;
	static constexpr uint32_t MAGIC = 0x4353465AU;

	// Layout selection inspects values, not encoded bytes. Unsupported shapes
	// require a canonical fallback; malformed persisted images must still fail.
	static bool canEncode(std::span<const std::optional<PropertyValue>> values) {
		if (values.size() > 64U) return false;
		Kind kind = Kind::SFK_NULL_ONLY;
		for (const auto &value : values) {
			if (!value) continue;
			const auto &variant = value->getVariant();
			if (std::holds_alternative<std::monostate>(variant)) continue;
			Kind current;
			if (std::holds_alternative<int64_t>(variant)) current = Kind::SFK_INTEGER;
			else if (std::holds_alternative<double>(variant)) current = Kind::SFK_DOUBLE;
			else if (std::holds_alternative<bool>(variant)) current = Kind::SFK_BOOLEAN;
			else return false;
			if (kind != Kind::SFK_NULL_ONLY && kind != current) return false;
			kind = current;
		}
		return true;
	}

	static std::vector<std::byte> encode(std::span<const std::optional<PropertyValue>> values) {
		if (values.size() > 64U) throw std::invalid_argument("Sparse column row bound exceeded");
		uint64_t presence = 0U, nulls = 0U;
		Kind kind = Kind::SFK_NULL_ONLY;
		for (size_t row = 0U; row < values.size(); ++row) {
			if (!values[row]) continue;
			presence |= uint64_t{1U} << row;
			const auto &value = values[row]->getVariant();
			if (std::holds_alternative<std::monostate>(value)) { nulls |= uint64_t{1U} << row; continue; }
			Kind current;
			if (std::holds_alternative<int64_t>(value)) current = Kind::SFK_INTEGER;
			else if (std::holds_alternative<double>(value)) current = Kind::SFK_DOUBLE;
			else if (std::holds_alternative<bool>(value)) current = Kind::SFK_BOOLEAN;
			else throw std::invalid_argument("Sparse fixed column requires primitive values");
			if (kind != Kind::SFK_NULL_ONLY && kind != current)
				throw std::invalid_argument("Sparse fixed column requires homogeneous values");
			kind = current;
		}
		const auto valid = presence & ~nulls;
		const size_t payloadBytes = static_cast<size_t>(std::popcount(valid)) * stride(kind);
		std::vector<std::byte> bytes(HEADER_BYTES + payloadBytes);
		LittleEndianCodec::writeU32(bytes, 0U, MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, 1U);
		LittleEndianCodec::writeU16(bytes, 6U, static_cast<uint16_t>(kind));
		LittleEndianCodec::writeU32(bytes, 8U, static_cast<uint32_t>(values.size()));
		LittleEndianCodec::writeU32(bytes, 12U, static_cast<uint32_t>(payloadBytes));
		LittleEndianCodec::writeU64(bytes, 16U, presence);
		LittleEndianCodec::writeU64(bytes, 24U, nulls);
		size_t offset = HEADER_BYTES;
		for (size_t row = 0U; row < values.size(); ++row) {
			if ((valid & (uint64_t{1U} << row)) == 0U) continue;
			const auto &value = values[row]->getVariant();
			if (kind == Kind::SFK_BOOLEAN) bytes[offset] = std::byte(std::get<bool>(value) ? 1U : 0U);
			else LittleEndianCodec::writeU64(bytes, offset, kind == Kind::SFK_INTEGER
				? std::bit_cast<uint64_t>(std::get<int64_t>(value)) : std::bit_cast<uint64_t>(std::get<double>(value)));
			offset += stride(kind);
		}
		LittleEndianCodec::writeU32(bytes, 40U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		return bytes;
	}

	static std::optional<PropertyValue> read(std::span<const std::byte> bytes, size_t row) {
		validate(bytes);
		return readValidated(bytes, row);
	}

	static std::vector<std::optional<PropertyValue>> readAll(std::span<const std::byte> bytes,
	                                                       const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		validate(bytes);
		const size_t rows = LittleEndianCodec::readU32(bytes, 8U);
		std::vector<std::optional<PropertyValue>> values;
		values.reserve(rows);
		for (size_t row = 0U; row < rows; ++row) {
			if (checkpoint) checkpoint->check();
			values.push_back(readValidated(bytes, row));
		}
		if (checkpoint) checkpoint->check();
		return values;
	}

	// Build an immutable replacement of this bounded column only. An absent
	// optional removes the field; a present NULL retains its presence bit.
	static std::vector<std::byte> replace(std::span<const std::byte> before, size_t row,
		const std::optional<PropertyValue> &value) {
		validate(before);
		const size_t rows = LittleEndianCodec::readU32(before, 8U);
		if (row >= rows) throw std::out_of_range("Sparse column row is out of range");
		std::vector<std::optional<PropertyValue>> values;
		values.reserve(rows);
		for (size_t current = 0U; current < rows; ++current)
			values.push_back(current == row ? value : readValidated(before, current));
		return encode(values);
	}

private:
	// Callers validate the complete immutable image before selecting rows.
	static std::optional<PropertyValue> readValidated(std::span<const std::byte> bytes, size_t row) {
		const size_t rows = LittleEndianCodec::readU32(bytes, 8U);
		if (row >= rows) throw std::out_of_range("Sparse column row is out of range");
		const auto presence = LittleEndianCodec::readU64(bytes, 16U);
		const auto nulls = LittleEndianCodec::readU64(bytes, 24U);
		const auto bit = uint64_t{1U} << row;
		if ((presence & bit) == 0U) return std::nullopt;
		if ((nulls & bit) != 0U) return PropertyValue{};
		const auto kind = static_cast<Kind>(LittleEndianCodec::readU16(bytes, 6U));
		const auto rank = static_cast<size_t>(std::popcount((presence & ~nulls) & (bit - 1U)));
		const auto offset = HEADER_BYTES + rank * stride(kind);
		if (kind == Kind::SFK_BOOLEAN) return PropertyValue(std::to_integer<uint8_t>(bytes[offset]) != 0U);
		const auto bits = LittleEndianCodec::readU64(bytes, offset);
		return kind == Kind::SFK_INTEGER ? PropertyValue(std::bit_cast<int64_t>(bits))
			: PropertyValue(std::bit_cast<double>(bits));
	}

public:
	static void validate(std::span<const std::byte> bytes) {
		if (bytes.size() < HEADER_BYTES || LittleEndianCodec::readU32(bytes, 0U) != MAGIC ||
			LittleEndianCodec::readU16(bytes, 4U) != 1U || LittleEndianCodec::readU64(bytes, 32U) != 0U ||
			LittleEndianCodec::readU32(bytes, 44U) != 0U)
			throw std::runtime_error("Invalid sparse column header");
		const auto rawKind = LittleEndianCodec::readU16(bytes, 6U);
		const auto rows = LittleEndianCodec::readU32(bytes, 8U);
		if (rawKind < 1U || rawKind > 4U || rows > 64U) throw std::runtime_error("Invalid sparse column shape");
		const auto kind = static_cast<Kind>(rawKind);
		const auto presence = LittleEndianCodec::readU64(bytes, 16U);
		const auto nulls = LittleEndianCodec::readU64(bytes, 24U);
		const auto mask = rows == 64U ? UINT64_MAX : (uint64_t{1U} << rows) - 1U;
		const auto valid = presence & ~nulls;
		if ((presence & ~mask) != 0U || (nulls & ~presence) != 0U ||
			((kind == Kind::SFK_NULL_ONLY) != (valid == 0U)))
			throw std::runtime_error("Invalid sparse column presence");
		const size_t payloadBytes = static_cast<size_t>(std::popcount(valid)) * stride(kind);
		if (LittleEndianCodec::readU32(bytes, 12U) != payloadBytes || bytes.size() != HEADER_BYTES + payloadBytes)
			throw std::runtime_error("Invalid sparse column payload length");
		auto checked = std::vector<std::byte>(bytes.begin(), bytes.end());
		LittleEndianCodec::writeU32(checked, 40U, 0U);
		if (graph::utils::calculateCrc(checked.data(), checked.size()) != LittleEndianCodec::readU32(bytes, 40U))
			throw std::runtime_error("Invalid sparse column checksum");
		if (kind == Kind::SFK_BOOLEAN)
			for (size_t offset = HEADER_BYTES; offset < bytes.size(); ++offset)
				if (std::to_integer<uint8_t>(bytes[offset]) > 1U) throw std::runtime_error("Invalid sparse boolean value");
	}

private:
	static size_t stride(Kind kind) { return kind == Kind::SFK_NULL_ONLY ? 0U : kind == Kind::SFK_BOOLEAN ? 1U : 8U; }
};

} // namespace zyx::benchmark
