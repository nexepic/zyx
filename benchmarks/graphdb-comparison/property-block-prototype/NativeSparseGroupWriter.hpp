#pragma once

#include "NativeSparseGroupReader.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/utils/Sha256.hpp"

namespace zyx::benchmark {

// Lab publication carrier. No authoritative graph route or retirement is added.
class NativeSparseGroupWriter final {
public:
	static graph::storage::LargeValueRef write(graph::storage::LargeValueStore &native,
		graph::storage::RelocatableMetadataStore &metadata, graph::storage::PropertyCollectionRootStore &properties,
		const graph::storage::MetadataSnapshotPin &snapshot, const PropertyBlockOwnerSlots &slots,
		const SparsePropertyGroup &group, const graph::storage::LargeValueWriteOptions &options,
		graph::storage::LogSequenceNumber lsn, size_t maximumFields, size_t maximumBytes,
		const NativeSparseGroupReader::Loaded *before = nullptr,
		const graph::storage::MetadataPublishOptions &publishOptions = {}) {
		using namespace graph::storage;
		if (!metadata.ownsSnapshot(snapshot) || !native.usesMetadataStore(metadata) ||
			!properties.usesMetadataStore(metadata) || properties.largeValueStore() != &native ||
			snapshot.superblock().generation.value() == UINT64_MAX ||
			group.identity().database_id != snapshot.superblock().database_id.value() ||
			group.identity().generation != snapshot.superblock().generation.value() + 1U)
			throw std::invalid_argument("Native sparse publication requires one store domain and the next snapshot identity");
		if (before && (before->descriptor.slots.blockId() != slots.blockId() ||
			before->descriptor.slots.ownerType() != slots.ownerType()))
			throw std::invalid_argument("Native sparse publication source group mismatch");
		std::vector<uint32_t> tokens;
		for (const auto owner : slots.activeOwnersSorted()) {
			const auto fields = group.directory().fields(slots, slots.route(owner), group.identity());
			tokens.insert(tokens.end(), fields.begin(), fields.end());
		}
		std::ranges::sort(tokens);
		tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
		std::vector<NativeGraphLargeValueMutation> mutations;
		std::vector<LargeValueRef> reused;
		std::vector<SparseGroupDescriptor::Column> columns;
		uint64_t nextId = snapshot.superblock().next_large_value_id;
		const auto plan = [&](std::span<const std::byte> bytes, const char *media) {
			if (nextId == 0U || nextId == UINT64_MAX) throw std::overflow_error("Native sparse value identity overflow");
			auto reference = native.planReference(LargeValueId(nextId++), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
				media, options.compression, options.extent_class);
			mutations.push_back({reference, std::vector<std::byte>(bytes.begin(), bytes.end()), options.first_data_slot,
				options.last_data_slot, options.first_manifest_slot, options.last_manifest_slot, options.compression,
				options.extent_class, native.plannedDataPageCount(bytes, options.compression, options.extent_class),
				native.plannedManifestPageCount(bytes, options.compression, options.extent_class)});
			return reference;
		};
		for (const auto token : tokens) {
			const auto bytes = group.column(token);
			if (!bytes) throw std::runtime_error("Native sparse publication omitted a column");
			const auto old = before ? before->group.column(token) : nullptr;
			if (old && *old == *bytes) {
				const auto found = std::find_if(before->descriptor.columns.begin(), before->descriptor.columns.end(),
					[&](const auto &column) { return column.token == token; });
				if (found == before->descriptor.columns.end() || found->reference.content_hash != graph::utils::Sha256::digest(*bytes) ||
					found->reference.logical_bytes != bytes->size())
					throw std::runtime_error("Native sparse reused reference disagrees with its payload");
				static_cast<void>(native.manifest(snapshot, found->reference));
				columns.push_back(*found); reused.push_back(found->reference);
			} else columns.push_back({token, plan(*bytes, SparseGroupDescriptor::COLUMN_MEDIA)});
		}
		const auto descriptor = SparseGroupDescriptor::encode(slots, group.directory(), columns, group.identity(), maximumFields, maximumBytes);
		const auto root = plan(descriptor, NativeSparseGroupReader::ROOT_MEDIA);
		auto content = native.planNativeGraphContentIndexMutation(snapshot, mutations, options.first_manifest_slot, options.last_manifest_slot);
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(content.mutation);
		std::ranges::sort(reused, {}, [](const auto &reference) { return reference.value_id.value(); });
		reused.erase(std::unique(reused.begin(), reused.end()), reused.end());
		derived.reused_large_values = std::move(reused);
		static_cast<void>(metadata.mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, mutations, derived, lsn,
			[&](auto, auto, auto, auto, auto, std::span<LargeValueMaterializationResult> materializations) {
				if (materializations.size() != mutations.size()) throw std::runtime_error("Native sparse materialization count mismatch");
				for (size_t index = 0U; index < mutations.size(); ++index)
					properties.preparePlannedLargeValue(mutations[index], materializations[index], lsn);
			}, [&](auto &batches) {
				if (batches.size() != 1U) throw std::runtime_error("Native sparse content index batch mismatch");
				content.preparer(batches.front(), lsn);
			}, {}, {}, {}, {}, {}, publishOptions, {}, snapshot.superblock().generation));
		return root;
	}
};

} // namespace zyx::benchmark
