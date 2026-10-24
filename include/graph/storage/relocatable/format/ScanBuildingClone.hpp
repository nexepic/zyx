#pragma once

#include <algorithm>
#include <functional>
#include <memory_resource>
#include <vector>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/ScanFormatCloneCodec.hpp"
#include "graph/storage/relocatable/format/SuperblockPair.hpp"

namespace graph::storage::scan_candidate_detail {
    // Private construction primitive, not a public converter or a completed
    // database. Caller owns a fresh exclusive destination and fresh uncached
    // read-only source IO. Failure leaves only an invalid/BUILDING private file.
    // No publication capability is returned: full graph integrity, membership
    // construction, total-space admission and final A/B publication still follow.
    [[nodiscard]] inline Superblock cloneUnverifiedBuildingImage(StorageIO &source, StorageIO &target,
        query::execution::MaterializedByteBudget &workspace, uint64_t maximumTotalFileBytes,
        const std::function<void()> &checkpoint) {
        if (!RSS_SCAN_PATH_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan clone requires the isolated format");
        if (!checkpoint || &source == &target || target.fileSize() != 0 ||
            !maximumTotalFileBytes || maximumTotalFileBytes > UINT64_C(2) * 1024U * 1024U * 1024U ||
            workspace.used() > 1024U * 1024U || workspace.available() > 1024U * 1024U - workspace.used())
            throw std::invalid_argument("scan clone requires fresh files and bounded workspace/file contracts");
        checkpoint();
        const auto sourceBytes = source.fileSize();
        if (sourceBytes < RSS_FILE_BOOTSTRAP_BYTES || sourceBytes > maximumTotalFileBytes / 2U)
            throw StorageFileLimitExceeded();
        // Codec/recovery stack frames are bounded independently of the PMR image
        // buffers. Reserve their conservative simultaneous high-water allowance.
        query::execution::MaterializedByteReservation stackBytes(&workspace);
        stackBytes.set(16U * 1024U);
        std::pmr::vector<SuperblockPair::Slots> frames(&workspace);
        frames.resize(3);
        auto &before = frames[0]; auto &converted = frames[1]; auto &after = frames[2];
        const auto readHeaders = [&](SuperblockPair::Slots &slots) {
            for (size_t i = 0; i < slots.size(); ++i)
                if (source.readAt(i * RSS_SUPERBLOCK_BYTES, slots[i].data(), slots[i].size()) != slots[i].size())
                    throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "scan clone source headers are truncated");
        };
        readHeaders(before);
        for (size_t i = 0; i < before.size(); ++i) {
            try { converted[i] = ScanFormatCloneCodec::legacySuperblockToBuilding(before[i]); }
            catch (const FormatException &) { converted[i].fill(std::byte{}); }
        }
        auto pair = std::allocate_shared<SuperblockPair>(std::pmr::polymorphic_allocator<SuperblockPair>(&workspace), converted);
        const auto selected = pair->recover(); // existing A/B conflict and identity rules
        if (selected.data_region_offset > sourceBytes || (sourceBytes - selected.data_region_offset) % RSS_SEGMENT_SIZE)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone source is not a complete segment image");
        std::pmr::vector<std::byte> buffer(&workspace);
        buffer.resize(RSS_SEGMENT_SIZE); // admit every image allocation before the first destination write
        checkpoint();
        for (size_t i = 0; i < converted.size(); ++i)
            target.writeAt(i * RSS_SUPERBLOCK_BYTES, converted[i].data(), converted[i].size());
        target.sync(); // a source-format header is never installed at the destination
        const auto copyRange = [&](uint64_t begin, uint64_t end, bool segmentHeaders) {
            for (uint64_t offset = begin; offset < end;) {
                checkpoint();
                const auto bytes = static_cast<size_t>(std::min<uint64_t>(buffer.size(), end - offset));
                if (source.readAt(offset, buffer.data(), bytes) != bytes)
                    throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "scan clone source was truncated during copy");
                if (segmentHeaders && LittleEndianCodec::readU32(buffer, 0) == RSS_SEGMENT_MAGIC) {
                    const auto header = ScanFormatCloneCodec::legacyEnvelopeHeader(std::span(buffer).first(RSS_ENVELOPE_BYTES));
                    std::copy(header.begin(), header.end(), buffer.begin());
                }
                target.writeAt(offset, buffer.data(), bytes);
                offset += bytes;
            }
        };
        copyRange(2U * RSS_SUPERBLOCK_BYTES, selected.data_region_offset, false);
        copyRange(selected.data_region_offset, sourceBytes, true);
        checkpoint(); readHeaders(after);
        if (before != after || source.fileSize() != sourceBytes || target.fileSize() != sourceBytes)
            throw FormatException(FormatError::FERR_INVALID_STATE, "scan clone source identity changed during copy");
        target.sync();
        return selected; // still BUILDING, unverified and unavailable through public open
    }
}
