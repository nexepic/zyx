#pragma once

#include "graph/storage/relocatable/entity/ScanBlockMembership.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"

namespace graph::storage {
    // Wire-compatible fixed map, but not the property group's smaller ID
    // domain. Membership must also cover nodes having no property owner key.
    using ScanMembershipDirectoryMap = FixedBTreeMap<8, 16>;
    class ScanMembershipDirectoryCodec final {
    public:
        static constexpr uint64_t MAX_GROUP = 1U + (UINT64_MAX - 1U) / 64U;
        [[nodiscard]] static ScanMembershipDirectoryMap::Key key(uint64_t group) {
            if (!group || group > MAX_GROUP)
                throw FormatException(FormatError::FERR_INVALID_HEADER, "scan directory group is outside the Node ID domain");
            ScanMembershipDirectoryMap::Key bytes{};
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = std::byte((group >> ((7U - i) * 8U)) & 0xffU);
            return bytes;
        }
        [[nodiscard]] static uint64_t decodeKey(const ScanMembershipDirectoryMap::Key &bytes) {
            uint64_t group = 0;
            for (const auto byte : bytes) group = (group << 8U) | std::to_integer<uint8_t>(byte);
            static_cast<void>(key(group));
            return group;
        }
        [[nodiscard]] static BTreeRootReadBinding bind(const HybridPropertyDescriptor &descriptor,
            const MetadataSnapshotPin &snapshot) {
            if (!RSS_SCAN_PATH_FORMAT_ENABLED)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan directory requires its isolated format");
            return HybridPropertyDirectoryCodec::bindRoot(descriptor, snapshot, HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
        }
    };
}
