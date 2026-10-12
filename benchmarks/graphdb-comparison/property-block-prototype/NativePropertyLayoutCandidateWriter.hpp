#pragma once

#include "NativePropertyLayoutCandidateReader.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace zyx::benchmark {

// Lab-only atomic container publication. No graph route or retirement is added;
// whole-container replacement is not a production local-update strategy.
class NativePropertyLayoutCandidateWriter final {
public:
	static LargeValueRef write(LargeValueStore &native, RelocatableMetadataStore &metadata,
	    PropertyCollectionRootStore &properties, const MetadataSnapshotPin &snapshot,
	    const PropertyBlockOwnerSlots &slots, const PropertyLayoutCandidate::Image &image,
	    const LargeValueWriteOptions &options, LogSequenceNumber lsn, size_t maximumFields, size_t maximumBytes,
	    const MetadataPublishOptions &publishOptions = {}, const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (!metadata.ownsSnapshot(snapshot) || !native.usesMetadataStore(metadata) ||
		    !properties.usesMetadataStore(metadata) || properties.largeValueStore() != &native ||
		    snapshot.superblock().generation.value() == UINT64_MAX ||
		    image.identity.database_id != snapshot.superblock().database_id.value() ||
		    image.identity.generation != snapshot.superblock().generation.value() + 1U)
			throw std::invalid_argument("Native candidate publication requires one domain and next snapshot identity");
		const auto bytes = PropertyLayoutCandidateCodec::encode(image, slots, maximumFields, maximumBytes, checkpoint);
		const auto id = snapshot.superblock().next_large_value_id;
		if (id == 0U || id == UINT64_MAX) throw std::overflow_error("Native candidate value identity overflow");
		const auto reference = native.planReference(LargeValueId(id), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
		    NativePropertyLayoutCandidateReader::MEDIA, options.compression, options.extent_class);
		if (checkpoint) checkpoint->check();
		const std::vector<NativeGraphLargeValueMutation> mutations{{reference, bytes, options.first_data_slot,
		    options.last_data_slot, options.first_manifest_slot, options.last_manifest_slot, options.compression,
		    options.extent_class, native.plannedDataPageCount(bytes, options.compression, options.extent_class),
		    native.plannedManifestPageCount(bytes, options.compression, options.extent_class)}};
		if (checkpoint) checkpoint->check();
		auto content = native.planNativeGraphContentIndexMutation(snapshot, mutations, options.first_manifest_slot, options.last_manifest_slot);
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(content.mutation);
		// Publication owns acknowledgment semantics from this point onward.
		if (checkpoint) checkpoint->check();
		static_cast<void>(metadata.mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, mutations, derived, lsn,
		    [&](auto, auto, auto, auto, auto, std::span<LargeValueMaterializationResult> materializations) {
			    if (materializations.size() != 1U) throw std::runtime_error("Native candidate materialization count mismatch");
			    properties.preparePlannedLargeValue(mutations.front(), materializations.front(), lsn);
		    }, [&](auto &batches) {
			    if (batches.size() != 1U) throw std::runtime_error("Native candidate content batch mismatch");
			    content.preparer(batches.front(), lsn);
		    }, {}, {}, {}, {}, {}, publishOptions, {}, snapshot.superblock().generation));
		return reference;
	}
};

} // namespace zyx::benchmark
