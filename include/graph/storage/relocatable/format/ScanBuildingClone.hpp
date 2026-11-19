#pragma once

#include <algorithm>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/format/SuperblockPair.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage::scan_candidate_detail {
    template<class Codec>
    inline void authenticateCurrentPoolControl(std::span<const std::byte> segment, const SegmentEnvelope &envelope) {
        if (envelope.state != SegmentState::SGS_SEALED || envelope.header_bytes != RSS_PAGE_SIZE ||
            envelope.payload_bytes != RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone source pool has invalid framing");
        using Header = decltype(Codec::decode(std::span<const std::byte>{}));
        std::optional<Header> selected;
        static_assert(RSS_METADATA_PAGE_POOL_CONTROL_RECORD_COUNT == RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT);
        static_assert(RSS_METADATA_PAGE_POOL_HEADER_BYTES == RSS_SHARED_PAGE_POOL_HEADER_BYTES);
        for (size_t record = 0; record < RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT; ++record) {
            try {
                const auto control = Codec::decode(segment.subspan(RSS_ENVELOPE_BYTES +
                    record * RSS_SHARED_PAGE_POOL_HEADER_BYTES, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
                if (!selected || control.control_sequence > selected->control_sequence) selected = control;
            } catch (const FormatException &) {
                // A valid peer remains authoritative after a torn control write.
            }
        }
        if (!selected)
            throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH, "scan clone source pool has no valid control record");
        if (selected->segment_id != envelope.segment_id || selected->generation != envelope.generation ||
            selected->committed_lsn != envelope.committed_lsn)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone source pool control identity differs from its envelope");
        if constexpr (requires { selected->physical_slot; })
            if (selected->physical_slot.value() != envelope.owner_id)
                throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone source metadata pool slot differs from its envelope owner");
    }

    inline void authenticateCurrentSegment(std::span<const std::byte> segment) {
        if (segment.size() != RSS_SEGMENT_SIZE)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone requires a complete source segment");
        const auto envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
        if (envelope.kind == SegmentKind::SGK_METADATA_PAGE_POOL)
            authenticateCurrentPoolControl<MetadataPagePoolHeaderCodec>(segment, envelope);
        else if (isSharedPagePoolSegmentKind(envelope.kind))
            authenticateCurrentPoolControl<SharedPagePoolHeaderCodec>(segment, envelope);
        else {
            const auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
            const std::byte empty{};
            if (utils::calculateCrc(payload.empty() ? &empty : payload.data(), payload.size()) != envelope.payload_crc)
                throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH, "scan clone source segment payload checksum mismatch");
        }
    }
    // Private construction primitive, not a public converter or a completed
    // database. Caller owns a fresh exclusive destination and fresh uncached
    // read-only source IO. Failure leaves only an invalid/BUILDING private file.
    // No publication capability is returned: full graph integrity, membership
    // construction, total-space admission and final A/B publication still follow.
    [[nodiscard]] inline Superblock cloneUnverifiedBuildingImage(StorageIO &source, StorageIO &target,
        query::execution::MaterializedByteBudget &workspace, uint64_t maximumTotalFileBytes,
        const std::function<void()> &checkpoint) {
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
        std::pmr::vector<SuperblockPair::Slots> frames(size_t{0U}, &workspace);
        frames.resize(3);
        auto &before = frames[0]; auto &converted = frames[1]; auto &after = frames[2];
        const auto readHeaders = [&](SuperblockPair::Slots &slots) {
            for (size_t i = 0; i < slots.size(); ++i)
                if (source.readAt(i * RSS_SUPERBLOCK_BYTES, slots[i].data(), slots[i].size()) != slots[i].size())
                    throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "scan clone source headers are truncated");
        };
        readHeaders(before);
        // The source must already use this format. Recovery rejects a
        // checksum-valid unknown format while retaining same-format torn-slot
        // recovery; cloning never invents logical revision values.
        const auto sourceRoot = [&] {
            auto sourcePair = std::allocate_shared<SuperblockPair>(std::pmr::polymorphic_allocator<SuperblockPair>(&workspace), before);
            return sourcePair->recover();
        }();
        if ((sourceRoot.flags & (RSS_SUPERBLOCK_FLAG_SCAN_BUILDING | RSS_SUPERBLOCK_FLAG_SCAN_COMPLETE)) ||
            sourceRoot.hybrid_property_descriptor_root.isValid())
            throw FormatException(FormatError::FERR_INVALID_STATE, "scan clone requires a ROW source image");
        for (size_t i = 0; i < before.size(); ++i) {
            try {
                auto root = SuperblockCodec::decode(before[i]);
                root.flags |= RSS_SUPERBLOCK_FLAG_SCAN_BUILDING;
                converted[i] = SuperblockCodec::encode(root);
            }
            catch (const FormatException &) { converted[i].fill(std::byte{}); }
        }
        auto pair = std::allocate_shared<SuperblockPair>(std::pmr::polymorphic_allocator<SuperblockPair>(&workspace), converted);
        const auto selected = pair->recover(); // existing A/B conflict and identity rules
        if (selected.data_region_offset > sourceBytes || (sourceBytes - selected.data_region_offset) % RSS_SEGMENT_SIZE)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "scan clone source is not a complete segment image");
        std::pmr::vector<std::byte> buffer(size_t{0U}, &workspace);
        buffer.resize(RSS_SEGMENT_SIZE); // admit every image allocation before the first destination write
        checkpoint();
        for (size_t i = 0; i < converted.size(); ++i)
            target.writeAt(i * RSS_SUPERBLOCK_BYTES, converted[i].data(), converted[i].size());
        target.sync(); // only BUILDING headers are installed at the destination
        const auto copyRange = [&](uint64_t begin, uint64_t end, bool segmentHeaders) {
            for (uint64_t offset = begin; offset < end;) {
                checkpoint();
                const auto bytes = static_cast<size_t>(std::min<uint64_t>(buffer.size(), end - offset));
                if (source.readAt(offset, buffer.data(), bytes) != bytes)
                    throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "scan clone source was truncated during copy");
                if (segmentHeaders && LittleEndianCodec::readU32(buffer, 0) == RSS_SEGMENT_MAGIC)
                    authenticateCurrentSegment(std::span(buffer).first(bytes));
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
