#include "NativeGraphWritePlannerContext.hpp"

namespace graph::storage {

	void NativeGraphWriteTransaction::stageVectorIndexBootstrap(
			BTreePageStore &mappingTrees, VectorPageStore &rawPages, VectorManifestStore &manifests,
			const IndexDefinition &definition, std::span<const NativeGraphVectorBootstrapRecord> records,
			NativeGraphVectorBootstrapOptions options) {
		if (options.checkpoint) options.checkpoint();
		requireActive("stage a Vector Index bootstrap");
		IndexDefinitionCodec::validate(definition);
		const uint8_t invalidInput = static_cast<uint8_t>(definition.kind != IndexDefinitionKind::IDK_VECTOR) |
				static_cast<uint8_t>(definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE) |
				static_cast<uint8_t>(definition.index_id == 0U) | static_cast<uint8_t>(records.empty()) |
				static_cast<uint8_t>(options.maximum_vectors == 0U) |
				static_cast<uint8_t>(records.size() > options.maximum_vectors) |
				static_cast<uint8_t>(options.maximum_pages == 0U) |
				static_cast<uint8_t>(!options.first_slot.isValid()) |
				static_cast<uint8_t>(!options.last_slot.isValid()) |
				static_cast<uint8_t>(options.first_slot > options.last_slot) |
				static_cast<uint8_t>(!mappingTrees.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!rawPages.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!manifests.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS);
		if (invalidInput != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector bootstrap requires a bounded complete schema and one metadata domain");
		}
		if (context_->metadata->findIndexRootOptional(snapshot_, definition.index_id).has_value() ||
			context_->metadata->findVectorManifestOptional(snapshot_, definition.index_id).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector bootstrap refuses to overwrite an existing physical Index state");
		}
		if (next_vector_id_ == 0U || records.size() - 1U > std::numeric_limits<uint64_t>::max() - next_vector_id_) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap exhausts the logical Vector-ID namespace");
		}

