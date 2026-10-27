#include "graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include <algorithm>
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::storage {
namespace {
using hybrid_property_detail::require;
uint32_t ownerType(EntityPropertyKey owner) {
    return owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
}
void validateSelected(std::span<const std::byte> bytes) {
    size_t offset = 0;
    static_cast<void>(CanonicalPropertyValueCodec::inspectNext(bytes, offset));
    require(offset == bytes.size(), "cold ROW selected value has trailing bytes");
}
}
HybridPropertyColdRowInspection::HybridPropertyColdRowInspection(const MetadataSnapshotPin &snapshot, HybridColdRowInput row,
    EntityPropertyKey owner, Generation blockGeneration, std::span<const uint32_t> hotTokens,
    const std::shared_ptr<std::pmr::memory_resource> &memory, const LargeValueStore *values,
    const CancellationCheckpoint *cancellation)
    : snapshot_(snapshot), generation_(snapshot.superblock().generation), database_(snapshot.superblock().database_id),
      row_(row), owner_(owner), values_(values), cancellation_(cancellation),
      // The inline token directory borrows the body; only the remaining
      // seven standard containers need their separate debug-STL proxies.
      proxy_credit_(memory, 7U * utils::detail::standardContainerProxyBytes()),
      reference_credit_(memory), manifest_credit_(memory), range_credit_(memory), header_credit_(memory), directory_credit_(memory) {
    require(memory && owner.isValid() && blockGeneration.isValid() &&
        blockGeneration <= snapshot.superblock().generation && row.owner_slot == hybridPropertySlotFor(owner) &&
        row.field_count && !row.root.empty() && row.root.size() <= HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES, "cold ROW inspection requires its owner, pin and account");
    check();
    uint32_t previous = 0;
    for (const auto token : hotTokens) {
        check(); require(token && token > previous, "cold ROW hot tokens are not canonical"); previous = token;
    }
    if (!row.large_value_reference) {
        const auto identity = EntityPropertyCollectionRootCodec::decodeOwner(row.root);
        require(identity.owner_entity_id == owner.entity_id && identity.owner_entity_type == ownerType(owner),
            "cold ROW inspection has a foreign inline owner");
        inline_tokens_ = EntityPropertyCollectionRootCodec::tokenDirectoryView(row.root, cancellation);
    } else {
        require(values != nullptr, "cold ROW LargeValue requires its pinned immutable store");
        // The existing owner-keyed reference decoder may allocate its bounded
        // media string. Charge the complete encoded width before decoding it.
        reference_credit_.set(row.root.size());
        reference_ = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        const auto &value = reference_->value;
        require(reference_->owner_entity_id == owner.entity_id && reference_->owner_entity_type == ownerType(owner) &&
            value.kind == LargeValueKind::LVK_BYTES && value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
            value.generation <= blockGeneration && value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
            "cold ROW LargeValue has a foreign owner, media or future generation");
        const auto admit = [&](size_t bytes) { manifest_credit_.set(bytes); };
        const auto checkpoint = [&] { check(); };
        manifest_ = values->verifyContent(snapshot, value, std::ref(admit), std::ref(checkpoint));
        header_ = readRange(0, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
        header_credit_.set(header_.capacity()); range_credit_.set(range_.retainedBytes());
        const auto fields = LittleEndianCodec::readU32(header_, 8);
        require(fields == row.field_count, "cold ROW LargeValue count disagrees with its block");
        const uint64_t directoryBytes = uint64_t(fields) * 8U;
        const uint64_t headerBytes = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + directoryBytes;
        require(headerBytes <= manifest_.logical_bytes, "cold ROW LargeValue directory exceeds its manifest");
        auto completeHeader = readRange(0, headerBytes);
        header_credit_.set(header_.capacity() + completeHeader.capacity());
        header_ = std::move(completeHeader);
        header_credit_.set(header_.capacity()); range_credit_.set(range_.retainedBytes());
        require(uint64_t(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES) + LittleEndianCodec::readU32(header_, 12) ==
            manifest_.logical_bytes, "cold ROW LargeValue length disagrees with its canonical header");
        const SelectedPropertyReadAdmission admitDirectory = [&](size_t bytes) { directory_credit_.set(bytes); };
        locations_ = EntityPropertyLargeValuePayloadCodec::decodeDirectory(header_, &admitDirectory, cancellation);
    }
    require(count() == row.field_count, "cold ROW directory count disagrees with its block");
    for (size_t i = 0; i < count(); ++i) {
        check();
        require(!std::binary_search(hotTokens.begin(), hotTokens.end(), token(i)),
            "cold ROW contains a group-owned hot token");
    }
    check();
}
uint32_t HybridPropertyColdRowInspection::token(size_t ordinal) const {
    require(ordinal < count(), "cold ROW token ordinal is outside its directory");
    return row_.large_value_reference ? locations_[ordinal].key_token : inline_tokens_->token(ordinal);
}
std::vector<std::byte> HybridPropertyColdRowInspection::readRange(uint64_t offset, uint64_t size) {
    check();
    const auto admit = [&](size_t bytes) { range_credit_.set(bytes); };
    return values_->readRangeFromManifestCached(snapshot_, manifest_, offset, size, range_, std::ref(admit));
}
void HybridPropertyColdRowInspection::visitSelected(std::span<const uint32_t> tokens,
    const EntityPropertyCollectionRootCodec::EncodedSelectionSink &sink, const SelectedPropertyReadAdmission *admission) {
    require(!tokens.empty() && bool(sink), "cold ROW selection requires tokens and a synchronous sink");
    uint32_t previous = 0;
    for (const auto token : tokens) { check(); require(token && token > previous, "cold ROW selected tokens are not canonical"); previous = token; }
    if (!row_.large_value_reference) {
        for (size_t ordinal = 0; ordinal < tokens.size(); ++ordinal) {
            check();
            const auto value = inline_tokens_->find(tokens[ordinal]);
            if (value) {
                if (admission) (*admission)(sizeof(uint32_t) + value->size());
                validateSelected(*value);
            }
            check(); sink(ordinal, value);
        }
    } else for (size_t ordinal = 0; ordinal < tokens.size(); ++ordinal) {
        check();
        const auto found = std::lower_bound(locations_.begin(), locations_.end(), tokens[ordinal],
            [](const auto &entry, uint32_t token) { return entry.key_token < token; });
        if (found == locations_.end() || found->key_token != tokens[ordinal]) { sink(ordinal, std::nullopt); continue; }
        require(found->length >= sizeof(uint32_t), "cold ROW LargeValue entry is truncated");
        if (admission) (*admission)(found->length);
        {
            const auto bytes = readRange(uint64_t(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES) + found->offset, found->length);
            require(LittleEndianCodec::readU32(bytes, 0) == tokens[ordinal], "cold ROW LargeValue entry has a foreign token");
            const auto value = std::span<const std::byte>(bytes).subspan(sizeof(uint32_t));
            validateSelected(value); check(); sink(ordinal, value);
        }
        range_credit_.set(range_.retainedBytes());
    }
    check();
}
}
