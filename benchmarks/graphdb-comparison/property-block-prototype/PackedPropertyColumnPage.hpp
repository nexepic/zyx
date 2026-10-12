#pragma once

#include "AdaptivePropertyColumn.hpp"
#include "CompactFixedPropertyColumn.hpp"

namespace zyx::benchmark {

// Laboratory shared-page carrier; returned spans borrow the caller's page.
class PackedPropertyColumnPage final {
public:
	enum class Kind : uint32_t { PCP_ADAPTIVE = 1U, PCP_COMPACT_FIXED = 2U };
	struct Column { uint32_t token; std::span<const std::byte> image; Kind kind = Kind::PCP_ADAPTIVE; };
	static constexpr size_t HEADER = 24U, ENTRY = 16U;
	static constexpr uint32_t MAGIC = 0x5043505AU;
	static std::optional<std::vector<std::optional<PropertyValue>>> readColumn(
	    std::span<const std::byte> page, uint32_t token, size_t maximumBytes,
	    const CancellationCheckpoint *checkpoint = nullptr) {
		if (token == 0U) throw std::invalid_argument("Packed read requires positive token");
		const auto columns = decode(page, maximumBytes, checkpoint);
		const auto position = std::lower_bound(columns.begin(), columns.end(), token,
		    [](const Column &column, uint32_t key) { return column.token < key; });
		if (position == columns.end() || position->token != token) return std::nullopt;
		if (position->kind == Kind::PCP_COMPACT_FIXED) return CompactFixedPropertyColumn::readAll(position->image, checkpoint);
		return AdaptivePropertyColumn::decodeImage(position->image, maximumBytes, checkpoint).readAll(checkpoint);
	}
	static std::vector<std::vector<std::byte>> replace(std::span<const std::byte> page, uint32_t token,
	    std::optional<std::span<const std::byte>> replacement, size_t pageBytes, size_t maximumPages,
	    const CancellationCheckpoint *checkpoint = nullptr, Kind replacementKind = Kind::PCP_ADAPTIVE) {
		if (token == 0U) throw std::invalid_argument("Packed replacement requires positive token");
		auto columns = decode(page, pageBytes, checkpoint);
		const auto position = std::lower_bound(columns.begin(), columns.end(), token,
		    [](const Column &column, uint32_t key) { return column.token < key; });
		if (position == columns.end() || position->token != token)
			throw std::invalid_argument("Packed replacement token is not present");
		if (replacement) { position->image = *replacement; position->kind = replacementKind; }
		else columns.erase(position);
		return pack(columns, pageBytes, maximumPages, checkpoint);
	}
	// Deterministic ordered packing; oversized columns require external fallback.
	static std::vector<std::vector<std::byte>> pack(std::span<const Column> columns, size_t pageBytes,
	                                               size_t maximumPages, const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (pageBytes < HEADER + ENTRY || pageBytes > UINT32_MAX || maximumPages == 0U || columns.size() > 4096U)
			throw std::invalid_argument("Invalid packed page limits");
		uint32_t previous = 0U;
		for (const auto &column : columns) {
			if (checkpoint) checkpoint->check();
			if (column.token <= previous || column.image.empty()) throw std::invalid_argument("Invalid packed column order or image");
			if (column.image.size() > pageBytes - HEADER - ENTRY)
				throw std::length_error("Packed column requires oversized-value fallback");
			previous = column.token;
		}
		const auto nextPageEnd = [&](size_t begin) {
			if (checkpoint) checkpoint->check();
			size_t end = begin, used = HEADER;
			while (end < columns.size() && columns[end].image.size() + ENTRY <= pageBytes - used) {
				if (checkpoint) checkpoint->check();
				used += ENTRY + columns[end].image.size(); ++end;
			}
			return end;
		};
		// Prove the complete page-count bound before allocating output pages.
		size_t requiredPages = 0U;
		for (size_t begin = 0U; begin < columns.size(); begin = nextPageEnd(begin)) {
			if (requiredPages == maximumPages) throw std::length_error("Packed column page count bound exceeded");
			++requiredPages;
		}
		if (checkpoint) checkpoint->check();
		std::vector<std::vector<std::byte>> pages;
		pages.reserve(requiredPages);
		for (size_t begin = 0U; begin < columns.size();) {
			const size_t end = nextPageEnd(begin);
			pages.push_back(encode(columns.subspan(begin, end - begin), pageBytes, checkpoint));
			begin = end;
		}
		if (checkpoint) checkpoint->check();
		return pages;
	}
	static std::vector<std::byte> encode(std::span<const Column> columns, size_t maximumBytes,
	                                     const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (columns.size() > 4096U || maximumBytes < HEADER + columns.size() * ENTRY)
			throw std::length_error("Packed column page directory bound exceeded");
		size_t size = HEADER + columns.size() * ENTRY;
		uint32_t previous = 0U;
		for (const auto &column : columns) {
			if (checkpoint) checkpoint->check();
			if (column.token <= previous || column.image.empty()) throw std::invalid_argument("Invalid packed column order or image");
			if (column.image.size() > maximumBytes - size) throw std::length_error("Packed column page byte bound exceeded");
			if (column.kind == Kind::PCP_ADAPTIVE)
				static_cast<void>(AdaptivePropertyColumn::decodeImage(column.image, maximumBytes, checkpoint));
			else if (column.kind == Kind::PCP_COMPACT_FIXED)
				static_cast<void>(CompactFixedPropertyColumn::readAll(column.image, checkpoint));
			else throw std::invalid_argument("Unknown packed column kind");
			size += column.image.size(); previous = column.token;
		}
		if (size > UINT32_MAX) throw std::length_error("Packed column page exceeds wire size");
		std::vector<std::byte> page(size);
		LittleEndianCodec::writeU32(page, 0U, MAGIC);
		LittleEndianCodec::writeU32(page, 4U, 2U);
		LittleEndianCodec::writeU32(page, 8U, static_cast<uint32_t>(columns.size()));
		LittleEndianCodec::writeU32(page, 12U, static_cast<uint32_t>(size));
		size_t offset = HEADER + columns.size() * ENTRY;
		for (size_t index = 0U; index < columns.size(); ++index) {
			if (checkpoint) checkpoint->check();
			const auto &column = columns[index]; const auto entry = HEADER + index * ENTRY;
			LittleEndianCodec::writeU32(page, entry, column.token);
			LittleEndianCodec::writeU32(page, entry + 4U, static_cast<uint32_t>(offset));
			LittleEndianCodec::writeU32(page, entry + 8U, static_cast<uint32_t>(column.image.size()));
			LittleEndianCodec::writeU32(page, entry + 12U, static_cast<uint32_t>(column.kind));
			for (size_t copied = 0U; copied < column.image.size();) {
				if (checkpoint) checkpoint->check();
				const size_t length = std::min<size_t>(65536U, column.image.size() - copied);
				std::copy_n(column.image.begin() + copied, length, page.begin() + offset + copied);
				copied += length;
			}
			offset += column.image.size();
		}
		LittleEndianCodec::writeU32(page, 16U, checksum(page, checkpoint));
		if (checkpoint) checkpoint->check();
		return page;
	}
	static std::vector<Column> decode(std::span<const std::byte> page, size_t maximumBytes,
	                                  const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (page.size() < HEADER || page.size() > maximumBytes || LittleEndianCodec::readU32(page, 0U) != MAGIC ||
		    (LittleEndianCodec::readU32(page, 4U) != 1U && LittleEndianCodec::readU32(page, 4U) != 2U) || LittleEndianCodec::readU32(page, 12U) != page.size() ||
		    LittleEndianCodec::readU32(page, 20U) != 0U) throw std::runtime_error("Invalid packed column page header");
		const size_t count = LittleEndianCodec::readU32(page, 8U);
		const bool legacy = LittleEndianCodec::readU32(page, 4U) == 1U;
		const size_t entryBytes = legacy ? 12U : ENTRY;
		if (count > 4096U || count > (page.size() - HEADER) / entryBytes) throw std::runtime_error("Invalid packed column directory size");
		const auto crc = checksum(page, checkpoint);
		if (crc != LittleEndianCodec::readU32(page, 16U)) throw std::runtime_error("Packed column page checksum mismatch");
		size_t offset = HEADER + count * entryBytes; uint32_t previous = 0U;
		for (size_t index = 0U; index < count; ++index) {
			if (checkpoint) checkpoint->check();
			const auto entry = HEADER + index * entryBytes;
			if (!legacy && (LittleEndianCodec::readU32(page, entry + 12U) < 1U || LittleEndianCodec::readU32(page, entry + 12U) > 2U))
				throw std::runtime_error("Unknown packed column kind");
			const auto token = LittleEndianCodec::readU32(page, entry);
			const auto length = LittleEndianCodec::readU32(page, entry + 8U);
			if (token <= previous || LittleEndianCodec::readU32(page, entry + 4U) != offset || length == 0U || length > page.size() - offset)
				throw std::runtime_error("Invalid packed column range or token");
			offset += length; previous = token;
		}
		if (offset != page.size()) throw std::runtime_error("Trailing packed column page bytes");
		std::vector<Column> columns; columns.reserve(count);
		for (size_t index = 0U; index < count; ++index) {
			if (checkpoint) checkpoint->check();
			const auto entry = HEADER + index * entryBytes;
			columns.push_back({LittleEndianCodec::readU32(page, entry), page.subspan(
			    LittleEndianCodec::readU32(page, entry + 4U), LittleEndianCodec::readU32(page, entry + 8U)),
			    legacy ? Kind::PCP_ADAPTIVE : static_cast<Kind>(LittleEndianCodec::readU32(page, entry + 12U))});
		}
		if (checkpoint) checkpoint->check();
		return columns;
	}
private:
	static uint32_t checksum(std::span<const std::byte> page, const CancellationCheckpoint *checkpoint) {
		const std::array<std::byte, 4U> zero{};
		auto crc = graph::utils::calculateCrc(page.data(), 16U);
		crc = graph::utils::updateCrc(crc, zero.data(), zero.size());
		for (size_t offset = 20U; offset < page.size();) {
			if (checkpoint) checkpoint->check();
			const size_t length = std::min<size_t>(65536U, page.size() - offset);
			crc = graph::utils::updateCrc(crc, page.data() + offset, length);
			offset += length;
		}
		return crc;
	}
};

} // namespace zyx::benchmark
