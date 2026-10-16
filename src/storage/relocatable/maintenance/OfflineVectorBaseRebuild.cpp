/**
 * @file OfflineVectorBaseRebuild.cpp
 * @brief Atomic base-generation rebuild for relocatable Vector indexes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkCancellation(const CancellationCheckpoint *cancellation) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline Vector base rebuild cannot advance the destination checkpoint LSN");
			}
			return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] std::vector<std::byte> rawRecord(uint64_t vectorId, std::span<const std::byte> rawBfloat16) {
			std::vector<std::byte> result(sizeof(uint64_t) + rawBfloat16.size());
			LittleEndianCodec::writeU64(result, 0U, vectorId);
			std::copy(rawBfloat16.begin(), rawBfloat16.end(), result.begin() + sizeof(uint64_t));
			return result;
		}

		[[nodiscard]] std::vector<std::byte> pqRecord(uint64_t vectorId, std::span<const uint8_t> codes) {
			std::vector<std::byte> result(sizeof(uint64_t) + codes.size());
			LittleEndianCodec::writeU64(result, 0U, vectorId);
			for (size_t ordinal = 0U; ordinal < codes.size(); ++ordinal) {
				result[sizeof(uint64_t) + ordinal] = static_cast<std::byte>(codes[ordinal]);
			}
			return result;
		}

		[[nodiscard]] uint64_t decodeVectorMappingKey(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline Vector base rebuild found a noncanonical Vector mapping key");
			}
			uint64_t vectorId = 0U;
			for (const std::byte byte: key) {
				vectorId = (vectorId << 8U) | std::to_integer<uint8_t>(byte);
			}
			if (vectorId == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild found a noncanonical Vector mapping key");
			}
			return vectorId;
		}

		void validateOptions(uint64_t indexId, std::span<const DeterministicPqTrainingRecord> records,
							 const OfflineVectorBaseRebuildOptions &options) {
			const OfflineVectorPqRebuildOptions &pq = options.pq;
			const OfflineVectorAdjacencyRebuildOptions &adjacency = options.adjacency;
			const uint8_t invalid =
					static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(records.empty()) |
					static_cast<uint8_t>(options.maximum_delta_mapping_pages == 0U) |
					static_cast<uint8_t>(options.maximum_delta_mapping_pages == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.maximum_delta_overlay_entries == 0U) |
					static_cast<uint8_t>(options.maximum_delta_overlay_entries == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(options.maximum_delta_overlay_entries >
									 VectorDeltaMapping::maximumEntriesForPageBudget(
												 options.maximum_delta_mapping_pages)) |
					static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(pq.codebook.codebook_id)) |
					static_cast<uint8_t>(!pq.codebook.manifest_segment_id.isValid()) |
					static_cast<uint8_t>(!pq.codebook.generation.isValid()) |
					static_cast<uint8_t>(pq.codebook.element_type != VectorCodebookElementType::VCET_FLOAT32) |
					static_cast<uint8_t>(pq.codebook.dimensions != pq.training.dimensions) |
					static_cast<uint8_t>(pq.codebook.subspace_count != pq.training.subspace_count) |
					static_cast<uint8_t>(pq.codebook.centroid_count != pq.training.centroid_count) |
					static_cast<uint8_t>(!pq.codebook_first_slot.isValid()) |
					static_cast<uint8_t>(!pq.codebook_last_slot.isValid()) |
					static_cast<uint8_t>(pq.codebook_first_slot > pq.codebook_last_slot) |
					static_cast<uint8_t>(pq.pages.codebook_generation != pq.codebook.generation) |
					static_cast<uint8_t>(pq.pages.maximum_records == 0U) |
					static_cast<uint8_t>(records.size() > pq.pages.maximum_records) |
					static_cast<uint8_t>(pq.pages.maximum_pages == 0U) |
					static_cast<uint8_t>(pq.mapping.maximum_entries == 0U) |
					static_cast<uint8_t>(pq.mapping.maximum_entries == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(records.size() > pq.mapping.maximum_entries) |
					static_cast<uint8_t>(pq.mapping.tree.maximum_entries != pq.mapping.maximum_entries) |
					static_cast<uint8_t>(pq.mapping.tree.pool_owner_id != pq.mapping_index_id) |
					static_cast<uint8_t>(!pq.mapping.tree.first_slot.isValid()) |
					static_cast<uint8_t>(!pq.mapping.tree.last_slot.isValid()) |
					static_cast<uint8_t>(pq.mapping.tree.first_slot > pq.mapping.tree.last_slot) |
					static_cast<uint8_t>(pq.mapping.tree.maximum_pages == 0U) |
					static_cast<uint8_t>(adjacency.pages.maximum_records == 0U) |
					static_cast<uint8_t>(records.size() > adjacency.pages.maximum_records) |
					static_cast<uint8_t>(adjacency.pages.maximum_pages == 0U) |
					static_cast<uint8_t>(!adjacency.pages.first_slot.isValid()) |
					static_cast<uint8_t>(!adjacency.pages.last_slot.isValid()) |
					static_cast<uint8_t>(adjacency.pages.first_slot > adjacency.pages.last_slot) |
					static_cast<uint8_t>(adjacency.mapping.maximum_entries == 0U) |
					static_cast<uint8_t>(adjacency.mapping.maximum_entries == std::numeric_limits<size_t>::max()) |
					static_cast<uint8_t>(records.size() > adjacency.mapping.maximum_entries) |
					static_cast<uint8_t>(adjacency.mapping.tree.maximum_entries != adjacency.mapping.maximum_entries) |
					static_cast<uint8_t>(adjacency.mapping.tree.pool_owner_id != adjacency.mapping_index_id) |
					static_cast<uint8_t>(!adjacency.mapping.tree.first_slot.isValid()) |
					static_cast<uint8_t>(!adjacency.mapping.tree.last_slot.isValid()) |
					static_cast<uint8_t>(adjacency.mapping.tree.first_slot > adjacency.mapping.tree.last_slot) |
					static_cast<uint8_t>(adjacency.mapping.tree.maximum_pages == 0U) |
					static_cast<uint8_t>(adjacency.adjacency_generation.value() == 0U) |
					static_cast<uint8_t>(pq.mapping_index_id == adjacency.mapping_index_id);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline Vector base rebuild requires bounded distinct PQ and adjacency plans");
			}
			VectorPqMapping::validateMappingIndexId(pq.mapping_index_id);
			VectorAdjacencyMapping::validateMappingIndexId(adjacency.mapping_index_id);
		}

		void verifyRawSource(const MetadataSnapshotPin &snapshot, VectorPageStore &vectors,
							 RelocatableMetadataStore &metadata, uint64_t indexId,
							 std::span<const DeterministicPqTrainingRecord> records,
							 const CancellationCheckpoint *cancellation) {
			for (const DeterministicPqTrainingRecord &record: records) {
				checkCancellation(cancellation);
				const VectorLocation location = metadata.findVectorLocation(snapshot, record.vector_id);
				const VectorPage page = vectors.read(snapshot, location.page_id);
				const uint8_t invalid = static_cast<uint8_t>(page.header().index_id != indexId) |
									static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_RAW) |
									static_cast<uint8_t>(page.read(location.slot_id, location.slot_generation) !=
															 rawRecord(record.vector_id, record.raw_bfloat16));
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild raw location disagrees with its canonical source input");
				}
			}
		}

		[[nodiscard]] std::vector<PqVectorMaterializationRecord>
		encodePq(const DeterministicProductQuantizer &quantizer,
				 std::span<const DeterministicPqTrainingRecord> records,
				 const CancellationCheckpoint *cancellation) {
			std::vector<PqVectorMaterializationRecord> result;
			result.reserve(records.size());
			for (const DeterministicPqTrainingRecord &record: records) {
				checkCancellation(cancellation);
				result.push_back({record.vector_id, quantizer.encode(record.raw_bfloat16, cancellation)});
			}
			return result;
		}

		[[nodiscard]] std::vector<VectorPqMaterializationPoolRequest>
		planPqPools(std::span<const PqVectorMaterializationRecord> records,
					const VectorPqMaterializationOptions &options) {
			// The top-level option and deterministic-PQ contracts establish a nonempty
			// fixed-width record stream. The Vector page store remains the canonical
			// validator for the persisted stride below.
			const uint32_t recordBytes = static_cast<uint32_t>(sizeof(uint64_t) + records.front().codes.size());
			const uint16_t slotsPerPage = VectorPageStore::rawSlotCapacity(recordBytes);
			const size_t pageCount = (records.size() - 1U) / slotsPerPage + 1U;
			if (pageCount > options.maximum_pages) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild exceeds its configured PQ page bound");
			}
			std::vector<VectorPqMaterializationPoolRequest> result;
			result.reserve((pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
			for (size_t firstPage = 0U; firstPage < pageCount; firstPage += RSS_PAGE_POOL_DATA_FRAME_COUNT) {
				result.push_back({options.first_slot, options.last_slot,
							  std::min(pageCount - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
			}
			return result;
		}

		[[nodiscard]] NativeGraphIndexTreeMutation planMappingTree(const BTreeInitialTreeMutationPlan &plan,
													 const BulkBTreeBuildOptions &bounds) {
			NativeGraphIndexTreeMutation result;
			result.index_id = plan.index_id;
			size_t pageCount = 0U;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				if (pool.appended_page_count > bounds.maximum_pages - pageCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild mapping-tree plan exceeds its configured page bound");
				}
				pageCount += pool.appended_page_count;
				result.new_pool_requests.push_back(
						{bounds.first_slot, bounds.last_slot, pool.owner_id, pool.appended_page_count});
			}
			// The fresh-tree planner creates this reference; the metadata publication
			// boundary validates it again while assigning its PageId.
			result.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			return result;
		}

		[[nodiscard]] std::vector<PageRetirement>
		verifyAndCollectPqRetirements(const MetadataSnapshotPin &snapshot, BTreePageStore &trees,
									 VectorPageStore &vectors, RelocatableMetadataStore &metadata,
									const VectorManifest &manifest, uint64_t indexId,
									std::span<const PqVectorMaterializationRecord> expected, size_t maximumEntries,
									bool requireExactCodes, const CancellationCheckpoint *cancellation) {
			const utils::OwnedStdVector<BTreePageEntry> mapping =
					BTreeIndex(manifest.pq_mapping_index_id, trees, metadata).scan(snapshot, std::nullopt, maximumEntries + 1U,
							BufferPoolReadHint::BPRH_SEQUENTIAL, cancellation);
			if (requireExactCodes && mapping.size() != expected.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild PQ mapping has a different canonical Vector ID set");
			}
			std::map<PageId, uint32_t> manifestPages;
			for (const VectorManifestEntry &entry: manifest.entries) {
				checkCancellation(cancellation);
				if (entry.storage_kind != VectorStorageKind::VSK_PQ) {
					continue;
				}
				// The canonical Manifest codec already establishes valid, sorted,
				// unique entries. This map is only the cross-structure inventory.
				manifestPages.emplace(entry.page_id, entry.record_count);
			}
			std::map<PageId, PageRetirement> pages;
			for (size_t ordinal = 0U; ordinal < mapping.size(); ++ordinal) {
				checkCancellation(cancellation);
				const BTreePageEntry &entry = mapping[ordinal];
				const uint64_t vectorId = decodeVectorMappingKey(entry.key);
				if (entry.value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild PQ mapping has an invalid Vector ID or target page");
				}
				if (requireExactCodes && entry.key != VectorPqMapping::key(expected[ordinal].vector_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild PQ mapping has a different canonical Vector ID set");
				}
				const PageId pageId(entry.value_or_child);
				const std::optional<DerivedVectorPqPage> observed = vectors.inspectDerivedPqPage(snapshot, pageId);
				const VectorPage page = vectors.read(snapshot, pageId);
				const std::optional<uint16_t> slot = page.findVectorId(vectorId);
				const uint8_t invalid = static_cast<uint8_t>(!observed.has_value()) |
									static_cast<uint8_t>(observed.has_value() && observed->index_id != indexId) |
									static_cast<uint8_t>(page.header().index_id != indexId) |
									static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_PQ) |
									static_cast<uint8_t>(page.header().codebook_generation != manifest.codebook_generation) |
									static_cast<uint8_t>(!slot.has_value()) |
									static_cast<uint8_t>(!manifestPages.contains(pageId)) |
									static_cast<uint8_t>(manifestPages.contains(pageId) &&
													 manifestPages.at(pageId) != page.header().live_count);
				if (invalid != 0U ||
					(requireExactCodes && page.read(*slot) != pqRecord(expected[ordinal].vector_id, expected[ordinal].codes))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild visible PQ mapping and Manifest inventory disagree");
				}
				pages.emplace(pageId, PageRetirement{observed->location, indexId, PageOwnerKind::POK_VECTOR_PQ,
															 observed->owner_lsn});
			}
			if (pages.size() != manifestPages.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild visible Manifest names a PQ page outside its mapping");
			}
			std::vector<PageRetirement> result;
			result.reserve(pages.size());
			for (const auto &[pageId, retirement]: pages) {
				static_cast<void>(pageId);
				result.push_back(retirement);
			}
			return result;
		}

		[[nodiscard]] std::vector<PageRetirement>
		verifyAndCollectAdjacencyRetirements(const MetadataSnapshotPin &snapshot, BTreePageStore &trees,
										VectorAdjacencyPageStore &adjacencies, RelocatableMetadataStore &metadata,
									 const VectorManifest &manifest, uint64_t indexId,
									 std::span<const VectorAdjacencyMaterializationRecord> expected,
									 size_t maximumEntries, bool requireExactRecords,
									 const CancellationCheckpoint *cancellation) {
			const utils::OwnedStdVector<BTreePageEntry> mapping = BTreeIndex(manifest.adjacency_mapping_index_id, trees, metadata)
					.scan(snapshot, std::nullopt, maximumEntries + 1U, BufferPoolReadHint::BPRH_SEQUENTIAL, cancellation);
			if (requireExactRecords && mapping.size() != expected.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild adjacency mapping has a different canonical Vector ID set");
			}
			std::map<PageId, PageRetirement> pages;
			for (size_t ordinal = 0U; ordinal < mapping.size(); ++ordinal) {
				checkCancellation(cancellation);
				const BTreePageEntry &entry = mapping[ordinal];
				const uint64_t vectorId = decodeVectorMappingKey(entry.key);
				if (entry.value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild adjacency mapping has an invalid Vector ID or target page");
				}
				if (requireExactRecords && entry.key != VectorAdjacencyMapping::key(expected[ordinal].vector_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild adjacency mapping has a different canonical Vector ID set");
				}
				const PageId pageId(entry.value_or_child);
				const std::optional<DerivedVectorAdjacencyPage> observed = adjacencies.inspectDerivedPage(snapshot, pageId);
				if (!observed.has_value() || observed->index_id != indexId) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild adjacency mapping resolves outside its derived page namespace");
				}
				const VectorAdjacencyRecord observedRecord = adjacencies.readRecord(snapshot, pageId, vectorId);
				const uint8_t invalid = static_cast<uint8_t>(observedRecord.vector_id != vectorId) |
									static_cast<uint8_t>(requireExactRecords &&
												observedRecord != VectorAdjacencyRecord{expected[ordinal].vector_id,
																							 expected[ordinal].neighbors});
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline Vector base rebuild adjacency mapping resolves to an invalid record");
				}
				pages.emplace(pageId,
							  PageRetirement{observed->location, VectorAdjacencyPageStore::derivedOwnerId(indexId),
														 PageOwnerKind::POK_VECTOR_ADJACENCY, observed->owner_lsn});
			}
			std::vector<PageRetirement> result;
			result.reserve(pages.size());
			for (const auto &[pageId, retirement]: pages) {
				static_cast<void>(pageId);
				result.push_back(retirement);
			}
			return result;
		}

		[[nodiscard]] std::vector<VectorManifestEntry>
		manifestEntries(const VectorManifest &base, const VectorPqMaterializationResult &pq) {
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

		void verifyCodebook(const VectorCodebook &actual, const VectorCodebookWriteRequest &expected,
							std::span<const std::byte> payload) {
			const uint8_t invalid = static_cast<uint8_t>(actual.header.codebook_id != expected.codebook_id) |
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
					 "offline Vector base rebuild existing Codebook disagrees with deterministic training");
			}
		}

		[[nodiscard]] bool isAlreadyPublishedBase(const VectorManifest &manifest,
																 const OfflineVectorBaseRebuildOptions &options,
																 bool codebookExists,
																 const std::optional<IndexRootLocation> &pqRoot,
																 const std::optional<IndexRootLocation> &adjacencyRoot,
																 bool hasDeltaId,
																 const OfflineVectorAdjacencyPreparedGraph &adjacency) {
			const IndexRootLocation observedPqRoot = pqRoot.value_or(IndexRootLocation{});
			const IndexRootLocation observedAdjacencyRoot = adjacencyRoot.value_or(IndexRootLocation{});
			return std::tuple{codebookExists,
							  pqRoot.has_value(),
							  adjacencyRoot.has_value(),
							  hasDeltaId,
							  manifest.delta_mapping_root_page_id.isValid(),
							  manifest.delta_mapping_root_generation.isValid(),
							  manifest.delta_entry_count,
							  manifest.codebook_id,
							  manifest.codebook_generation,
							  manifest.pq_mapping_index_id,
							  manifest.pq_mapping_root_page_id,
							  manifest.pq_mapping_root_generation,
							  manifest.adjacency_generation,
							  manifest.entry_point_vector_id,
							  manifest.adjacency_mapping_index_id,
							  manifest.adjacency_mapping_root_page_id,
							  manifest.adjacency_mapping_root_generation} ==
				   std::tuple{true,
							  true,
							  true,
							  false,
							  false,
							  false,
							  0U,
							  options.pq.codebook.codebook_id,
							  options.pq.codebook.generation,
							  options.pq.mapping_index_id,
							  observedPqRoot.root_page_id,
							  observedPqRoot.root_generation,
							  options.adjacency.adjacency_generation,
							  adjacency.records.front().vector_id,
							  options.adjacency.mapping_index_id,
							  observedAdjacencyRoot.root_page_id,
							  observedAdjacencyRoot.root_generation};
		}

	} // namespace

	OfflineVectorBaseRebuildResult OfflineVectorBaseRebuild::run(
			BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
			VectorCodebookStore &codebooks, VectorManifestStore &manifests, RelocatableMetadataStore &metadata,
			uint64_t indexId, std::span<const DeterministicPqTrainingRecord> records,
			const OfflineVectorBaseRebuildOptions &options, const CancellationCheckpoint *cancellation) {
		validateOptions(indexId, records, options);
		checkCancellation(cancellation);
		const uint8_t differentStores = static_cast<uint8_t>(!trees.usesMetadataStore(metadata)) |
									static_cast<uint8_t>(!vectors.usesMetadataStore(metadata)) |
									static_cast<uint8_t>(!adjacencies.usesMetadataStore(metadata)) |
									static_cast<uint8_t>(!codebooks.usesMetadataStore(metadata)) |
									static_cast<uint8_t>(!manifests.usesMetadataStore(metadata));
		if (differentStores != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "offline Vector base rebuild requires one destination metadata store");
		}

		const DeterministicProductQuantizer quantizer =
				DeterministicProductQuantizer::train(records, options.pq.training, cancellation);
		const std::vector<std::byte> codebookPayload = quantizer.codebookPayload(cancellation);
		const std::vector<PqVectorMaterializationRecord> pqRecords = encodePq(quantizer, records, cancellation);
		std::vector<CanonicalVectorRecord> adjacencyInput;
		adjacencyInput.reserve(records.size());
		for (const DeterministicPqTrainingRecord &record: records) {
			checkCancellation(cancellation);
			adjacencyInput.push_back({record.vector_id, record.raw_bfloat16});
		}
		MetadataSnapshotPin source = metadata.openSnapshot();
		const VectorManifest base = manifests.read(source, indexId).manifest();
		if (base.index_id != indexId || base.dimensions != options.pq.training.dimensions || !base.page_id.isValid()) {
			source.release();
			fail(FormatError::FERR_INVALID_STATE,
				 "offline Vector base rebuild source Manifest is not compatible with the requested base generation");
		}
		const OfflineVectorAdjacencyPreparedGraph adjacencyGraph =
				OfflineVectorAdjacencyRebuild::buildGraph(base.metric, options.pq.training.dimensions, adjacencyInput,
																 options.adjacency, cancellation);
		const size_t nonPqEntries =
				static_cast<size_t>(std::ranges::count_if(base.entries, [](const VectorManifestEntry &entry) {
					return entry.storage_kind != VectorStorageKind::VSK_PQ;
				}));
		if (nonPqEntries > RSS_VECTOR_MANIFEST_MAX_ENTRIES ||
			options.pq.pages.maximum_pages > RSS_VECTOR_MANIFEST_MAX_ENTRIES - nonPqEntries) {
			source.release();
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline Vector base rebuild page bound cannot fit in one immutable Vector Manifest");
		}
		verifyRawSource(source, vectors, metadata, indexId, records, cancellation);
		const PageLocation manifestLocation = metadata.findPageLocation(source, base.page_id);
		const CatalogEntry manifestPool = metadata.findCatalog(source, manifestLocation.segment_id);
		const VectorManifestLocation manifestDirectory = metadata.findVectorManifest(source, indexId);

		const bool codebookExists = metadata
				.findExtentLocationOptional(source, VectorCodebookExtentIdCodec::manifestExtentId(options.pq.codebook.codebook_id))
				.has_value();
		std::optional<VectorCodebookPreparedWrite> codebookCandidate;
		if (codebookExists) {
			verifyCodebook(codebooks.readBounded(source, options.pq.codebook.codebook_id, codebookPayload.size(),
														VectorCodebookStore::requiredDataExtentCount(codebookPayload.size())),
						   options.pq.codebook, codebookPayload);
		} else {
			codebookCandidate = VectorCodebookStore::prepareImmutable(options.pq.codebook, codebookPayload);
		}

		const std::optional<IndexRootLocation> currentPqRoot =
				metadata.findIndexRootOptional(source, options.pq.mapping_index_id);
		const std::optional<IndexRootLocation> currentAdjacencyRoot =
				metadata.findIndexRootOptional(source, options.adjacency.mapping_index_id);
		const std::optional<IndexRootLocation> currentDeltaRoot =
				base.delta_mapping_index_id == 0U ? std::nullopt
																		 : metadata.findIndexRootOptional(source, base.delta_mapping_index_id);
		const bool hasDeltaId = base.delta_mapping_index_id != 0U;
		const bool hasDeltaPage = base.delta_mapping_root_page_id.isValid();
		const bool hasDeltaGeneration = base.delta_mapping_root_generation.isValid();
		const uint8_t partialDelta = static_cast<uint8_t>(hasDeltaId != hasDeltaPage) |
										 static_cast<uint8_t>(hasDeltaId != hasDeltaGeneration);
		if (partialDelta != 0U || (hasDeltaId && base.delta_entry_count == 0U) ||
			(hasDeltaId && !currentDeltaRoot.has_value()) ||
			(currentDeltaRoot.has_value() &&
			 (currentDeltaRoot->root_page_id != base.delta_mapping_root_page_id ||
			  currentDeltaRoot->root_generation != base.delta_mapping_root_generation))) {
			source.release();
			fail(FormatError::FERR_INVALID_STATE,
				 "offline Vector base rebuild found a partial or stale delta mapping root");
		}
		const bool alreadyPublished = isAlreadyPublishedBase(base, options, codebookExists, currentPqRoot,
															 currentAdjacencyRoot, hasDeltaId, adjacencyGraph);
		if (alreadyPublished) {
			static_cast<void>(verifyAndCollectPqRetirements(source, trees, vectors, metadata, base, indexId, pqRecords,
															 options.pq.mapping.maximum_entries, true, cancellation));
			static_cast<void>(verifyAndCollectAdjacencyRetirements(
					source, trees, adjacencies, metadata, base, indexId, adjacencyGraph.records,
					options.adjacency.mapping.maximum_entries, true, cancellation));
			OfflineVectorBaseRebuildResult result;
			result.superblock = source.superblock();
			result.manifest = base;
			result.vector_count = records.size();
			result.adjacency_build_work = adjacencyGraph.build_work;
			result.reused_codebook = true;
			result.already_published = true;
			source.release();
			return result;
		}

		std::optional<BTreeTreeRetirementPlan> pqRetirementPlan;
		std::vector<PageRetirement> pqRetirements;
		if (currentPqRoot.has_value()) {
			if (base.pq_mapping_index_id != options.pq.mapping_index_id ||
				base.pq_mapping_root_page_id != currentPqRoot->root_page_id ||
				base.pq_mapping_root_generation != currentPqRoot->root_generation) {
				source.release();
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild PQ Manifest and mapping root disagree");
			}
			pqRetirements = verifyAndCollectPqRetirements(source, trees, vectors, metadata, base, indexId, pqRecords,
															 options.pq.mapping.maximum_entries, false, cancellation);
			pqRetirementPlan = trees.planTreeRetirement(source, options.pq.mapping_index_id,
															options.pq.mapping.tree.maximum_pages);
		} else if (base.pq_mapping_index_id != 0U) {
			source.release();
			fail(FormatError::FERR_INVALID_STATE,
				 "offline Vector base rebuild found a dangling PQ mapping reference");
		}

		std::optional<BTreeTreeRetirementPlan> adjacencyRetirementPlan;
		std::vector<PageRetirement> adjacencyRetirements;
		if (currentAdjacencyRoot.has_value()) {
			if (base.adjacency_mapping_index_id != options.adjacency.mapping_index_id ||
				base.adjacency_mapping_root_page_id != currentAdjacencyRoot->root_page_id ||
				base.adjacency_mapping_root_generation != currentAdjacencyRoot->root_generation) {
				source.release();
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild adjacency Manifest is stale or does not advance its generation");
			}
			adjacencyRetirements = verifyAndCollectAdjacencyRetirements(
					source, trees, adjacencies, metadata, base, indexId, adjacencyGraph.records,
					options.adjacency.mapping.maximum_entries, false, cancellation);
			adjacencyRetirementPlan = trees.planTreeRetirement(source, options.adjacency.mapping_index_id,
																	 options.adjacency.mapping.tree.maximum_pages);
		} else if (base.adjacency_mapping_index_id != 0U) {
			source.release();
			fail(FormatError::FERR_INVALID_STATE,
				 "offline Vector base rebuild found a dangling adjacency mapping reference");
		}
		if (currentAdjacencyRoot.has_value() && options.adjacency.adjacency_generation <= base.adjacency_generation) {
			source.release();
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline Vector base rebuild must advance the visible adjacency generation");
		}

		std::optional<BTreeTreeRetirementPlan> deltaRetirementPlan;
		if (currentDeltaRoot.has_value()) {
			VectorDeltaMapping::validateMappingIndexId(base.delta_mapping_index_id);
			const auto deltaEntries = VectorDeltaMapping::scanOwned(
					source, trees, metadata, base.delta_mapping_index_id, options.maximum_delta_overlay_entries + 1U,
					cancellation);
			if (deltaEntries.size() != base.delta_entry_count ||
				deltaEntries.size() > options.maximum_delta_overlay_entries) {
				source.release();
				fail(FormatError::FERR_INVALID_STATE,
					 "offline Vector base rebuild delta inventory disagrees with its Manifest bound");
			}
			deltaRetirementPlan =
					trees.planTreeRetirement(source, base.delta_mapping_index_id, options.maximum_delta_mapping_pages);
		}

		utils::OwnedStdVector<BTreePageEntry> pqEntries;
		pqEntries.reserve(pqRecords.size());
		for (const PqVectorMaterializationRecord &record: pqRecords) {
			checkCancellation(cancellation);
			pqEntries.push_back({VectorPqMapping::key(record.vector_id), 0U});
		}
		std::vector<VectorPqPageMapping> pqMappings;
		const BTreeInitialTreeValueBinder pqBinder =
				[&pqMappings](std::span<BTreePageEntry> entries, const BTreeInitialTreeValueBindingContext &) {
					// The materializer returns one ordered valid PageId mapping per
					// canonical input record. Both streams originate from `pqRecords`.
					for (size_t ordinal = 0U; ordinal < entries.size(); ++ordinal) {
						entries[ordinal].value_or_child = pqMappings[ordinal].page_id.value();
					}
				};
		const BTreeInitialTreeMutationPlan pqPlan = currentPqRoot.has_value()
					? trees.planReplacementTreeMutation(source, options.pq.mapping_index_id, pqEntries, pqBinder)
					: trees.planInitialTreeMutation(source, options.pq.mapping_index_id, pqEntries, pqBinder);

		utils::OwnedStdVector<BTreePageEntry> adjacencyEntries;
		adjacencyEntries.reserve(adjacencyGraph.records.size());
		for (const VectorAdjacencyMaterializationRecord &record: adjacencyGraph.records) {
			checkCancellation(cancellation);
			adjacencyEntries.push_back({VectorAdjacencyMapping::key(record.vector_id), 0U});
		}
		std::vector<VectorAdjacencyPageMapping> adjacencyMappings;
		const BTreeInitialTreeValueBinder adjacencyBinder =
				[&adjacencyMappings](std::span<BTreePageEntry> entries, const BTreeInitialTreeValueBindingContext &) {
					// The adjacency materializer provides the equivalent ordered mapping
					// for the canonical graph records that built this fresh tree.
					for (size_t ordinal = 0U; ordinal < entries.size(); ++ordinal) {
						entries[ordinal].value_or_child = adjacencyMappings[ordinal].page_id.value();
					}
				};
		const BTreeInitialTreeMutationPlan adjacencyPlan = currentAdjacencyRoot.has_value()
					? trees.planReplacementTreeMutation(source, options.adjacency.mapping_index_id, adjacencyEntries,
																adjacencyBinder)
					: trees.planInitialTreeMutation(source, options.adjacency.mapping_index_id, adjacencyEntries,
														adjacencyBinder);

		const NativeGraphIndexTreeMutation pqTree = planMappingTree(pqPlan, options.pq.mapping.tree);
		const NativeGraphIndexTreeMutation adjacencyTree = planMappingTree(adjacencyPlan, options.adjacency.mapping.tree);
		const std::vector<VectorPqMaterializationPoolRequest> pqPools = planPqPools(pqRecords, options.pq.pages);
		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata);
		const std::vector<VectorAdjacencyMaterializationPoolRequest> adjacencyPools =
				VectorAdjacencyPageStore::planNewPoolRequests(indexId, adjacencyGraph.records, options.adjacency.pages,
																	 checkpointLsn);
		std::vector<CatalogSlotReservationRequest> codebookReservations;
		std::span<const ExtentLocation> codebookExtents;
		if (codebookCandidate.has_value()) {
			checkCancellation(cancellation);
			codebookReservations = codebookCandidate->catalogReservations();
			codebookExtents = codebookCandidate->extentLocations();
		}
		source.release();

		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = indexId;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = base.committed_lsn;
		mutation.codebook_first_slot = options.pq.codebook_first_slot;
		mutation.codebook_last_slot = options.pq.codebook_last_slot;
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		mutation.pq_pools = pqPools;
		mutation.pq_retirements = pqRetirements;
		if (pqRetirementPlan.has_value()) {
			mutation.expected_mapping_root = pqRetirementPlan->expected_root;
			mutation.mapping_retirement_pools = pqRetirementPlan->pool_requests;
		}
		mutation.mapping_tree = pqTree;
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_retirements = adjacencyRetirements;
		if (adjacencyRetirementPlan.has_value()) {
			mutation.expected_adjacency_mapping_root = adjacencyRetirementPlan->expected_root;
			mutation.adjacency_mapping_retirement_pools = adjacencyRetirementPlan->pool_requests;
		}
		mutation.adjacency_mapping_tree = adjacencyTree;
		if (deltaRetirementPlan.has_value()) {
			mutation.expected_delta_mapping_root = deltaRetirementPlan->expected_root;
			mutation.delta_mapping_retirement_pools = deltaRetirementPlan->pool_requests;
		}

		OfflineVectorBaseRebuildResult result;
		result.vector_count = records.size();
		result.adjacency_build_work = adjacencyGraph.build_work;
		result.reused_codebook = codebookExists;
		// All cancellation checkpoints above this call run before its atomic
		// publication boundary. Do not check after it returns: it may have made the
		// complete new derived generation durable and visible.
		checkCancellation(cancellation);
		result.superblock = metadata.publishOfflineVectorDerivedRebuild(
				mutation, checkpointLsn,
				[&](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
					checkCancellation(cancellation);
					if (codebookCandidate.has_value()) {
						checkCancellation(cancellation);
						codebooks.writePreparedImmutable(*codebookCandidate, materialization.codebook_extents);
					}
					checkCancellation(cancellation);
					const VectorPqMaterializationResult pq = vectors.preparePqNewPoolPages(
							indexId, pqRecords, options.pq.pages, materialization.pq_pools, checkpointLsn, cancellation);
					pqMappings = pq.mappings;
					checkCancellation(cancellation);
					const VectorAdjacencyMaterializationResult adjacency = adjacencies.prepareNewPoolPages(
							indexId, adjacencyGraph.records, options.adjacency.pages, materialization.adjacency_pools,
							checkpointLsn, cancellation);
					adjacencyMappings = adjacency.mappings;
					if (!materialization.pq_retirement_pools.empty()) {
						checkCancellation(cancellation);
						vectors.prepareDerivedPqRetirements(materialization.pq_retirement_pools, checkpointLsn, cancellation);
					}
					if (!materialization.adjacency_retirement_pools.empty()) {
						checkCancellation(cancellation);
						adjacencies.prepareDerivedRetirements(materialization.adjacency_retirement_pools, checkpointLsn,
															cancellation);
					}
					if (pqRetirementPlan.has_value()) {
						checkCancellation(cancellation);
						pqRetirementPlan->preparer(materialization.mapping_retirement_pools, checkpointLsn);
					}
					if (adjacencyRetirementPlan.has_value()) {
						checkCancellation(cancellation);
						adjacencyRetirementPlan->preparer(materialization.adjacency_mapping_retirement_pools, checkpointLsn);
					}
					if (deltaRetirementPlan.has_value()) {
						checkCancellation(cancellation);
						deltaRetirementPlan->preparer(materialization.delta_mapping_retirement_pools, checkpointLsn);
					}
					checkCancellation(cancellation);
					pqPlan.preparer(materialization.mapping_tree_pools, checkpointLsn);
					checkCancellation(cancellation);
					adjacencyPlan.preparer(materialization.adjacency_mapping_tree_pools, checkpointLsn);

					VectorManifest candidate = base;
					candidate.codebook_id = options.pq.codebook.codebook_id;
					candidate.codebook_generation = options.pq.codebook.generation;
					candidate.pq_mapping_index_id = options.pq.mapping_index_id;
					candidate.pq_mapping_root_page_id = materialization.mapping_root.root_page_id;
					candidate.pq_mapping_root_generation = materialization.mapping_root.root_generation;
					candidate.entry_point_vector_id = adjacencyGraph.records.front().vector_id;
					candidate.adjacency_generation = options.adjacency.adjacency_generation;
					candidate.adjacency_mapping_index_id = options.adjacency.mapping_index_id;
					candidate.adjacency_mapping_root_page_id = materialization.adjacency_mapping_root.root_page_id;
					candidate.adjacency_mapping_root_generation =
							materialization.adjacency_mapping_root.root_generation;
					candidate.delta_mapping_index_id = 0U;
					candidate.delta_mapping_root_page_id = {};
					candidate.delta_mapping_root_generation = {};
					candidate.delta_entry_count = 0U;
					checkCancellation(cancellation);
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
				options.pq.manifest_publish_options);
		return result;
	}

} // namespace graph::storage
