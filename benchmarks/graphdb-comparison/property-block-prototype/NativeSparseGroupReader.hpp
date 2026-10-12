#pragma once

#include "SparseGroupDescriptor.hpp"
#include "SparsePropertyGroup.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace zyx::benchmark {

// Lab-only native reader. The caller must select the descriptor reference and
// its birth version from an authoritative route in this snapshot.
class NativeSparseGroupReader final {
public:
	static constexpr const char *ROOT_MEDIA = "application/vnd.zyx.lab.sparse-property-group";
	struct Loaded {
		SparseGroupDescriptor::Decoded descriptor;
		SparsePropertyGroup group;
	};
	static Loaded read(graph::storage::LargeValueStore &store,
		const graph::storage::MetadataSnapshotPin &snapshot, const graph::storage::LargeValueRef &reference,
		uint64_t block, uint32_t type, uint64_t descriptorGeneration, size_t maximumFields, size_t maximumBytes,
		const graph::storage::SelectedPropertyReadAdmission &admission,
		const graph::storage::CancellationCheckpoint *cancellation = nullptr) {
		if (cancellation) cancellation->check();
		if (!admission || descriptorGeneration == 0U || descriptorGeneration > snapshot.superblock().generation.value() ||
			reference.kind != graph::storage::LargeValueKind::LVK_BYTES || reference.media_type != ROOT_MEDIA ||
			reference.logical_bytes > maximumBytes)
			throw std::invalid_argument("Native sparse group read requires a bounded descriptor, version and admission");
		const graph::storage::SelectedPropertyReadAdmission guardedAdmission = [&](size_t bytes) {
			if (cancellation) cancellation->check();
			admission(bytes);
			if (cancellation) cancellation->check();
		};
		const auto bytes = store.read(snapshot, reference, guardedAdmission);
		if (cancellation) cancellation->check();
		const SparseGroupDescriptor::Identity identity{snapshot.superblock().database_id.value(), descriptorGeneration};
		auto descriptor = SparseGroupDescriptor::decode(bytes, block, type, identity, maximumFields, maximumBytes);
		std::vector<std::vector<std::byte>> columns;
		columns.reserve(descriptor.columns.size());
		for (const auto &column : descriptor.columns) {
			if (cancellation) cancellation->check();
			columns.push_back(store.read(snapshot, column.reference, guardedAdmission));
			if (cancellation) cancellation->check();
		}
		std::vector<PropertyBlockFieldMembership::SparseColumnView> views;
		views.reserve(columns.size());
		for (size_t index = 0U; index < columns.size(); ++index)
			views.push_back({descriptor.columns[index].token, columns[index]});
		auto group = SparsePropertyGroup::freeze(descriptor.slots, descriptor.directory, views, identity);
		if (cancellation) cancellation->check();
		return {std::move(descriptor), std::move(group)};
	}
};

} // namespace zyx::benchmark
