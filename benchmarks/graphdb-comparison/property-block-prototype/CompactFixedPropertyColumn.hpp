#pragma once

#include "SparseFixedWidthPropertyColumn.hpp"

namespace zyx::benchmark {

// Laboratory subblock codec. The enclosing format must authenticate bytes,
// version, identity and lifetime; this subblock has no independent checksum.
class CompactFixedPropertyColumn final {
public:
	static constexpr size_t HEADER = 24U;
	class VerifiedColumn final {
	public:
		[[nodiscard]] size_t encodedBytes() const { return bytes_->size(); }
		std::optional<PropertyValue> read(size_t row) const {
			if (row >= LittleEndianCodec::readU16(*bytes_, 2U)) throw std::out_of_range("Compact column row exceeds bounds");
			const auto presence = LittleEndianCodec::readU64(*bytes_, 8U);
			const auto nulls = LittleEndianCodec::readU64(*bytes_, 16U);
			const uint64_t bit = uint64_t{1U} << row;
			if ((presence & bit) == 0U) return std::nullopt;
			if ((nulls & bit) != 0U) return PropertyValue{};
			const auto kind = LittleEndianCodec::readU16(*bytes_, 0U);
			const size_t rank = static_cast<size_t>(std::popcount(presence & ~nulls & (bit - 1U)));
			const size_t offset = HEADER + rank * (kind == 3U ? 1U : sizeof(uint64_t));
			if (kind == 3U) return PropertyValue(std::to_integer<uint8_t>(bytes_->at(offset)) != 0U);
			const auto bits = LittleEndianCodec::readU64(*bytes_, offset);
			if (kind == 1U) return PropertyValue(std::bit_cast<int64_t>(bits));
			return PropertyValue(std::bit_cast<double>(bits));
		}
	private:
		friend class CompactFixedPropertyColumn;
		explicit VerifiedColumn(std::vector<std::byte> bytes)
		    : bytes_(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}
		std::shared_ptr<const std::vector<std::byte>> bytes_;
	};
	static VerifiedColumn freeze(std::span<const std::byte> compact, const CancellationCheckpoint *checkpoint = nullptr) {
		// Copy before validation; no mutable alias to verified storage escapes.
		if (compact.size() > HEADER + 64U * sizeof(uint64_t)) throw std::runtime_error("Compact column exceeds bounds");
		if (checkpoint) checkpoint->check();
		std::vector<std::byte> owned(compact.begin(), compact.end());
		static_cast<void>(readAll(owned, checkpoint));
		return VerifiedColumn(std::move(owned));
	}
	static std::vector<std::byte> encode(std::span<const std::optional<PropertyValue>> values) {
		const auto full = SparseFixedWidthPropertyColumn::encode(values);
		std::vector<std::byte> compact(HEADER + full.size() - SparseFixedWidthPropertyColumn::HEADER_BYTES);
		LittleEndianCodec::writeU16(compact, 0U, LittleEndianCodec::readU16(full, 6U));
		LittleEndianCodec::writeU16(compact, 2U, static_cast<uint16_t>(values.size()));
		LittleEndianCodec::writeU64(compact, 8U, LittleEndianCodec::readU64(full, 16U));
		LittleEndianCodec::writeU64(compact, 16U, LittleEndianCodec::readU64(full, 24U));
		std::copy(full.begin() + SparseFixedWidthPropertyColumn::HEADER_BYTES, full.end(), compact.begin() + HEADER);
		return compact;
	}

	static std::vector<std::optional<PropertyValue>> readAll(std::span<const std::byte> compact,
	                                                       const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (compact.size() < HEADER || compact.size() > HEADER + 64U * sizeof(uint64_t) ||
		    LittleEndianCodec::readU32(compact, 4U) != 0U)
			throw std::runtime_error("Invalid compact fixed column bounds or reserved bytes");
		// Reconstruct the standalone validator's bounded image, retaining its
		// complete type, mask, payload and boolean validation contract.
		std::vector<std::byte> full(SparseFixedWidthPropertyColumn::HEADER_BYTES + compact.size() - HEADER);
		LittleEndianCodec::writeU32(full, 0U, SparseFixedWidthPropertyColumn::MAGIC);
		LittleEndianCodec::writeU16(full, 4U, 1U);
		LittleEndianCodec::writeU16(full, 6U, LittleEndianCodec::readU16(compact, 0U));
		LittleEndianCodec::writeU32(full, 8U, LittleEndianCodec::readU16(compact, 2U));
		LittleEndianCodec::writeU32(full, 12U, static_cast<uint32_t>(compact.size() - HEADER));
		LittleEndianCodec::writeU64(full, 16U, LittleEndianCodec::readU64(compact, 8U));
		LittleEndianCodec::writeU64(full, 24U, LittleEndianCodec::readU64(compact, 16U));
		std::copy(compact.begin() + HEADER, compact.end(), full.begin() + SparseFixedWidthPropertyColumn::HEADER_BYTES);
		LittleEndianCodec::writeU32(full, 40U, graph::utils::calculateCrc(full.data(), full.size()));
		return SparseFixedWidthPropertyColumn::readAll(full, checkpoint);
	}
};

} // namespace zyx::benchmark
