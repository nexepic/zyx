#pragma once

#include "PropertyLayoutCandidate.hpp"

namespace zyx::benchmark {

// Experimental container, separate from every production file format.
class PropertyLayoutCandidateCodec final {
public:
	static constexpr size_t HEADER = PropertyLayoutCandidate::CONTAINER_HEADER_BYTES;
	static constexpr uint32_t MAGIC = 0x474C505AU;
	static std::vector<std::byte> encode(const PropertyLayoutCandidate::Image &image,
	    const PropertyBlockOwnerSlots &slots, size_t maximumFields, size_t maximumBytes,
	    const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		const auto owners = slots.activeOwnersSorted();
		if (owners.empty()) throw std::invalid_argument("Candidate container requires owners");
		static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, owners.front(), image.identity, maximumFields, maximumBytes, checkpoint));
		std::vector<std::pair<uint32_t, std::span<const std::byte>>> sections;
		const bool packed = image.layout == PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS;
		if (packed) { sections.push_back({1U, image.slots}); sections.push_back({2U, image.membership}); }
		for (const auto &bytes : packed ? image.pages : image.owners) sections.push_back({packed ? 4U : 3U, bytes});
		size_t size = HEADER;
		if (maximumBytes < HEADER || sections.size() > 4096U) throw std::length_error("Candidate container limits exceeded");
		for (const auto &[kind, bytes] : sections) {
			if (checkpoint) checkpoint->check();
			static_cast<void>(kind);
			if (maximumBytes - size < 8U || bytes.size() > maximumBytes - size - 8U)
				throw std::length_error("Candidate container byte bound exceeded");
			size += 8U + bytes.size();
		}
		if (size > UINT32_MAX) throw std::length_error("Candidate container exceeds wire range");
		std::vector<std::byte> bytes(size);
		LittleEndianCodec::writeU32(bytes, 0U, MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, 1U);
		LittleEndianCodec::writeU16(bytes, 6U, packed ? 2U : 1U);
		LittleEndianCodec::writeU64(bytes, 8U, image.identity.database_id);
		LittleEndianCodec::writeU64(bytes, 16U, image.identity.generation);
		LittleEndianCodec::writeU64(bytes, 24U, slots.blockId());
		LittleEndianCodec::writeU32(bytes, 32U, slots.ownerType());
		LittleEndianCodec::writeU32(bytes, 36U, static_cast<uint32_t>(sections.size()));
		LittleEndianCodec::writeU32(bytes, 40U, static_cast<uint32_t>(size));
		size_t offset = HEADER;
		for (const auto &[kind, payload] : sections) {
			if (checkpoint) checkpoint->check();
			LittleEndianCodec::writeU32(bytes, offset, kind);
			LittleEndianCodec::writeU32(bytes, offset + 4U, static_cast<uint32_t>(payload.size()));
			offset += 8U;
			for (size_t copied = 0U; copied < payload.size();) {
				if (checkpoint) checkpoint->check();
				const size_t length = std::min<size_t>(65536U, payload.size() - copied);
				std::copy_n(payload.begin() + copied, length, bytes.begin() + offset + copied); copied += length;
			}
			offset += payload.size();
		}
		LittleEndianCodec::writeU32(bytes, 44U, checksum(bytes, checkpoint));
		if (checkpoint) checkpoint->check();
		return bytes;
	}
	static PropertyLayoutCandidate::Image decode(std::span<const std::byte> bytes,
	    const PropertyBlockOwnerSlots &slots, PropertyBlockFieldMembership::SnapshotIdentity identity,
	    size_t maximumFields, size_t maximumBytes, const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (bytes.size() < HEADER || bytes.size() > maximumBytes || LittleEndianCodec::readU32(bytes, 0U) != MAGIC ||
		    LittleEndianCodec::readU16(bytes, 4U) != 1U || LittleEndianCodec::readU32(bytes, 40U) != bytes.size() ||
		    LittleEndianCodec::readU64(bytes, 8U) != identity.database_id || LittleEndianCodec::readU64(bytes, 16U) != identity.generation ||
		    LittleEndianCodec::readU64(bytes, 24U) != slots.blockId() || LittleEndianCodec::readU32(bytes, 32U) != slots.ownerType())
			throw std::runtime_error("Candidate container header or identity mismatch");
		const auto layout = LittleEndianCodec::readU16(bytes, 6U);
		const size_t count = LittleEndianCodec::readU32(bytes, 36U);
		if ((layout != 1U && layout != 2U) || count == 0U || count > 4096U ||
		    (layout == 1U && count > 64U) || (layout == 2U && count < 2U) || checksum(bytes, checkpoint) != LittleEndianCodec::readU32(bytes, 44U))
			throw std::runtime_error("Invalid candidate container shape or checksum");
		// Validate every section range before constructing owned payloads.
		size_t offset = HEADER;
		for (size_t index = 0U; index < count; ++index) {
			if (checkpoint) checkpoint->check();
			if (bytes.size() - offset < 8U) throw std::runtime_error("Truncated candidate section");
			const auto kind = LittleEndianCodec::readU32(bytes, offset);
			const auto length = LittleEndianCodec::readU32(bytes, offset + 4U);
			const uint32_t expected = layout == 1U ? 3U : index == 0U ? 1U : index == 1U ? 2U : 4U;
			offset += 8U;
			if (kind != expected || length == 0U || length > bytes.size() - offset) throw std::runtime_error("Invalid candidate section kind or range");
			offset += length;
		}
		if (offset != bytes.size()) throw std::runtime_error("Trailing candidate container bytes");
		PropertyLayoutCandidate::Image image;
		image.identity = identity;
		image.layout = layout == 1U ? PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS : PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS;
		if (layout == 2U) image.bytes = HEADER + count * PropertyLayoutCandidate::CONTAINER_SECTION_BYTES;
		offset = HEADER;
		for (size_t index = 0U; index < count; ++index) {
			if (checkpoint) checkpoint->check();
			const auto kind = LittleEndianCodec::readU32(bytes, offset), length = LittleEndianCodec::readU32(bytes, offset + 4U);
			offset += 8U;
			std::vector<std::byte> payload(length);
			for (size_t copied = 0U; copied < length;) {
				if (checkpoint) checkpoint->check();
				const size_t chunk = std::min<size_t>(65536U, length - copied);
				std::copy_n(bytes.begin() + offset + copied, chunk, payload.begin() + copied); copied += chunk;
			}
			image.bytes += length;
			if (kind == 1U) image.slots = std::move(payload);
			else if (kind == 2U) image.membership = std::move(payload);
			else if (kind == 3U) image.owners.push_back(std::move(payload));
			else image.pages.push_back(std::move(payload));
			offset += length;
		}
		const auto owners = slots.activeOwnersSorted();
		if (owners.empty()) throw std::runtime_error("Candidate container has no owners");
		static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, owners.front(), identity, maximumFields, maximumBytes, checkpoint));
		if (checkpoint) checkpoint->check();
		return image;
	}
private:
	static uint32_t checksum(std::span<const std::byte> bytes, const CancellationCheckpoint *checkpoint) {
		const std::array<std::byte, 4U> zero{};
		auto crc = graph::utils::calculateCrc(bytes.data(), 44U);
		crc = graph::utils::updateCrc(crc, zero.data(), zero.size());
		for (size_t offset = HEADER; offset < bytes.size();) {
			if (checkpoint) checkpoint->check();
			const size_t length = std::min<size_t>(65536U, bytes.size() - offset);
			crc = graph::utils::updateCrc(crc, bytes.data() + offset, length); offset += length;
		}
		return crc;
	}
};

} // namespace zyx::benchmark