		const VectorDistanceMetric metric = VectorSemanticCodec::metricFor(definition);
		const size_t expectedEncodedBytes = static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t);
		std::vector<const NativeGraphVectorBootstrapRecord *> ordered;
		ordered.reserve(records.size());
		for (const NativeGraphVectorBootstrapRecord &record: records) {
			if (ordered.size() % 256U == 0U && options.checkpoint) options.checkpoint();
			if (record.node_id == 0U || record.encoded_bfloat16.size() != expectedEncodedBytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph Vector bootstrap record has an invalid Node identity or BF16 width");
			}
			const auto stagedNode = nodes_.find(record.node_id);
			if (stagedNode != nodes_.end()) {
				if (stagedNode->second.erased) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector bootstrap cannot map a Node erased by this transaction");
				}
			} else {
				// A native mapping is part of the graph generation, so it must not
				// introduce a durable reference to a non-existent logical Node.
				static_cast<void>(context_->entities->readNode(snapshot_, record.node_id));
			}
			const bool nonzeroFiniteNorm = VectorSemanticCodec::hasNonZeroFiniteL2Norm(record.encoded_bfloat16);
			if (metric == VectorDistanceMetric::VDM_COSINE && !nonzeroFiniteNorm) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph Vector bootstrap rejects a zero-norm cosine Vector record");
			}
			ordered.push_back(&record);
		}
		std::ranges::sort(ordered, {}, [](const NativeGraphVectorBootstrapRecord *record) { return record->node_id; });
		if (std::ranges::adjacent_find(ordered, {}, [](const NativeGraphVectorBootstrapRecord *record) {
				return record->node_id;
			}) != ordered.end()) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph Vector bootstrap has duplicate Node mappings");
		}

		if (expectedEncodedBytes > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap record stride exceeds the persisted width");
		}
		const uint32_t rawRecordBytes = static_cast<uint32_t>(sizeof(uint64_t) + expectedEncodedBytes);
		const uint16_t rawCapacity = VectorPageStore::rawSlotCapacity(rawRecordBytes);
		if (rawCapacity == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector bootstrap record does not fit a Raw Vector page");
		}
		const size_t rawPageCount = (ordered.size() - 1U) / static_cast<size_t>(rawCapacity) + 1U;
		if (rawPageCount > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector bootstrap Raw-page count exceeds its persisted width");
		}

		const uint64_t mappingIndexId =
				VectorNodeMapping::selectMappingIndexId(snapshot_, *context_->metadata, definition.index_id);
		if (std::ranges::any_of(vector_bootstraps_,
								[&](const StagedVectorBootstrap &staged) {
									return staged.mutation.index_id == definition.index_id ||
										   staged.mutation.vector_node_mapping_index_id == mappingIndexId;
								}) ||
			std::ranges::any_of(initial_scalar_index_mutations_, [&](const StagedInitialScalarIndexMutation &staged) {
				return staged.index_id == definition.index_id || staged.index_id == mappingIndexId;
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction has overlapping Vector bootstrap mapping identities");
		}

		utils::OwnedStdVector<BTreePageEntry> nodeMappingEntries;
		utils::OwnedStdVector<BTreePageEntry> vectorNodeMappingEntries;
		nodeMappingEntries.reserve(ordered.size());
		vectorNodeMappingEntries.reserve(ordered.size());
		StagedVectorBootstrap staged;
		staged.mapping_trees = &mappingTrees;
		staged.raw_pages = &rawPages;
		staged.manifests = &manifests;
		staged.maximum_pages = options.maximum_pages;
		staged.records.reserve(ordered.size());
		const uint64_t firstVectorId = next_vector_id_;
		for (size_t ordinal = 0U; ordinal < ordered.size(); ++ordinal) {
			if (ordinal % 256U == 0U && options.checkpoint) options.checkpoint();
			const NativeGraphVectorBootstrapRecord &record = *ordered[ordinal];
			const uint64_t vectorId = firstVectorId + static_cast<uint64_t>(ordinal);
			nodeMappingEntries.push_back({VectorSemanticCodec::nodeMappingKey(record.node_id), vectorId});
			vectorNodeMappingEntries.push_back({VectorNodeMapping::key(vectorId), record.node_id});
			staged.records.push_back(
					{record.node_id, vectorId, VectorSemanticCodec::rawRecord(vectorId, record.encoded_bfloat16)});
		}
		const BTreeInitialTreeMutationPlan nodeMappingPlan =
				mappingTrees.planInitialTreeMutation(snapshot_, definition.index_id, nodeMappingEntries);
		if (options.checkpoint) options.checkpoint();
		const BTreeInitialTreeMutationPlan vectorNodeMappingPlan =
				mappingTrees.planInitialTreeMutation(snapshot_, mappingIndexId, vectorNodeMappingEntries);
		const auto planPageCount = [](const BTreeInitialTreeMutationPlan &plan) {
			size_t count = 0U;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				if (pool.appended_page_count > std::numeric_limits<size_t>::max() - count) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native graph Vector bootstrap mapping-page count overflows host address space");
				}
				count += pool.appended_page_count;
			}
			return count;
		};
		const size_t nodeMappingPageCount = planPageCount(nodeMappingPlan);
		const size_t vectorNodeMappingPageCount = planPageCount(vectorNodeMappingPlan);
		const uint8_t invalidPagePlan = static_cast<uint8_t>(nodeMappingPageCount == 0U) |
				static_cast<uint8_t>(vectorNodeMappingPageCount == 0U) |
				static_cast<uint8_t>(nodeMappingPageCount > options.maximum_pages) |
				static_cast<uint8_t>(vectorNodeMappingPageCount > options.maximum_pages - nodeMappingPageCount) |
				static_cast<uint8_t>(rawPageCount >
									  options.maximum_pages - nodeMappingPageCount - vectorNodeMappingPageCount) |
				static_cast<uint8_t>(options.maximum_pages - nodeMappingPageCount - vectorNodeMappingPageCount - rawPageCount <
									  1U);
		if (invalidPagePlan != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector bootstrap exceeds its configured typed-page work bound");
		}

		// All cancellation checks precede mutation of this transaction's staged roots.
		if (options.checkpoint) options.checkpoint();
		stageScalarIndexCreate(mappingTrees, nodeMappingPlan);
		stageScalarIndexCreate(mappingTrees, vectorNodeMappingPlan);
		staged.mutation = {definition.index_id,
						   mappingIndexId,
						   firstVectorId,
						   static_cast<uint64_t>(ordered.size()),
						   definition.vector_dimensions,
						   metric,
						   static_cast<uint32_t>(rawPageCount),
						   options.first_slot,
						   options.last_slot};
		vector_bootstraps_.push_back(std::move(staged));
		const uint64_t lastVectorId = firstVectorId + static_cast<uint64_t>(ordered.size()) - 1U;
		next_vector_id_ = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
	}

	void NativeGraphWriteTransaction::stageVectorIndexDelta(BTreePageStore &mappingTrees, VectorPageStore &rawPages,
														VectorManifestStore &manifests,
														const IndexDefinition &definition,
														std::span<const NativeVectorMutation> mutations,
														NativeGraphVectorDeltaOptions options) {
		requireActive("stage a Vector Index delta");
		IndexDefinitionCodec::validate(definition);
		const uint8_t invalidInput = static_cast<uint8_t>(definition.kind != IndexDefinitionKind::IDK_VECTOR) |
				static_cast<uint8_t>(definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE) |
				static_cast<uint8_t>(definition.index_id == 0U) | static_cast<uint8_t>(mutations.empty()) |
				static_cast<uint8_t>(options.maximum_records == 0U) |
				static_cast<uint8_t>(mutations.size() > options.maximum_records) |
				static_cast<uint8_t>(options.maximum_raw_pages == 0U) |
				static_cast<uint8_t>(!options.first_slot.isValid()) |
				static_cast<uint8_t>(!options.last_slot.isValid()) |
				static_cast<uint8_t>(options.rebuild_threshold == 0U) |
				static_cast<uint8_t>(options.rebuild_threshold == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.rebuild_threshold > std::numeric_limits<uint32_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries == 0U) |
				static_cast<uint8_t>(options.maximum_overlay_entries == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries > std::numeric_limits<uint32_t>::max()) |
				static_cast<uint8_t>(options.rebuild_threshold > options.maximum_overlay_entries) |
				static_cast<uint8_t>(options.maximum_delta_pages == 0U) |
				static_cast<uint8_t>(options.maximum_delta_pages == std::numeric_limits<size_t>::max()) |
				static_cast<uint8_t>(options.maximum_overlay_entries >
									  VectorDeltaMapping::maximumEntriesForPageBudget(options.maximum_delta_pages)) |
				static_cast<uint8_t>(options.first_slot > options.last_slot) |
				static_cast<uint8_t>(!mappingTrees.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!rawPages.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(!manifests.usesMetadataStore(*context_->metadata)) |
				static_cast<uint8_t>(definition.vector_dimensions == 0U) |
				static_cast<uint8_t>(definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS);
		if (invalidInput != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph Vector delta requires bounded canonical input in one metadata domain");
		}
		if (mutations.size() > std::numeric_limits<size_t>::max() / 2U) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta mapping work exceeds host address space");
		}
		if (std::ranges::any_of(vector_bootstraps_,
								[&](const StagedVectorBootstrap &staged) {
									return staged.mutation.index_id == definition.index_id;
								}) ||
			std::ranges::any_of(vector_deltas_, [&](const StagedVectorDelta &staged) {
				return staged.mutation.index_id == definition.index_id;
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction cannot stage more than one Vector publication shape per Index");
		}

		const VectorDistanceMetric metric = VectorSemanticCodec::metricFor(definition);
		const std::optional<IndexRootLocation> nodeMappingRoot =
				context_->metadata->findIndexRootOptional(snapshot_, definition.index_id);
		const std::optional<VectorManifestLocation> manifestDirectory =
				context_->metadata->findVectorManifestOptional(snapshot_, definition.index_id);
		if (!nodeMappingRoot.has_value() || !manifestDirectory.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta requires an already published mapping root and Manifest");
		}
		const VectorManifest sourceManifest = manifests.read(snapshot_, definition.index_id).manifest();
		const std::optional<IndexRootLocation> reverseMappingRoot =
				sourceManifest.vector_node_mapping_index_id == 0U
						? std::nullopt
						: context_->metadata->findIndexRootOptional(snapshot_,
																	sourceManifest.vector_node_mapping_index_id);
		const bool hasSourceDelta = sourceManifest.delta_mapping_index_id != 0U;
		const std::optional<IndexRootLocation> sourceDeltaRoot =
				hasSourceDelta
						? context_->metadata->findIndexRootOptional(snapshot_, sourceManifest.delta_mapping_index_id)
						: std::nullopt;
		const PageLocation manifestLocation = context_->metadata->findPageLocation(snapshot_, sourceManifest.page_id);
		const CatalogEntry manifestPool = context_->metadata->findCatalog(snapshot_, manifestLocation.segment_id);
		const uint8_t invalidManifest =
				static_cast<uint8_t>(sourceManifest.index_id != definition.index_id) |
				static_cast<uint8_t>(sourceManifest.dimensions != definition.vector_dimensions) |
				static_cast<uint8_t>(sourceManifest.metric != metric) |
				static_cast<uint8_t>(sourceManifest.node_mapping_root_page_id != nodeMappingRoot->root_page_id) |
				static_cast<uint8_t>(sourceManifest.node_mapping_root_generation != nodeMappingRoot->root_generation) |
				static_cast<uint8_t>(
						!VectorNodeMapping::isMappingIndexId(sourceManifest.vector_node_mapping_index_id)) |
				static_cast<uint8_t>(!reverseMappingRoot.has_value()) |
				static_cast<uint8_t>(reverseMappingRoot.has_value() &&
									 sourceManifest.vector_node_mapping_root_page_id !=
											 reverseMappingRoot->root_page_id) |
				static_cast<uint8_t>(reverseMappingRoot.has_value() &&
									 sourceManifest.vector_node_mapping_root_generation !=
											 reverseMappingRoot->root_generation) |
				static_cast<uint8_t>(hasSourceDelta &&
									 !VectorDeltaMapping::isMappingIndexId(sourceManifest.delta_mapping_index_id)) |
				static_cast<uint8_t>(hasSourceDelta != sourceManifest.delta_mapping_root_page_id.isValid()) |
				static_cast<uint8_t>(hasSourceDelta != sourceManifest.delta_mapping_root_generation.isValid()) |
				static_cast<uint8_t>(hasSourceDelta != (sourceManifest.delta_entry_count != 0U)) |
				static_cast<uint8_t>(hasSourceDelta != sourceDeltaRoot.has_value()) |
				static_cast<uint8_t>(sourceDeltaRoot.has_value() &&
									 sourceManifest.delta_mapping_root_page_id != sourceDeltaRoot->root_page_id) |
				static_cast<uint8_t>(sourceDeltaRoot.has_value() &&
									 sourceManifest.delta_mapping_root_generation != sourceDeltaRoot->root_generation) |
				static_cast<uint8_t>(manifestDirectory->manifest_page_id != sourceManifest.page_id) |
				static_cast<uint8_t>(manifestDirectory->generation != sourceManifest.generation) |
				static_cast<uint8_t>(manifestPool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
				static_cast<uint8_t>(manifestPool.owner_id != definition.index_id) |
				static_cast<uint8_t>(manifestPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(manifestPool.generation != manifestLocation.generation) |
				static_cast<uint8_t>(!manifestPool.committed_lsn.isValid()) |
				static_cast<uint8_t>(!sourceManifest.committed_lsn.isValid());
		if (invalidManifest != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta found an inconsistent mapping or Manifest fence");
		}

		const size_t encodedBytes = static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t);
		if (encodedBytes > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta raw record stride exceeds persisted width");
		}
		const uint32_t rawRecordBytes = static_cast<uint32_t>(sizeof(uint64_t) + encodedBytes);
		const uint16_t rawCapacity = VectorPageStore::rawSlotCapacity(rawRecordBytes);
		if (rawCapacity == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta record does not fit a Raw Vector page");
		}
		std::map<uint64_t, VectorDeltaEntry> originalDeltaOperations;
		if (hasSourceDelta) {
			// The threshold schedules maintenance, while the independently bounded
			// overlay limit remains a hard safety ceiling for retained exact state.
			const size_t sourceDeltaEntries = static_cast<size_t>(sourceManifest.delta_entry_count);
			if (sourceDeltaEntries > options.maximum_overlay_entries) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta exceeds its configured durable overlay bound");
			}
			const size_t scanLimit = std::max(options.rebuild_threshold, sourceDeltaEntries);
			const auto sourceEntries = VectorDeltaMapping::scanOwned(
					snapshot_, mappingTrees, *context_->metadata, sourceManifest.delta_mapping_index_id, scanLimit);
			if (sourceEntries.size() != sourceManifest.delta_entry_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta Manifest count disagrees with its typed overlay tree");
			}
			for (const VectorDeltaEntry &entry: sourceEntries) {
				if (!originalDeltaOperations.emplace(entry.vector_id, entry).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta overlay contains a duplicate Vector-ID entry");
				}
			}
		}
		std::map<uint64_t, VectorDeltaEntry> deltaOperations = originalDeltaOperations;

		std::vector<const NativeVectorMutation *> ordered;
		ordered.reserve(mutations.size());
		for (const NativeVectorMutation &mutation: mutations) {
			const bool insert = mutation.kind == NativeVectorMutationKind::NVMK_INSERT;
			const bool erase = mutation.kind == NativeVectorMutationKind::NVMK_ERASE;
			const uint8_t invalid = static_cast<uint8_t>(!insert && !erase) |
									static_cast<uint8_t>(mutation.index_id != definition.index_id) |
									static_cast<uint8_t>(mutation.node_id == 0U) |
									static_cast<uint8_t>(mutation.dimensions != definition.vector_dimensions) |
									static_cast<uint8_t>(mutation.metric != metric) |
									static_cast<uint8_t>(insert && mutation.encoded_bfloat16.size() != encodedBytes) |
									static_cast<uint8_t>(erase && !mutation.encoded_bfloat16.empty());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta has an invalid semantic mutation");
			}
			if (insert) {
				const auto stagedNode = nodes_.find(mutation.node_id);
				if (stagedNode != nodes_.end()) {
					if (stagedNode->second.erased) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector delta cannot insert a mapping for a Node erased by this transaction");
					}
				} else {
					static_cast<void>(context_->entities->readNode(snapshot_, mutation.node_id));
				}
				const bool nonzeroFiniteNorm = VectorSemanticCodec::hasNonZeroFiniteL2Norm(mutation.encoded_bfloat16);
				if (metric == VectorDistanceMetric::VDM_COSINE && !nonzeroFiniteNorm) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native graph Vector delta rejects a zero-norm cosine Vector record");
				}
			}
			ordered.push_back(&mutation);
		}
		std::ranges::sort(ordered, {}, [](const NativeVectorMutation *mutation) {
			return std::tuple<uint64_t, uint8_t>{mutation->node_id, static_cast<uint8_t>(mutation->kind)};
		});
		for (size_t ordinal = 1U; ordinal < ordered.size(); ++ordinal) {
			const NativeVectorMutation &previous = *ordered[ordinal - 1U];
			const NativeVectorMutation &current = *ordered[ordinal];
			if (previous.node_id == current.node_id && previous.kind == current.kind) {
				fail(FormatError::FERR_INVALID_HEADER, "native graph Vector delta has duplicate Node transition kinds");
			}
		}

		StagedVectorDelta staged;
		staged.mapping_trees = &mappingTrees;
		staged.raw_pages = &rawPages;
		staged.manifests = &manifests;
		staged.source_manifest = sourceManifest;
		staged.maximum_raw_pages = options.maximum_raw_pages;
		staged.mutation.index_id = definition.index_id;
		staged.mutation.vector_node_mapping_index_id = sourceManifest.vector_node_mapping_index_id;
		staged.mutation.expected_delta_mapping_index_id = sourceManifest.delta_mapping_index_id;
		staged.mutation.expected_delta_mapping_root = sourceDeltaRoot;
		staged.mutation.expected_node_mapping_root = *nodeMappingRoot;
		staged.mutation.expected_vector_node_mapping_root = *reverseMappingRoot;
		staged.mutation.expected_manifest_pool = manifestPool;
		staged.mutation.expected_manifest_location = manifestLocation;
		staged.mutation.expected_manifest = *manifestDirectory;
		staged.mutation.expected_manifest_owner_lsn = sourceManifest.committed_lsn;
		staged.mutation.dimensions = definition.vector_dimensions;
		staged.mutation.metric = sourceManifest.metric;
		staged.mutation.first_slot = options.first_slot;
		staged.mutation.last_slot = options.last_slot;
		staged.mutation.manifest_in_new_pool = !manifests.hasReplacementFrameCapacity(manifestPool, 1U);

		std::set<uint64_t> erasedNodes;
		std::set<uint64_t> insertedNodes;
		struct MappingOperation {
			bool erase = false;
			uint64_t index_id = 0U;
			std::vector<std::byte> key;
			uint64_t value = 0U;
		};
		std::vector<MappingOperation> mappingOperations;
		mappingOperations.reserve(mutations.size() * 2U);
		for (const NativeVectorMutation *mutation: ordered) {
			if (mutation->kind != NativeVectorMutationKind::NVMK_ERASE) {
				continue;
			}
			const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(mutation->node_id);
			const std::optional<uint64_t> vectorId =
					BTreeIndex(definition.index_id, mappingTrees, *context_->metadata).find(snapshot_, key);
			const std::optional<uint64_t> reverseNode =
					vectorId.has_value()
							? VectorNodeMapping::findNode(snapshot_, mappingTrees, *context_->metadata,
														  sourceManifest.vector_node_mapping_index_id, *vectorId)
							: std::nullopt;
			if (!vectorId.has_value() || *vectorId == 0U || !reverseNode.has_value() || // staged Vector mappings are produced only after mapping-tree validation.
				*reverseNode != mutation->node_id ||
				!context_->metadata->findVectorLocationOptional(snapshot_, *vectorId).has_value() ||
				!erasedNodes.insert(mutation->node_id).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta erase found an inconsistent semantic mapping");
			}
			mappingOperations.push_back({true, definition.index_id, key, 0U});
			mappingOperations.push_back(
					{true, sourceManifest.vector_node_mapping_index_id, VectorNodeMapping::key(*vectorId), 0U});
			staged.records.push_back({NativeVectorMutationKind::NVMK_ERASE, mutation->node_id, *vectorId, {}});
			staged.mutation.erased_vector_ids.push_back(*vectorId);
			const auto delta = deltaOperations.find(*vectorId);
			if (delta == deltaOperations.end()) {
				deltaOperations.emplace(
						*vectorId, VectorDeltaEntry{*vectorId, mutation->node_id, VectorDeltaOperation::VDO_ERASE});
			} else if (delta->second.operation == VectorDeltaOperation::VDO_INSERT) {
				if (delta->second.node_id != mutation->node_id) { // reverse mapping lookup is authenticated against the staged Node identity.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta erase changed the Node identity of an inserted Vector");
				}
				deltaOperations.erase(delta);
			} else {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta found a live semantic mapping hidden by an overlay tombstone");
			}
		}

		const size_t insertCount =
				static_cast<size_t>(std::ranges::count_if(ordered, [](const NativeVectorMutation *mutation) {
					return mutation->kind == NativeVectorMutationKind::NVMK_INSERT;
				}));
		uint64_t nextVectorId = next_vector_id_;
		if (insertCount != 0U &&
			(nextVectorId == 0U || insertCount - 1U > std::numeric_limits<uint64_t>::max() - nextVectorId)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta exhausts the logical Vector-ID namespace");
		}
		staged.mutation.first_vector_id = insertCount == 0U ? 0U : nextVectorId;
		staged.mutation.inserted_vector_count = static_cast<uint64_t>(insertCount);
		for (const NativeVectorMutation *mutation: ordered) {
			if (mutation->kind != NativeVectorMutationKind::NVMK_INSERT) {
				continue;
			}
			const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(mutation->node_id);
			const std::optional<uint64_t> existing =
					BTreeIndex(definition.index_id, mappingTrees, *context_->metadata).find(snapshot_, key);
			if ((existing.has_value() && !erasedNodes.contains(mutation->node_id)) || // duplicate staged Vector identities are rejected during request validation.
				!insertedNodes.insert(mutation->node_id).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta insert found a duplicate Node mapping");
			}
			const uint64_t vectorId = nextVectorId;
			nextVectorId = vectorId == std::numeric_limits<uint64_t>::max() ? 0U : vectorId + 1U;
			mappingOperations.push_back({false, definition.index_id, key, vectorId});
			mappingOperations.push_back({false, sourceManifest.vector_node_mapping_index_id,
										 VectorNodeMapping::key(vectorId), mutation->node_id});
			staged.records.push_back({NativeVectorMutationKind::NVMK_INSERT, mutation->node_id, vectorId,
									  VectorSemanticCodec::rawRecord(vectorId, mutation->encoded_bfloat16)});
			if (!deltaOperations
						 .emplace(vectorId,
								  VectorDeltaEntry{vectorId, mutation->node_id, VectorDeltaOperation::VDO_INSERT})
						 .second) {
				fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta reused an existing overlay Vector ID");
			}
		}

		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(snapshot_.superblock());
		std::vector<size_t> insertRecordOrdinals;
		for (size_t ordinal = 0U; ordinal < staged.records.size(); ++ordinal) {
			if (staged.records[ordinal].kind == NativeVectorMutationKind::NVMK_INSERT) {
				insertRecordOrdinals.push_back(ordinal);
			}
		}
		if (!insertRecordOrdinals.empty()) {
			if (!sourceManifest.raw_write_tail_page_id.isValid()) { // a delta source is admitted only with a durable Raw tail.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Vector delta has inserts but no current Raw write tail");
			}
			const PageLocation tailLocation =
					context_->metadata->findPageLocation(snapshot_, sourceManifest.raw_write_tail_page_id);
			const CatalogEntry tailPool = context_->metadata->findCatalog(snapshot_, tailLocation.segment_id);
			const VectorPage tail = rawPages.read(snapshot_, tailLocation.page_id);
			const VectorPageHeader tailHeader = tail.header();
			const uint8_t invalidTail = static_cast<uint8_t>(tailPool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
										static_cast<uint8_t>(tailPool.owner_id != definition.index_id) |
										static_cast<uint8_t>(tailPool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(tailPool.generation != tailLocation.generation) |
										static_cast<uint8_t>(tailHeader.kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(tailHeader.index_id != definition.index_id) |
										static_cast<uint8_t>(tailHeader.page_id != tailLocation.page_id) |
										static_cast<uint8_t>(tailHeader.generation != tailLocation.generation) |
										static_cast<uint8_t>(tailHeader.record_bytes != rawRecordBytes) |
										static_cast<uint8_t>(tailHeader.slot_capacity != rawCapacity) |
										static_cast<uint8_t>(!tailHeader.committed_lsn.isValid());
			if (invalidTail != 0U) { // source Manifest page bounds are validated before delta planning.
				fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta found an invalid Raw write tail");
			}
			size_t nextRecord = 0U;
			const size_t tailFree = static_cast<size_t>(tailHeader.slot_capacity - tailHeader.live_count);
			if (tailFree != 0U && rawPages.hasReplacementFrameCapacity(tailPool, 1U)) {
				const PageOwnerHeader owner{tailLocation.page_id,
											definition.index_id,
											PageOwnerKind::POK_VECTOR_RAW,
											0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											tailLocation.generation,
											checkpointLsn};
				VectorPage image = tail.rebindOwner(owner);
				const size_t count = std::min(tailFree, insertRecordOrdinals.size());
				StagedVectorDeltaRawPage rawPage;
				rawPage.mutation = {NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL,
									tailPool,
									tailLocation,
									tailHeader.committed_lsn,
									0U,
									0U,
									static_cast<uint32_t>(count)};
				rawPage.record_ordinals.insert(rawPage.record_ordinals.end(), insertRecordOrdinals.begin(),
											   insertRecordOrdinals.begin() + static_cast<std::ptrdiff_t>(count));
				for (const size_t recordOrdinal: rawPage.record_ordinals) {
					static_cast<void>(image.insert(staged.records[recordOrdinal].raw_record));
				}
				rawPage.tail_image.emplace(std::move(image));
				staged.raw_page_images.push_back(std::move(rawPage));
				nextRecord = count;
			}
			const size_t remaining = insertRecordOrdinals.size() - nextRecord;
			if (remaining != 0U) {
				const size_t freshPageCount = (remaining - 1U) / static_cast<size_t>(rawCapacity) + 1U;
				if (staged.raw_page_images.size() + freshPageCount > options.maximum_raw_pages) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta exceeds its configured Raw-page work bound");
				}
				const bool appendCurrent =
						rawPages.hasReplacementFrameCapacity(tailPool, freshPageCount + (nextRecord != 0U ? 1U : 0U));
				for (size_t pageOrdinal = 0U; pageOrdinal < freshPageCount; ++pageOrdinal) {
					const size_t first = nextRecord + pageOrdinal * static_cast<size_t>(rawCapacity);
					const size_t count =
							std::min(static_cast<size_t>(rawCapacity), insertRecordOrdinals.size() - first);
					StagedVectorDeltaRawPage rawPage;
					rawPage.mutation.placement =
							appendCurrent ? NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL
										  : NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL;
					rawPage.mutation.expected_pool = appendCurrent ? tailPool : CatalogEntry{};
					rawPage.mutation.page_ordinal = static_cast<uint32_t>(staged.raw_page_images.size());
					rawPage.mutation.new_pool_ordinal =
							appendCurrent ? 0U
										  : static_cast<uint32_t>(pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
					rawPage.mutation.inserted_record_count = static_cast<uint32_t>(count);
					rawPage.record_ordinals.insert(rawPage.record_ordinals.end(),
												   insertRecordOrdinals.begin() + static_cast<std::ptrdiff_t>(first),
												   insertRecordOrdinals.begin() +
														   static_cast<std::ptrdiff_t>(first + count));
					staged.raw_page_images.push_back(std::move(rawPage));
				}
			}
		}
		if (staged.raw_page_images.size() > options.maximum_raw_pages) { // raw page planning is bounded before materialization.
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta exceeds its configured Raw-page work bound");
		}
		staged.mutation.raw_pages.reserve(staged.raw_page_images.size());
		for (const StagedVectorDeltaRawPage &rawPage: staged.raw_page_images) {
			staged.mutation.raw_pages.push_back(rawPage.mutation);
		}
		// Semantic operations are ordered by Node ID so mapping updates are
		// deterministic; directory erasures instead require Vector-ID order.
		std::ranges::sort(staged.mutation.erased_vector_ids);
		if (std::ranges::adjacent_find(staged.mutation.erased_vector_ids) != staged.mutation.erased_vector_ids.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta resolved one logical Vector ID more than once");
		}
		// Crossing the configured threshold schedules a base rebuild after this
		// transaction publishes. Keeping the overlay visible until the rebuild
		// completes preserves the `base - tombstones + exact inserts` invariant.
		if (deltaOperations.size() > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph Vector delta entry count exceeds its persisted width");
		}
		if (deltaOperations.size() > options.maximum_overlay_entries) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph Vector delta exceeds its configured durable overlay bound");
		}
		staged.mutation.maintenance_rebuild_required = deltaOperations.size() > options.rebuild_threshold;
		staged.mutation.delta_entry_count = static_cast<uint32_t>(deltaOperations.size());
		if (!deltaOperations.empty()) {
			const uint64_t deltaMappingIndexId =
					hasSourceDelta ? sourceManifest.delta_mapping_index_id
								   : VectorDeltaMapping::selectMappingIndexId(snapshot_, *context_->metadata,
																			  definition.index_id);
			staged.mutation.delta_mapping_index_id = deltaMappingIndexId;
			if (hasSourceDelta) {
				for (const auto &[vectorId, original]: originalDeltaOperations) {
					const auto updated = deltaOperations.find(vectorId);
					if (updated == deltaOperations.end()) {
						stageScalarIndexErase(mappingTrees, deltaMappingIndexId,
											  VectorDeltaMapping::key(vectorId, original.operation));
					} else if (updated->second != original) {
						stageScalarIndexErase(mappingTrees, deltaMappingIndexId,
											  VectorDeltaMapping::key(vectorId, original.operation));
						stageScalarIndexUpsert(mappingTrees, deltaMappingIndexId,
											   VectorDeltaMapping::key(vectorId, updated->second.operation),
											   updated->second.node_id);
					}
				}
				for (const auto &[vectorId, updated]: deltaOperations) {
					if (!originalDeltaOperations.contains(vectorId)) {
						stageScalarIndexUpsert(mappingTrees, deltaMappingIndexId,
											   VectorDeltaMapping::key(vectorId, updated.operation), updated.node_id);
					}
				}
			} else {
				utils::OwnedStdVector<BTreePageEntry> entries;
				entries.reserve(deltaOperations.size());
				for (const auto &[vectorId, entry]: deltaOperations) {
					entries.push_back({VectorDeltaMapping::key(vectorId, entry.operation), entry.node_id});
				}
				stageScalarIndexCreate(mappingTrees, deltaMappingIndexId, entries);
			}
		} else if (hasSourceDelta) {
			stageScalarIndexDrop(mappingTrees, sourceManifest.delta_mapping_index_id, options.maximum_delta_pages);
		}
		for (const MappingOperation &operation: mappingOperations) {
			if (operation.erase) {
				stageScalarIndexErase(mappingTrees, operation.index_id, operation.key);
			} else {
				stageScalarIndexUpsert(mappingTrees, operation.index_id, operation.key, operation.value);
			}
		}
		next_vector_id_ = nextVectorId;
		vector_deltas_.push_back(std::move(staged));
	}

} // namespace graph::storage
