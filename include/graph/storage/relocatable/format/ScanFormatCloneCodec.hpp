#pragma once

#include <algorithm>
#include <array>
#include <span>
#include "graph/storage/relocatable/format/Superblock.hpp"
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
    private:
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
