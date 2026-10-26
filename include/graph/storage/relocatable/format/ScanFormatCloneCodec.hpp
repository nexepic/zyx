#pragma once

#include <algorithm>
#include <array>
#include <span>
#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {
    // Offline-builder byte transcoding only. These functions neither copy files
    // nor authorize publication. The builder must establish stable source input,
    // validate the full graph, and keep both destination roots BUILDING until
    // integrity/whole-file resource admission succeeds.
    class ScanFormatCloneCodec final {
    public:
        [[nodiscard]] static std::array<std::byte, RSS_SUPERBLOCK_BYTES>
        legacySuperblockToBuilding(std::span<const std::byte> source) {
            requireProfile();
            constexpr size_t legacyHeader = 336, reserveBytes = 32;
            require(source.size() == RSS_SUPERBLOCK_BYTES, "scan clone requires a complete legacy Superblock");
            require(LittleEndianCodec::readU32(source, 0) == RSS_SUPERBLOCK_MAGIC &&
                LittleEndianCodec::readU16(source, 4) == 31 && LittleEndianCodec::readU16(source, 6) == legacyHeader,
                "scan clone accepts only default Superblock 31");
            std::array<std::byte, legacyHeader> header{};
            std::copy_n(source.begin(), header.size(), header.begin());
            const auto expectedCrc = LittleEndianCodec::readU32(header, 116);
            LittleEndianCodec::writeU32(header, 116, 0);
            require(crc(header) == expectedCrc && crc(source.subspan(legacyHeader)) == LittleEndianCodec::readU32(source, 112),
                "scan clone legacy Superblock checksum mismatch");
            require((LittleEndianCodec::readU32(source, 8) & ~RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP) == 0 &&
                LittleEndianCodec::readU32(source, 12) == 0 &&
                std::all_of(source.begin() + legacyHeader + reserveBytes, source.end(),
                    [](std::byte byte) { return byte == std::byte{}; }),
                "scan clone legacy Superblock has unknown state or reserved bytes");
            std::array<std::byte, RSS_SUPERBLOCK_BYTES> target{};
            std::copy_n(source.begin(), legacyHeader, target.begin());
            std::copy_n(source.begin() + legacyHeader, reserveBytes, target.begin() + RSS_SUPERBLOCK_HEADER_BYTES);
            LittleEndianCodec::writeU16(target, 4, RSS_SUPERBLOCK_VERSION);
            LittleEndianCodec::writeU16(target, 6, RSS_SUPERBLOCK_HEADER_BYTES);
            LittleEndianCodec::writeU32(target, 8, LittleEndianCodec::readU32(source, 8) | RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
            LittleEndianCodec::writeU32(target, 112, crc(std::span(target).subspan(RSS_SUPERBLOCK_HEADER_BYTES)));
            LittleEndianCodec::writeU32(target, 116, 0);
            LittleEndianCodec::writeU32(target, 116, crc(std::span(target).first(RSS_SUPERBLOCK_HEADER_BYTES)));
            // Current decoding checks all unchanged roots, counters, dimensions,
            // identities and emergency-reserve fields, after source CRC checks.
            static_cast<void>(SuperblockCodec::decode(target));
            return target;
        }

        // Payload framing is unchanged. Each owning page/extent codec must still
        // authenticate its actual payload during whole-target integrity; this
        // header operation cannot certify free bytes or a complete segment.
        [[nodiscard]] static std::array<std::byte, RSS_ENVELOPE_BYTES>
        legacyEnvelopeHeader(std::span<const std::byte> source) {
            requireProfile();
            require(source.size() == RSS_ENVELOPE_BYTES && LittleEndianCodec::readU16(source, 4) == 5,
                "scan clone accepts only default envelope 5");
            std::array<std::byte, RSS_ENVELOPE_BYTES> target{};
            std::copy(source.begin(), source.end(), target.begin());
            const auto expectedCrc = LittleEndianCodec::readU32(target, 52);
            LittleEndianCodec::writeU32(target, 52, 0);
            require(crc(target) == expectedCrc, "scan clone legacy envelope checksum mismatch");
            LittleEndianCodec::writeU16(target, 4, RSS_FORMAT_VERSION);
            LittleEndianCodec::writeU32(target, 52, crc(target));
            const auto decoded = SegmentEnvelopeCodec::decode(target);
            return SegmentEnvelopeCodec::encode(decoded);
        }

        // Authenticate immutable payloads or the mutable pool's A/B control
        // state before changing its header. Pool frames have typed checksums,
        // not an envelope payload CRC; full target integrity must authenticate
        // every reachable frame before BUILDING may ever be cleared.
        static void legacySegmentToBuilding(std::span<std::byte> source) {
            requireProfile();
            require(source.size() == RSS_SEGMENT_SIZE, "scan clone requires a complete source segment");
            const auto header = legacyEnvelopeHeader(source.first(RSS_ENVELOPE_BYTES));
            const auto envelope = SegmentEnvelopeCodec::decode(header);
            if (envelope.kind == SegmentKind::SGK_METADATA_PAGE_POOL) {
                authenticatePoolControl<MetadataPagePoolHeaderCodec>(source, envelope);
            } else if (isSharedPagePoolSegmentKind(envelope.kind)) {
                authenticatePoolControl<SharedPagePoolHeaderCodec>(source, envelope);
            } else {
                const auto payload = source.subspan(envelope.header_bytes, envelope.payload_bytes);
                const std::byte empty{};
                if (utils::calculateCrc(payload.empty() ? &empty : payload.data(), payload.size()) != envelope.payload_crc)
                    throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH,
                        "scan clone legacy segment payload checksum mismatch");
            }
            std::copy(header.begin(), header.end(), source.begin());
        }
    private:
        template<class Codec>
        static void authenticatePoolControl(std::span<const std::byte> source, const SegmentEnvelope &envelope) {
            require(envelope.state == SegmentState::SGS_SEALED && envelope.header_bytes == RSS_PAGE_SIZE &&
                envelope.payload_bytes == RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE,
                "scan clone source pool has invalid framing");
            using Header = decltype(Codec::decode(std::span<const std::byte>{}));
            std::optional<Header> selected;
            static_assert(RSS_METADATA_PAGE_POOL_CONTROL_RECORD_COUNT == RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT);
            static_assert(RSS_METADATA_PAGE_POOL_HEADER_BYTES == RSS_SHARED_PAGE_POOL_HEADER_BYTES);
            for (size_t record = 0; record < RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT; ++record) {
                try {
                    const auto control = Codec::decode(source.subspan(RSS_ENVELOPE_BYTES +
                        record * RSS_SHARED_PAGE_POOL_HEADER_BYTES, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
                    if (!selected || control.control_sequence > selected->control_sequence) selected = control;
                } catch (const FormatException &) {
                    // The complete peer is authoritative after a torn inactive write.
                }
            }
            if (!selected)
                throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH, "scan clone source pool has no valid control record");
            require(selected->segment_id == envelope.segment_id && selected->generation == envelope.generation &&
                selected->committed_lsn == envelope.committed_lsn,
                "scan clone source pool control identity differs from its envelope");
            if constexpr (requires { selected->physical_slot; })
                require(selected->physical_slot.value() == envelope.owner_id,
                    "scan clone source metadata pool slot differs from its envelope owner");
        }
        static void require(bool valid, const char *message) {
            if (!valid) throw FormatException(FormatError::FERR_INVALID_HEADER, message);
        }
        static void requireProfile() {
            if (!RSS_SCAN_PATH_FORMAT_ENABLED)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan clone requires the isolated scan format");
        }
        static uint32_t crc(std::span<const std::byte> bytes) {
            return utils::calculateCrc(bytes.data(), bytes.size());
        }
    };
}
