#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {
    // Candidate B's bounded membership record. This byte codec grants no read
    // authority: the caller must authenticate its containing page and snapshot
    // root before using it to replace primary Entity record validation.
    // 64 IDs reuse the existing physical group size, within the 512-row contract.
    struct ScanBlockMembership {
        static constexpr uint32_t MAGIC = 0x4d53595aU; // ZYSM
        static constexpr uint16_t VERSION = 1;
        static constexpr size_t HEADER_BYTES = 64;
        static constexpr size_t MAX_LABELS = 64;
        static constexpr size_t MAX_BYTES = HEADER_BYTES + MAX_LABELS * 16;
        struct Label {
            uint64_t token = 0;
            uint64_t members = 0;
            constexpr bool operator==(const Label &) const = default;
        };
        DatabaseId database_id{};
        Generation semantic_generation{};
        uint64_t group_id = 0;
        uint64_t live = 0;
        uint16_t label_count = 0;
        std::array<Label, MAX_LABELS> labels{};

        void validate() const {
            require(database_id.isValid() && semantic_generation.isValid(), "scan membership has no identity");
            require(group_id && group_id <= 1U + (UINT64_MAX - 1U) / 64U && live,
                "scan membership has an invalid ID range");
            // The final possible group contains only IDs through UINT64_MAX.
            if (group_id == 1U + (UINT64_MAX - 1U) / 64U)
                require((live & (UINT64_C(1) << 63U)) == 0, "scan membership contains an overflowing ID");
            require(label_count <= MAX_LABELS, "scan membership label limit exceeded");
            uint64_t previous = 0;
            for (size_t i = 0; i < label_count; ++i) {
                const auto &label = labels[i];
                require(label.token > previous && label.members && !(label.members & ~live),
                    "scan membership has unordered labels or nonlive members");
                previous = label.token;
            }
        }

        [[nodiscard]] uint64_t select(std::span<const uint64_t> requiredTokens) const {
            validate();
            uint64_t result = live, previous = 0;
            for (const auto token : requiredTokens) {
                require(token > previous, "scan label predicate must be sorted and unique");
                previous = token;
                const auto end = labels.begin() + label_count;
                const auto found = std::lower_bound(labels.begin(), end, token,
                    [](const Label &label, uint64_t value) { return label.token < value; });
                result &= found == end || found->token != token ? 0 : found->members;
            }
            return result;
        }

        [[nodiscard]] size_t wireSize() const { validate(); return HEADER_BYTES + label_count * 16U; }
        void encodeInto(std::span<std::byte> bytes) const {
            require(bytes.size() == wireSize(), "scan membership encoding has the wrong capacity");
            std::fill(bytes.begin(), bytes.end(), std::byte{});
            LittleEndianCodec::writeU32(bytes, 0, MAGIC);
            LittleEndianCodec::writeU16(bytes, 4, VERSION);
            LittleEndianCodec::writeU16(bytes, 6, HEADER_BYTES);
            LittleEndianCodec::writeU64(bytes, 8, database_id.value());
            LittleEndianCodec::writeU64(bytes, 16, semantic_generation.value());
            LittleEndianCodec::writeU64(bytes, 24, group_id);
            LittleEndianCodec::writeU64(bytes, 32, live);
            LittleEndianCodec::writeU16(bytes, 40, label_count);
            LittleEndianCodec::writeU32(bytes, 44, static_cast<uint32_t>(bytes.size()));
            for (size_t i = 0; i < label_count; ++i) {
                LittleEndianCodec::writeU64(bytes, HEADER_BYTES + 16U * i, labels[i].token);
                LittleEndianCodec::writeU64(bytes, HEADER_BYTES + 16U * i + 8U, labels[i].members);
            }
            LittleEndianCodec::writeU32(bytes, 48, utils::calculateCrc(bytes.data() + HEADER_BYTES, bytes.size() - HEADER_BYTES));
            LittleEndianCodec::writeU32(bytes, 52, utils::calculateCrc(bytes.data(), HEADER_BYTES));
        }
        [[nodiscard]] std::vector<std::byte> encode() const {
            std::vector<std::byte> bytes(wireSize()); encodeInto(bytes); return bytes;
        }

        [[nodiscard]] static ScanBlockMembership decode(std::span<const std::byte> bytes,
            DatabaseId database, Generation visibleGeneration, uint64_t expectedGroup) {
            require(bytes.size() >= HEADER_BYTES && bytes.size() <= MAX_BYTES, "scan membership width is invalid");
            std::array<std::byte, HEADER_BYTES> header{};
            std::copy_n(bytes.begin(), HEADER_BYTES, header.begin());
            const auto headerCrc = LittleEndianCodec::readU32(header, 52);
            LittleEndianCodec::writeU32(header, 52, 0);
            require(utils::calculateCrc(header.data(), header.size()) == headerCrc,
                "scan membership header checksum mismatch");
            require(LittleEndianCodec::readU32(bytes, 0) == MAGIC && LittleEndianCodec::readU16(bytes, 4) == VERSION &&
                LittleEndianCodec::readU16(bytes, 6) == HEADER_BYTES, "scan membership format is unsupported");
            require(LittleEndianCodec::readU16(bytes, 42) == 0 && LittleEndianCodec::readU64(bytes, 56) == 0,
                "scan membership reserved bits are nonzero");
            ScanBlockMembership result;
            result.database_id = DatabaseId(LittleEndianCodec::readU64(bytes, 8));
            result.semantic_generation = Generation(LittleEndianCodec::readU64(bytes, 16));
            result.group_id = LittleEndianCodec::readU64(bytes, 24);
            result.live = LittleEndianCodec::readU64(bytes, 32);
            result.label_count = LittleEndianCodec::readU16(bytes, 40);
            require(result.label_count <= MAX_LABELS && bytes.size() == HEADER_BYTES + result.label_count * 16U &&
                LittleEndianCodec::readU32(bytes, 44) == bytes.size(), "scan membership length disagrees with its labels");
            require(utils::calculateCrc(bytes.data() + HEADER_BYTES, bytes.size() - HEADER_BYTES) ==
                LittleEndianCodec::readU32(bytes, 48), "scan membership body checksum mismatch");
            require(database.isValid() && visibleGeneration.isValid() && result.database_id == database &&
                result.semantic_generation <= visibleGeneration && result.group_id == expectedGroup,
                "scan membership belongs to another database, group or future snapshot");
            for (size_t i = 0; i < result.label_count; ++i)
                result.labels[i] = {LittleEndianCodec::readU64(bytes, HEADER_BYTES + 16U * i),
                    LittleEndianCodec::readU64(bytes, HEADER_BYTES + 16U * i + 8U)};
            result.validate();
            return result;
        }
    private:
        static void require(bool condition, const char *message) {
            if (!condition) throw FormatException(FormatError::FERR_INVALID_HEADER, message);
        }
    };
}
