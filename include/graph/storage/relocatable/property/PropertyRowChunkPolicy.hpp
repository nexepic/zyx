#pragma once

#include <algorithm>
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"

namespace graph::storage {

// A ROW-only placement policy. The canonical property bytes, sole owner route,
// ref identity and explicit-offset Manifest codec are unchanged. Small adjacent
// entries share chunks; only large entry payloads introduce additional cuts.
class PropertyRowChunkPolicy final {
public:
	[[nodiscard]] static LargeValueCompressionOptions select(std::span<const std::byte> payload,
		const LargeValueCompressionOptions &base, bool enabled) {
		if (!enabled || base.codec != LargeValueCodec::LVC_RAW || !base.logical_chunk_ends.empty() ||
			payload.size() < RSS_LARGE_VALUE_EXTENT_32K_BYTES ||
			payload.size() < RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES)
			return base;
		const auto count = LittleEndianCodec::readU32(payload, 8U);
		if (count == 0U || count > 2048U) return base;
		const auto locations = EntityPropertyLargeValuePayloadCodec::decodeDirectory(payload);
		LargeValueCompressionOptions candidate = base;
		for (const auto &entry : locations) {
			if (entry.length < RSS_LARGE_VALUE_EXTENT_16K_BYTES) continue;
			const uint64_t start = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + static_cast<uint64_t>(entry.offset);
			const uint64_t prefixEnd = start + sizeof(uint32_t) + CanonicalPropertyValueCodec::HEADER_BYTES;
			const uint64_t end = start + entry.length;
			if (end > payload.size() || prefixEnd >= end) throw FormatException(FormatError::FERR_INVALID_HEADER, "ROW chunk policy encountered an invalid entry envelope");
			// Keep token/value headers with the preceding small group. Presence
			// probes can validate the type envelope without loading the large body.
			if (prefixEnd < payload.size()) candidate.logical_chunk_ends.push_back(prefixEnd);
			if (end < payload.size()) candidate.logical_chunk_ends.push_back(end);
		}
		if (candidate.logical_chunk_ends.empty()) return base;
		std::ranges::sort(candidate.logical_chunk_ends);
		candidate.logical_chunk_ends.erase(std::unique(candidate.logical_chunk_ends.begin(), candidate.logical_chunk_ends.end()), candidate.logical_chunk_ends.end());
		const auto old = rawCost(payload.size(), base.logical_chunk_ends);
		const auto next = rawCost(payload.size(), candidate.logical_chunk_ends);
		// General page-cost admission, never a label/workload/size-name rule.
		// Include data/Manifest frames and their directory/catalog entry bytes.
		// A new first-use chunk must be smaller; added encoded allocation is <=5%.
		const auto costBytes = [](const Cost &cost) {
			const uint64_t pages = cost.pages + manifestPages(cost.chunks);
			return pages * (RSS_PAGE_SIZE + RSS_LOCATION_ENTRY_BYTES + RSS_CATALOG_ENTRY_BYTES);
		};
		const uint64_t oldBytes = costBytes(old), newBytes = costBytes(next);
		const uint64_t firstOld = std::min<uint64_t>(payload.size(), RSS_LARGE_VALUE_EXTENT_32K_BYTES);
		const uint64_t firstNew = std::min<uint64_t>(candidate.logical_chunk_ends.front(), RSS_LARGE_VALUE_EXTENT_32K_BYTES);
		if (newBytes > oldBytes && newBytes - oldBytes > oldBytes / 20U) return base;
		if (dataPages(firstNew) >= dataPages(firstOld)) return base;
		return candidate;
	}
private:
	struct Cost { uint64_t pages = 0U, chunks = 0U; };
	[[nodiscard]] static uint64_t dataPages(uint64_t bytes) {
		return bytes / RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + static_cast<uint64_t>(bytes % RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES != 0U);
	}
	[[nodiscard]] static Cost rawCost(uint64_t bytes, std::span<const uint64_t> ends) {
		Cost result; uint64_t begin = 0U;
		for (size_t segment = 0U; segment <= ends.size(); ++segment) {
			const uint64_t end = segment == ends.size() ? bytes : ends[segment];
			const uint64_t length = end - begin, full = length / RSS_LARGE_VALUE_EXTENT_32K_BYTES;
			const uint64_t rest = length % RSS_LARGE_VALUE_EXTENT_32K_BYTES;
			result.pages += full * dataPages(RSS_LARGE_VALUE_EXTENT_32K_BYTES) + dataPages(rest);
			result.chunks += full + static_cast<uint64_t>(rest != 0U); begin = end;
		}
		return result;
	}
	[[nodiscard]] static uint64_t manifestPages(uint64_t chunks) {
		if (chunks <= RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) return 1U;
		uint64_t level = chunks / RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS + static_cast<uint64_t>(chunks % RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS != 0U);
		uint64_t result = level;
		while (level > RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN) {
			level = level / RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN + static_cast<uint64_t>(level % RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN != 0U); result += level;
		}
		return result + 1U;
	}
};
}
