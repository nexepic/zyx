/**
 * @file OfflineVectorPqRebuild.cpp
 * @brief Offline, resumable publication of one derived PQ vector generation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline PQ rebuild cannot advance the destination checkpoint LSN");
			}
			return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] std::vector<std::byte> rawRecord(uint64_t vectorId, std::span<const std::byte> rawBfloat16) {
			// Product-quantizer training has already validated this canonical input.
			std::vector<std::byte> result(sizeof(uint64_t) + rawBfloat16.size());
			LittleEndianCodec::writeU64(result, 0, vectorId);
			std::copy(rawBfloat16.begin(), rawBfloat16.end(), result.begin() + sizeof(uint64_t));
			return result;
		}

		[[nodiscard]] std::vector<std::byte> pqRecord(uint64_t vectorId, std::span<const uint8_t> codes) {
			// `encodeRecords` derives codes from the same validated canonical input.
			std::vector<std::byte> result(sizeof(uint64_t) + codes.size());
			LittleEndianCodec::writeU64(result, 0, vectorId);
			for (size_t index = 0; index < codes.size(); ++index) {
				result[sizeof(uint64_t) + index] = static_cast<std::byte>(codes[index]);
			}
			return result;
		}

		void validateOptions(uint64_t indexId, std::span<const DeterministicPqTrainingRecord> records,
							 const OfflineVectorPqRebuildOptions &options) {
			const uint8_t invalid =
					static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(records.empty()) |
					static_cast<uint8_t>(
							!VectorCodebookExtentIdCodec::canEncodeCodebookId(options.codebook.codebook_id)) |
					static_cast<uint8_t>(!options.codebook.manifest_segment_id.isValid()) |
					static_cast<uint8_t>(!options.codebook.generation.isValid()) |
					static_cast<uint8_t>(options.codebook.element_type != VectorCodebookElementType::VCET_FLOAT32) |
					static_cast<uint8_t>(options.codebook.dimensions != options.training.dimensions) |
					static_cast<uint8_t>(options.codebook.subspace_count != options.training.subspace_count) |
					static_cast<uint8_t>(options.codebook.centroid_count != options.training.centroid_count) |
					static_cast<uint8_t>(!options.codebook_first_slot.isValid()) |
					static_cast<uint8_t>(!options.codebook_last_slot.isValid()) |
					static_cast<uint8_t>(options.codebook_first_slot > options.codebook_last_slot) |
					static_cast<uint8_t>(options.pages.codebook_generation != options.codebook.generation) |
					static_cast<uint8_t>(!options.pages.first_slot.isValid()) |
					static_cast<uint8_t>(!options.pages.last_slot.isValid()) |
					static_cast<uint8_t>(options.pages.first_slot > options.pages.last_slot) |
					static_cast<uint8_t>(options.pages.maximum_records == 0U) |
					static_cast<uint8_t>(records.size() > options.pages.maximum_records) |
					static_cast<uint8_t>(options.pages.maximum_pages == 0U) |
					static_cast<uint8_t>(options.mapping_index_id == 0U) |
					static_cast<uint8_t>(options.mapping.maximum_entries == 0U) |
					static_cast<uint8_t>(options.mapping.maximum_entries == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(records.size() > options.mapping.maximum_entries) |
					static_cast<uint8_t>(options.mapping.tree.maximum_entries != options.mapping.maximum_entries) |
					static_cast<uint8_t>(options.mapping.tree.pool_owner_id != options.mapping_index_id) |
					static_cast<uint8_t>(!options.mapping.tree.first_slot.isValid()) |
					static_cast<uint8_t>(!options.mapping.tree.last_slot.isValid()) |
					static_cast<uint8_t>(options.mapping.tree.first_slot > options.mapping.tree.last_slot) |
					static_cast<uint8_t>(options.mapping.tree.maximum_pages == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline PQ rebuild requires bounded compatible codebook, page, and mapping identities");
			}
			VectorPqMapping::validateMappingIndexId(options.mapping_index_id);
		}

		void verifyRawSource(const MetadataSnapshotPin &snapshot, VectorPageStore &vectors,
							 RelocatableMetadataStore &metadata, uint64_t indexId,
							 std::span<const DeterministicPqTrainingRecord> records) {
			for (const DeterministicPqTrainingRecord &record: records) {
				const VectorLocation location = metadata.findVectorLocation(snapshot, record.vector_id);
				const VectorPage page = vectors.read(snapshot, location.page_id);
				const uint8_t invalid = static_cast<uint8_t>(page.header().index_id != indexId) |
										static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(page.read(location.slot_id, location.slot_generation) !=
															 rawRecord(record.vector_id, record.raw_bfloat16));
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild raw Vector Location disagrees with its canonical training input");
				}
			}
		}

		void verifyCodebook(const VectorCodebook &actual, const VectorCodebookWriteRequest &expected,
							std::span<const std::byte> payload) {
			const uint8_t invalid =
					static_cast<uint8_t>(actual.header.codebook_id != expected.codebook_id) |
					static_cast<uint8_t>(actual.header.generation != expected.generation) |
					static_cast<uint8_t>(actual.header.element_type != expected.element_type) |
					static_cast<uint8_t>(actual.header.dimensions != expected.dimensions) |
					static_cast<uint8_t>(actual.header.subspace_count != expected.subspace_count) |
					static_cast<uint8_t>(actual.header.centroid_count != expected.centroid_count) |
					static_cast<uint8_t>(actual.payload.size() != payload.size()) |
					static_cast<uint8_t>(actual.payload.size() == payload.size() &&
										 !std::equal(actual.payload.begin(), actual.payload.end(), payload.begin()));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild existing codebook disagrees with the deterministic training result");
			}
		}

		[[nodiscard]] std::vector<PqVectorMaterializationRecord>
		encodeRecords(const DeterministicProductQuantizer &quantizer,
					  std::span<const DeterministicPqTrainingRecord> records) {
			std::vector<PqVectorMaterializationRecord> result;
			result.reserve(records.size());
			for (const DeterministicPqTrainingRecord &record: records) {
				result.push_back({record.vector_id, quantizer.encode(record.raw_bfloat16)});
			}
			return result;
		}

		void verifyMapping(const MetadataSnapshotPin &snapshot, BTreePageStore &trees, VectorPageStore &vectors,
						   RelocatableMetadataStore &metadata, uint64_t indexId, uint64_t mappingIndexId,
						   Generation codebookGeneration, std::span<const PqVectorMaterializationRecord> expected,
						   size_t maximumEntries) {
			const std::vector<BTreePageEntry> actual =
					BTreeIndex(mappingIndexId, trees, metadata).scan(snapshot, std::nullopt, maximumEntries + 1U);
			if (actual.size() != expected.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild existing mapping has a different canonical Vector ID set");
			}
			for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
				const PqVectorMaterializationRecord &record = expected[ordinal];
				const BTreePageEntry &entry = actual[ordinal];
				const uint8_t invalidEntry = static_cast<uint8_t>(entry.key != VectorPqMapping::key(record.vector_id)) |
											 static_cast<uint8_t>(entry.value_or_child == 0U);
				if (invalidEntry != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild existing mapping disagrees with canonical Vector IDs");
				}
				const VectorPage page = vectors.read(snapshot, PageId(entry.value_or_child));
				const uint8_t invalidPage =
						static_cast<uint8_t>(page.header().index_id != indexId) |
						static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_PQ) |
						static_cast<uint8_t>(page.header().codebook_generation != codebookGeneration);
				const std::optional<uint16_t> slot = page.findVectorId(record.vector_id);
				if (invalidPage != 0U || !slot.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild existing mapping resolves to an invalid PQ Vector record");
				}
				if (page.read(*slot) != pqRecord(record.vector_id, record.codes)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild existing mapping resolves to an invalid PQ Vector record");
				}
			}
		}

		// A replacement may train a new Codebook, so its old PQ codes are not
		// compared with the new candidate. This still proves that the visible
		// generation is structurally complete before its exact frames are retired.
		[[nodiscard]] std::vector<PageRetirement>
		verifyExistingPqGeneration(const MetadataSnapshotPin &snapshot, BTreePageStore &trees, VectorPageStore &vectors,
								   RelocatableMetadataStore &metadata, uint64_t indexId, uint64_t mappingIndexId,
								   const VectorManifest &manifest,
								   std::span<const PqVectorMaterializationRecord> expected, size_t maximumEntries) {
			const std::vector<BTreePageEntry> mapping =
					BTreeIndex(mappingIndexId, trees, metadata).scan(snapshot, std::nullopt, maximumEntries + 1U);
			if (mapping.size() != expected.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild visible mapping has a different canonical Vector ID set");
			}

			std::map<PageId, uint32_t> visibleManifestPages;
			for (const VectorManifestEntry &entry: manifest.entries) {
				if (entry.storage_kind != VectorStorageKind::VSK_PQ) {
					continue;
				}
				if (!entry.page_id.isValid() || entry.record_count == 0U ||
					!visibleManifestPages.emplace(entry.page_id, entry.record_count).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild visible Manifest has malformed or duplicate PQ page inventory");
				}
			}

			std::map<PageId, PageRetirement> observedPages;
			for (size_t ordinal = 0U; ordinal < mapping.size(); ++ordinal) {
				const BTreePageEntry &entry = mapping[ordinal];
				const PqVectorMaterializationRecord &record = expected[ordinal];
				if (entry.key != VectorPqMapping::key(record.vector_id) || entry.value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild visible mapping has an invalid Vector ID or target page");
				}
				const PageId pageId(entry.value_or_child);
				const VectorPage page = vectors.read(snapshot, pageId);
				const std::optional<uint16_t> slot = page.findVectorId(record.vector_id);
				const uint8_t invalidPage =
						static_cast<uint8_t>(page.header().index_id != indexId) |
						static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_PQ) |
						static_cast<uint8_t>(page.header().codebook_generation != manifest.codebook_generation) |
						static_cast<uint8_t>(!slot.has_value());
				if (invalidPage != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild visible mapping resolves to an invalid PQ Vector record");
				}
				const std::optional<DerivedVectorPqPage> observed = vectors.inspectDerivedPqPage(snapshot, pageId);
				if (!observed.has_value() || observed->index_id != indexId ||
					observed->location.generation != page.header().generation ||
					!visibleManifestPages.contains(pageId) ||
					visibleManifestPages.at(pageId) != page.header().live_count) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild visible Manifest and PQ-page inventory disagree");
				}
				observedPages.emplace(pageId, PageRetirement{observed->location, indexId, PageOwnerKind::POK_VECTOR_PQ,
															 observed->owner_lsn});
			}
			if (observedPages.size() != visibleManifestPages.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild visible Manifest names a PQ page that no mapping entry reaches");
			}

			std::vector<PageRetirement> result;
			result.reserve(observedPages.size());
			for (const auto &[pageId, retirement]: observedPages) {
				static_cast<void>(pageId);
				result.push_back(retirement);
			}
			return result;
		}

		[[nodiscard]] std::vector<VectorManifestEntry> manifestEntries(const VectorManifest &base,
																	   const VectorPqMaterializationResult &pq) {
			std::vector<VectorManifestEntry> result;
			result.reserve(base.entries.size() + pq.pages.size());
			for (const VectorManifestEntry &entry: base.entries) {
				if (entry.storage_kind != VectorStorageKind::VSK_PQ) {
					result.push_back(entry);
				}
			}
			for (const VectorPqMaterializationPage &page: pq.pages) {
				result.push_back({page.page_id, VectorStorageKind::VSK_PQ, page.record_count});
			}
			std::ranges::sort(result, [](const VectorManifestEntry &left, const VectorManifestEntry &right) {
				return std::pair{static_cast<uint16_t>(left.storage_kind), left.page_id.value()} <
					   std::pair{static_cast<uint16_t>(right.storage_kind), right.page_id.value()};
			});
			return result;
		}

		[[nodiscard]] std::vector<VectorManifestEntry>
		manifestEntriesFromMapping(const MetadataSnapshotPin &snapshot, BTreePageStore &trees, VectorPageStore &vectors,
								   RelocatableMetadataStore &metadata, const VectorManifest &base,
								   uint64_t mappingIndexId, size_t maximumEntries) {
			const std::vector<BTreePageEntry> mapping =
					BTreeIndex(mappingIndexId, trees, metadata).scan(snapshot, std::nullopt, maximumEntries);
			std::set<PageId> pageIds;
			for (const BTreePageEntry &entry: mapping) {
				// `verifyMapping` has already rejected zero or invalid mapping values.
				pageIds.emplace(entry.value_or_child);
			}
			std::vector<VectorManifestEntry> result;
			result.reserve(base.entries.size() + pageIds.size());
			for (const VectorManifestEntry &entry: base.entries) {
				if (entry.storage_kind != VectorStorageKind::VSK_PQ) {
					result.push_back(entry);
				}
			}
			for (const PageId pageId: pageIds) {
				const VectorPage page = vectors.read(snapshot, pageId);
				result.push_back({pageId, VectorStorageKind::VSK_PQ, page.header().live_count});
			}
			std::ranges::sort(result, [](const VectorManifestEntry &left, const VectorManifestEntry &right) {
				return std::pair{static_cast<uint16_t>(left.storage_kind), left.page_id.value()} <
					   std::pair{static_cast<uint16_t>(right.storage_kind), right.page_id.value()};
			});
			return result;
		}

		[[nodiscard]] bool manifestMatches(const VectorManifest &manifest, uint64_t indexId,
										   const OfflineVectorPqRebuildOptions &options) {
			return manifest.index_id == indexId && manifest.codebook_id == options.codebook.codebook_id &&
				   manifest.codebook_generation == options.codebook.generation &&
				   manifest.pq_mapping_index_id == options.mapping_index_id;
		}

		[[nodiscard]] std::vector<VectorPqMaterializationPoolRequest>
		planPqPools(std::span<const PqVectorMaterializationRecord> records,
					const VectorPqMaterializationOptions &options) {
			if (records.empty() || records.front().codes.empty() ||
				records.front().codes.size() > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline PQ rebuild cannot plan an empty or unrepresentable PQ record layout");
			}
			const uint32_t recordBytes = static_cast<uint32_t>(sizeof(uint64_t) + records.front().codes.size());
			const uint16_t slotsPerPage = VectorPageStore::rawSlotCapacity(recordBytes);
			const size_t pageCount = (records.size() - 1U) / slotsPerPage + 1U;
			if (pageCount > options.maximum_pages) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild exceeds its configured derived Vector page bound");
			}
			const size_t poolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
			std::vector<VectorPqMaterializationPoolRequest> pools;
			pools.reserve(poolCount);
			for (size_t poolOrdinal = 0U; poolOrdinal < poolCount; ++poolOrdinal) {
				const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
				pools.push_back({options.first_slot, options.last_slot,
								 std::min(pageCount - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
			}
			return pools;
		}

		[[nodiscard]] NativeGraphIndexTreeMutation planMappingTree(const BTreeInitialTreeMutationPlan &plan,
																   const OfflineVectorPqRebuildOptions &options) {
			NativeGraphIndexTreeMutation result;
			result.index_id = plan.index_id;
			result.new_pool_requests.reserve(plan.new_pool_mutations.size());
			size_t pageCount = 0U;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				if (pool.owner_id != plan.index_id || pool.appended_page_count == 0U ||
					pool.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
					pool.appended_page_count > options.mapping.tree.maximum_pages - pageCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline PQ rebuild mapping-tree plan exceeds its configured page bound");
				}
				pageCount += pool.appended_page_count;
				result.new_pool_requests.push_back({options.mapping.tree.first_slot, options.mapping.tree.last_slot,
													pool.owner_id, pool.appended_page_count});
			}
			if (result.new_pool_requests.empty() || plan.root_page.pool_ordinal >= result.new_pool_requests.size() ||
				plan.root_page.appended_page_ordinal >=
						result.new_pool_requests[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild mapping-tree plan does not identify a bounded root page");
			}
			result.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			return result;
		}

	} // namespace

	OfflineVectorPqRebuildResult OfflineVectorPqRebuild::run(BTreePageStore &trees, VectorPageStore &vectors,
															 VectorCodebookStore &codebooks,
															 VectorManifestStore &manifests,
															 RelocatableMetadataStore &metadata, uint64_t indexId,
															 std::span<const DeterministicPqTrainingRecord> records,
															 const OfflineVectorPqRebuildOptions &options) {
		validateOptions(indexId, records, options);
		const uint8_t differentStores = static_cast<uint8_t>(!trees.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!vectors.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!codebooks.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!manifests.usesMetadataStore(metadata));
		if (differentStores != 0U) {
			fail(FormatError::FERR_INVALID_STATE, "offline PQ rebuild requires one destination metadata store");
		}

		const DeterministicProductQuantizer quantizer = DeterministicProductQuantizer::train(records, options.training);
		const std::vector<std::byte> codebookPayload = quantizer.codebookPayload();
		const std::vector<PqVectorMaterializationRecord> encoded = encodeRecords(quantizer, records);
		MetadataSnapshotPin sourceSnapshot = metadata.openSnapshot();
		const Superblock sourceSuperblock = sourceSnapshot.superblock();
		const VectorManifest base = manifests.read(sourceSnapshot, indexId).manifest();
		verifyRawSource(sourceSnapshot, vectors, metadata, indexId, records);
		const PageLocation manifestLocation = metadata.findPageLocation(sourceSnapshot, base.page_id);
		const CatalogEntry manifestPool = metadata.findCatalog(sourceSnapshot, manifestLocation.segment_id);
		const VectorManifestLocation manifestDirectory = metadata.findVectorManifest(sourceSnapshot, indexId);
		const bool codebookExists =
				metadata.findExtentLocationOptional(sourceSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(
																			options.codebook.codebook_id))
						.has_value();
		std::optional<VectorCodebookPreparedWrite> codebookCandidate;
		if (codebookExists) {
			verifyCodebook(codebooks.readBounded(sourceSnapshot, options.codebook.codebook_id, codebookPayload.size(),
												 VectorCodebookStore::requiredDataExtentCount(codebookPayload.size())),
						   options.codebook, codebookPayload);
		} else {
			codebookCandidate = VectorCodebookStore::prepareImmutable(options.codebook, codebookPayload);
		}
		const size_t nonPqEntries =
				static_cast<size_t>(std::ranges::count_if(base.entries, [](const VectorManifestEntry &entry) {
					return entry.storage_kind != VectorStorageKind::VSK_PQ;
				}));
		if (nonPqEntries > RSS_VECTOR_MANIFEST_MAX_ENTRIES ||
			options.pages.maximum_pages > RSS_VECTOR_MANIFEST_MAX_ENTRIES - nonPqEntries) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline PQ rebuild page bound cannot fit within one immutable Vector Manifest");
		}

		OfflineVectorPqRebuildResult result;
		result.vector_count = records.size();
		result.reused_codebook = codebookExists;

		const std::optional<IndexRootLocation> existingRoot =
				metadata.findIndexRootOptional(sourceSnapshot, options.mapping_index_id);
		std::optional<BTreeTreeRetirementPlan> mappingRetirementPlan;
		std::vector<PageRetirement> pqRetirements;
		if (existingRoot.has_value()) {
			pqRetirements = verifyExistingPqGeneration(sourceSnapshot, trees, vectors, metadata, indexId,
													   options.mapping_index_id, base, encoded,
													   options.mapping.maximum_entries);
			const std::vector<VectorManifestEntry> mappingEntries =
					manifestEntriesFromMapping(sourceSnapshot, trees, vectors, metadata, base, options.mapping_index_id,
											   options.mapping.maximum_entries);
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(base.pq_mapping_index_id != options.mapping_index_id) |
					static_cast<uint8_t>(base.pq_mapping_root_page_id != existingRoot->root_page_id) |
					static_cast<uint8_t>(base.pq_mapping_root_generation != existingRoot->root_generation) |
					static_cast<uint8_t>(base.entries != mappingEntries);
			if (invalidCurrent != 0U) {
				sourceSnapshot.release();
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild visible Manifest disagrees with its PQ mapping root");
			}
			if (manifestMatches(base, indexId, options)) {
				verifyMapping(sourceSnapshot, trees, vectors, metadata, indexId, options.mapping_index_id,
							  options.codebook.generation, encoded, options.mapping.maximum_entries);
				sourceSnapshot.release();
				result.reused_mapping = true;
				result.manifest = base;
				result.superblock = sourceSuperblock;
				result.already_published = true;
				return result;
			}
			mappingRetirementPlan = trees.planTreeRetirement(sourceSnapshot, options.mapping_index_id,
															 options.mapping.tree.maximum_pages);
		} else {
			// Unanchored PQ inventory carries no reachable physical page identity and
			// is replaced by the first complete generation. A dangling root reference,
			// in contrast, would make the Manifest internally inconsistent.
			const bool hasDanglingMappingReference = base.pq_mapping_index_id != 0U ||
													 base.pq_mapping_root_page_id.isValid() ||
													 base.pq_mapping_root_generation.isValid();
			if (hasDanglingMappingReference) {
				sourceSnapshot.release();
				fail(FormatError::FERR_INVALID_STATE,
					 "offline PQ rebuild refuses a visible PQ Manifest with a dangling mapping-root reference");
			}
		}

		std::vector<BTreePageEntry> mappingEntries;
		mappingEntries.reserve(encoded.size());
		for (const PqVectorMaterializationRecord &record: encoded) {
			mappingEntries.push_back({VectorPqMapping::key(record.vector_id), 0U});
		}
		std::vector<VectorPqPageMapping> pqMappings;
		const BTreeInitialTreeValueBinder mappingValueBinder =
				[&pqMappings](std::span<BTreePageEntry> entries, const BTreeInitialTreeValueBindingContext &) {
					if (entries.size() != pqMappings.size()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "offline PQ rebuild mapping binder lost a canonical Vector ID");
					}
					for (size_t ordinal = 0U; ordinal < entries.size(); ++ordinal) {
						if (entries[ordinal].key != VectorPqMapping::key(pqMappings[ordinal].vector_id) ||
							!pqMappings[ordinal].page_id.isValid()) {
							fail(FormatError::FERR_INVALID_STATE,
								 "offline PQ rebuild mapping binder received a malformed PQ page mapping");
						}
						entries[ordinal].value_or_child = pqMappings[ordinal].page_id.value();
					}
				};
		const BTreeInitialTreeMutationPlan mappingPlan =
				mappingRetirementPlan.has_value()
						? trees.planReplacementTreeMutation(sourceSnapshot, options.mapping_index_id, mappingEntries,
															mappingValueBinder)
						: trees.planInitialTreeMutation(sourceSnapshot, options.mapping_index_id, mappingEntries,
														mappingValueBinder);
		const NativeGraphIndexTreeMutation mappingTree = planMappingTree(mappingPlan, options);
		const std::vector<VectorPqMaterializationPoolRequest> pqPools = planPqPools(encoded, options.pages);
		std::vector<CatalogSlotReservationRequest> codebookReservations;
		std::span<const ExtentLocation> codebookExtents;
		if (codebookCandidate.has_value()) {
			codebookReservations = codebookCandidate->catalogReservations();
			codebookExtents = codebookCandidate->extentLocations();
		}
		sourceSnapshot.release();

		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata);
		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = indexId;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = base.committed_lsn;
		mutation.codebook_first_slot = options.codebook_first_slot;
		mutation.codebook_last_slot = options.codebook_last_slot;
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		mutation.pq_pools = pqPools;
		mutation.pq_retirements = pqRetirements;
		if (mappingRetirementPlan.has_value()) {
			mutation.expected_mapping_root = mappingRetirementPlan->expected_root;
			mutation.mapping_retirement_pools = mappingRetirementPlan->pool_requests;
		}
		mutation.mapping_tree = mappingTree;
		result.superblock = metadata.publishOfflineVectorDerivedRebuild(
				mutation, checkpointLsn,
				[&](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
					if (codebookCandidate.has_value()) {
						codebooks.writePreparedImmutable(*codebookCandidate, materialization.codebook_extents);
					}
					const VectorPqMaterializationResult pq = vectors.preparePqNewPoolPages(
							indexId, encoded, options.pages, materialization.pq_pools, checkpointLsn);
					pqMappings = pq.mappings;
					if (!materialization.pq_retirement_pools.empty()) {
						vectors.prepareDerivedPqRetirements(materialization.pq_retirement_pools, checkpointLsn);
					}
					if (mappingRetirementPlan.has_value()) {
						mappingRetirementPlan->preparer(materialization.mapping_retirement_pools, checkpointLsn);
					}
					mappingPlan.preparer(materialization.mapping_tree_pools, checkpointLsn);
					VectorManifest candidate = base;
					candidate.codebook_id = options.codebook.codebook_id;
					candidate.codebook_generation = options.codebook.generation;
					candidate.pq_mapping_index_id = options.mapping_index_id;
					candidate.pq_mapping_root_page_id = materialization.mapping_root.root_page_id;
					candidate.pq_mapping_root_generation = materialization.mapping_root.root_generation;
					candidate.entries = manifestEntries(base, pq);
					candidate.committed_lsn = checkpointLsn;
					const PageOwnerHeader owner{candidate.page_id,
												candidate.index_id,
												PageOwnerKind::POK_VECTOR_MANIFEST,
												0U,
												RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												candidate.generation,
												checkpointLsn};
					const VectorManifestPage page = VectorManifestPage::create(owner, candidate);
					manifests.prepareReplacement(manifestPool, manifestLocation, base.committed_lsn, page,
												 checkpointLsn, materialization.manifest_replacement);
					result.manifest = std::move(candidate);
				},
				options.manifest_publish_options);
		return result;
	}

} // namespace graph::storage
