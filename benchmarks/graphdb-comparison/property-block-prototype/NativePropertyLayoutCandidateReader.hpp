#pragma once

#include "PropertyLayoutCandidateCodec.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace zyx::benchmark {

// Lab-only carrier. The caller must resolve reference, slots and birth version
// from the authoritative route of the supplied native snapshot.
class NativePropertyLayoutCandidateReader final {
public:
	static constexpr const char *MEDIA = "application/vnd.zyx.lab.property-layout-candidate";
	static PropertyLayoutCandidate::VerifiedImage read(LargeValueStore &store, const MetadataSnapshotPin &snapshot,
	    const LargeValueRef &reference, const PropertyBlockOwnerSlots &slots, uint64_t birthGeneration,
	    size_t maximumFields, size_t maximumBytes, const SelectedPropertyReadAdmission &admission,
	    const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (!admission || maximumBytes == 0U || birthGeneration == 0U ||
		    birthGeneration > snapshot.superblock().generation.value() || reference.logical_bytes > maximumBytes ||
		    reference.kind != LargeValueKind::LVK_BYTES || reference.media_type != MEDIA)
			throw std::invalid_argument("Native candidate read requires bounded reference, version and admission");
		const SelectedPropertyReadAdmission guarded = [&](size_t bytes) {
			if (checkpoint) checkpoint->check();
			admission(bytes);
			if (checkpoint) checkpoint->check();
		};
		const auto bytes = store.read(snapshot, reference, guarded);
		const PropertyBlockFieldMembership::SnapshotIdentity identity{
		    snapshot.superblock().database_id.value(), birthGeneration};
		auto image = PropertyLayoutCandidateCodec::decode(bytes, slots, identity, maximumFields, maximumBytes, checkpoint);
		return PropertyLayoutCandidate::freeze(std::move(image), slots, identity, maximumFields, maximumBytes, checkpoint);
	}
};

} // namespace zyx::benchmark
