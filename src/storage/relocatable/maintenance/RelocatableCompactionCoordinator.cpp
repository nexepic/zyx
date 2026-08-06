/**
 * @file RelocatableCompactionCoordinator.cpp
 * @brief Bounded, snapshot-safe relocatable slotted-pool evacuation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwCompactionError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void requireCompaction(bool condition, FormatError error, const char *message) {
			if (!condition) {
				throwCompactionError(error, message);
			}
		}

		[[nodiscard]] bool supportsSlottedEvacuation(SegmentKind kind) {
			return kind == SegmentKind::SGK_PROPERTY_PAGE_POOL || kind == SegmentKind::SGK_STATE_PAGE_POOL ||
				   kind == SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
		}

		[[nodiscard]] bool supportsVectorEvacuation(SegmentKind kind) {
			const uint8_t supported = static_cast<uint8_t>(kind == SegmentKind::SGK_VECTOR_RAW_POOL) |
									  static_cast<uint8_t>(kind == SegmentKind::SGK_VECTOR_PQ_POOL);
			return supported != 0U;
		}

		void validateCompactionRequest(const RelocatableSlottedPoolCompactionRequest &request, const char *message) {
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!request.source_segment_id.isValid()) |
					static_cast<uint8_t>(!request.destination_segment_id.isValid()) |
					static_cast<uint8_t>(request.source_segment_id == request.destination_segment_id) |
					static_cast<uint8_t>(request.page_budget == 0U) |
					static_cast<uint8_t>(!request.first_lsn.isValid());
			requireCompaction(invalidRequest == 0U, FormatError::FERR_INVALID_HEADER, message);
		}

		void validateCurrentPool(const RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
								 const CatalogEntry &entry, SegmentKind expectedKind) {
			const uint8_t invalid =
					static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(entry.kind != expectedKind) |
					static_cast<uint8_t>(!metadata.isPhysicalSlotAllocated(snapshot, entry.physical_slot));
			requireCompaction(invalid == 0U, FormatError::FERR_INVALID_STATE,
							  "relocatable slotted-pool compaction requires current allocated compatible pools");
		}

		class LsnCursor final {
		public:
			explicit LsnCursor(LogSequenceNumber first) : next_(first.value()) {}

			[[nodiscard]] LogSequenceNumber consume() {
				requireCompaction(next_ != std::numeric_limits<uint64_t>::max(), FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable slotted-pool compaction exhausted its persistent LSN space");
				return LogSequenceNumber(next_++);
			}

			[[nodiscard]] LogSequenceNumber next() const { return LogSequenceNumber(next_); }

		private:
			uint64_t next_ = 0;
		};

		struct TypedPoolCompactionMessages {
			const char *invalid_request = nullptr;
			const char *unsupported_pool = nullptr;
			const char *lsn_overflow = nullptr;
			const char *stale_plan = nullptr;
		};

		using TypedPageEvacuator = std::function<void(MetadataSnapshotPin &, PageId, SegmentId, LogSequenceNumber,
													  const MetadataPublishOptions &)>;
		using RetiredPoolReleaser =
				std::function<bool(SegmentId, Generation, LogSequenceNumber, const MetadataPublishOptions &)>;

		struct TypedPoolCompactionOperations {
			std::function<bool(SegmentKind)> supports_pool;
			TypedPageEvacuator evacuate_page;
		};

		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactTypedPool(SlottedPagePool &pages, RelocatableMetadataStore &metadata,
						 const RelocatableSlottedPoolCompactionRequest &request,
						 const TypedPoolCompactionOperations &operations, const RetiredPoolReleaser &releasePool,
						 const TypedPoolCompactionMessages &messages, const MetadataPublishOptions &options) {
			validateCompactionRequest(request, messages.invalid_request);
			MetadataSnapshotPin planningSnapshot = metadata.openSnapshot();
			const CatalogEntry source = metadata.findCatalog(planningSnapshot, request.source_segment_id);
			const CatalogEntry destination = metadata.findCatalog(planningSnapshot, request.destination_segment_id);
			requireCompaction(operations.supports_pool(source.kind), FormatError::FERR_INVALID_STATE,
							  messages.unsupported_pool);
			validateCurrentPool(metadata, planningSnapshot, source, source.kind);
			validateCurrentPool(metadata, planningSnapshot, destination, source.kind);
			std::vector<PageLocation> sourcePages =
					metadata.findPageLocationsForSegment(planningSnapshot, request.source_segment_id);
			std::ranges::sort(sourcePages, {}, &PageLocation::page_id);
			const uint32_t moveCount =
					std::min<uint32_t>(request.page_budget, static_cast<uint32_t>(sourcePages.size()));
			if (pages.availableFrameCount(planningSnapshot, request.destination_segment_id) < moveCount) {
				planningSnapshot.release();
				return {RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY,
						0,
						sourcePages.empty(),
						false,
						false,
						request.first_lsn};
			}
			const uint64_t remaining = std::numeric_limits<uint64_t>::max() - request.first_lsn.value();
			requireCompaction(static_cast<uint64_t>(moveCount) + 2U <= remaining, FormatError::FERR_ARITHMETIC_OVERFLOW,
							  messages.lsn_overflow);
			planningSnapshot.release();

			LsnCursor lsns(request.first_lsn);
			RelocatableSlottedPoolCompactionResult result;
			for (uint32_t index = 0; index < moveCount; ++index) {
				const PageLocation expected = sourcePages[index];
				MetadataSnapshotPin snapshot = metadata.openSnapshot();
				const CatalogEntry currentSource = metadata.findCatalog(snapshot, request.source_segment_id);
				const CatalogEntry currentDestination = metadata.findCatalog(snapshot, request.destination_segment_id);
				validateCurrentPool(metadata, snapshot, currentSource, source.kind);
				validateCurrentPool(metadata, snapshot, currentDestination, source.kind);
				requireCompaction(metadata.findPageLocation(snapshot, expected.page_id) == expected,
								  FormatError::FERR_INVALID_STATE, messages.stale_plan);
				operations.evacuate_page(snapshot, expected.page_id, request.destination_segment_id, lsns.consume(),
										 options);
				++result.moved_pages;
			}

			MetadataSnapshotPin completionSnapshot = metadata.openSnapshot();
			const CatalogEntry currentSource = metadata.findCatalog(completionSnapshot, request.source_segment_id);
			result.source_empty =
					metadata.findPageLocationsForSegment(completionSnapshot, request.source_segment_id).empty();
			completionSnapshot.release();
			if (!result.source_empty) {
				result.next_lsn = lsns.next();
				return result;
			}

			pages.reclaimRetiredFrames(request.source_segment_id);
			static_cast<void>(pages.retireEmptyPool(request.source_segment_id, currentSource.generation, lsns.consume(),
													options));
			result.source_retired = true;
			if (releasePool(request.source_segment_id, currentSource.generation, lsns.next(), options)) {
				static_cast<void>(lsns.consume());
				result.source_released = true;
			} else {
				result.status = RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS;
			}
			result.next_lsn = lsns.next();
			return result;
		}

	} // namespace

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires a page pool bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, EntityPageStore &entities,
															   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), entity_pages_(&entities) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!entity_pages_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires Entity pages bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, BTreePageStore &btreePages,
														   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), btree_pages_(&btreePages) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!btree_pages_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires B+Tree pages bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorPageStore &vectorPages,
														   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), vector_pages_(&vectorPages) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!vector_pages_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires Vector pages bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorManifestStore &vectorManifests,
																					   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), vector_manifests_(&vectorManifests) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!vector_manifests_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires Vector Manifests bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, LargeValueStore &largeValues,
																					   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), large_values_(&largeValues) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!large_values_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument(
					"relocatable compaction coordinator requires LargeValue pages bound to its metadata store");
		}
	}

	RelocatableCompactionCoordinator::RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorCodebookStore &codebooks,
														   PhysicalSegmentAllocator &physical,
														   RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata), physical_(&physical), codebooks_(&codebooks) {
		const uint8_t mismatch = static_cast<uint8_t>(!pages_.usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!physical_->usesMetadataStore(metadata_)) |
								 static_cast<uint8_t>(!codebooks_->usesMetadataStore(metadata_));
		if (mismatch != 0U) {
			throw std::invalid_argument("relocatable compaction coordinator requires Codebooks bound to its metadata store");
		}
	}


	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactSlottedPool(const RelocatableSlottedPoolCompactionRequest &request,
												   const MetadataPublishOptions &options) {
		validateCompactionRequest(
				request, "relocatable slotted-pool compaction requires distinct pools, a bounded page budget, and an LSN");

		MetadataSnapshotPin planningSnapshot = metadata_.openSnapshot();
		const CatalogEntry source = metadata_.findCatalog(planningSnapshot, request.source_segment_id);
		const CatalogEntry destination = metadata_.findCatalog(planningSnapshot, request.destination_segment_id);
		requireCompaction(supportsSlottedEvacuation(source.kind), FormatError::FERR_INVALID_STATE,
						  "relocatable slotted-pool compaction does not accept a page format with an additional page header");
		validateCurrentPool(metadata_, planningSnapshot, source, source.kind);
		validateCurrentPool(metadata_, planningSnapshot, destination, source.kind);
		std::vector<PageLocation> sourcePages =
				metadata_.findPageLocationsForSegment(planningSnapshot, request.source_segment_id);
		std::ranges::sort(sourcePages, {}, &PageLocation::page_id);
		const uint32_t moveCount = std::min<uint32_t>(request.page_budget, static_cast<uint32_t>(sourcePages.size()));
		if (pages_.availableFrameCount(planningSnapshot, request.destination_segment_id) < moveCount) {
			planningSnapshot.release();
			return {RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY,
					0,
					sourcePages.empty(),
					false,
					false,
					request.first_lsn};
		}
		const uint64_t remaining = std::numeric_limits<uint64_t>::max() - request.first_lsn.value();
		requireCompaction(static_cast<uint64_t>(moveCount) + 2U <= remaining, FormatError::FERR_ARITHMETIC_OVERFLOW,
						  "relocatable slotted-pool compaction cannot reserve a complete LSN publication sequence");
		planningSnapshot.release();

		LsnCursor lsns(request.first_lsn);
		RelocatableSlottedPoolCompactionResult result;
		for (uint32_t index = 0; index < moveCount; ++index) {
			const PageLocation expected = sourcePages[index];
			MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			const CatalogEntry currentSource = metadata_.findCatalog(snapshot, request.source_segment_id);
			const CatalogEntry currentDestination = metadata_.findCatalog(snapshot, request.destination_segment_id);
			validateCurrentPool(metadata_, snapshot, currentSource, source.kind);
			validateCurrentPool(metadata_, snapshot, currentDestination, source.kind);
			requireCompaction(metadata_.findPageLocation(snapshot, expected.page_id) == expected,
							  FormatError::FERR_INVALID_STATE,
							  "relocatable slotted-pool compaction observed a page move while planning evacuation");
			SlottedPage page = pages_.read(snapshot, expected.page_id);
			snapshot.release();
			static_cast<void>(pages_.evacuatePage(page, request.destination_segment_id, lsns.consume(), options));
			++result.moved_pages;
		}

		MetadataSnapshotPin completionSnapshot = metadata_.openSnapshot();
		const CatalogEntry currentSource = metadata_.findCatalog(completionSnapshot, request.source_segment_id);
		result.source_empty =
				metadata_.findPageLocationsForSegment(completionSnapshot, request.source_segment_id).empty();
		completionSnapshot.release();
		if (!result.source_empty) {
			result.next_lsn = lsns.next();
			return result;
		}

		pages_.reclaimRetiredFrames(request.source_segment_id);
		static_cast<void>(
				pages_.retireEmptyPool(request.source_segment_id, currentSource.generation, lsns.consume(), options));
		result.source_retired = true;
		if (releaseRetiredSlottedPool(request.source_segment_id, currentSource.generation, lsns.next(), options)) {
			static_cast<void>(lsns.consume());
			result.source_released = true;
		} else {
			result.status = RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS;
		}
		result.next_lsn = lsns.next();
		return result;
	}

	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactEntityPool(const RelocatableSlottedPoolCompactionRequest &request,
															const MetadataPublishOptions &options) {
		requireCompaction(entity_pages_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable Entity-page compaction requires a typed Entity-page store");
		const TypedPoolCompactionOperations operations{
				[](SegmentKind kind) { return kind == SegmentKind::SGK_ENTITY_PAGE_POOL; },
				[this](MetadataSnapshotPin &snapshot, PageId pageId, SegmentId destination, LogSequenceNumber lsn,
					   const MetadataPublishOptions &publishOptions) {
					static_cast<void>(entity_pages_->evacuatePoolPage(snapshot, pageId, destination, lsn, publishOptions));
				},
		};
		const RetiredPoolReleaser releasePool = [this](SegmentId segmentId, Generation generation,
																 LogSequenceNumber lsn,
																 const MetadataPublishOptions &publishOptions) {
			return releaseRetiredEntityPool(segmentId, generation, lsn, publishOptions);
		};
		return compactTypedPool(
				pages_, metadata_, request, operations, releasePool,
				{"relocatable Entity-page compaction requires distinct pools, a bounded page budget, and an LSN",
				 "relocatable Entity-page compaction requires Entity page pools",
				 "relocatable Entity-page compaction cannot reserve a complete LSN publication sequence",
				 "relocatable Entity-page compaction observed a page move while planning evacuation"},
				options);
	}

	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactBTreePool(const RelocatableSlottedPoolCompactionRequest &request,
												 const MetadataPublishOptions &options) {
		requireCompaction(btree_pages_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable B+Tree compaction requires a typed B+Tree page store");
		const TypedPoolCompactionOperations operations{
				[](SegmentKind kind) { return kind == SegmentKind::SGK_INDEX_PAGE_POOL; },
				[this](MetadataSnapshotPin &snapshot, PageId pageId, SegmentId destination, LogSequenceNumber lsn,
					   const MetadataPublishOptions &publishOptions) {
					BTreePage page = btree_pages_->read(snapshot, pageId);
					snapshot.release();
					static_cast<void>(btree_pages_->evacuate(page, destination, lsn, publishOptions));
				},
		};
		const RetiredPoolReleaser releasePool = [this](SegmentId segmentId, Generation generation,
													   LogSequenceNumber lsn,
													   const MetadataPublishOptions &publishOptions) {
			return releaseRetiredSlottedPool(segmentId, generation, lsn, publishOptions);
		};
		return compactTypedPool(pages_, metadata_, request, operations, releasePool,
								{"relocatable B+Tree compaction requires distinct pools, a bounded page budget, and an LSN",
								 "relocatable B+Tree compaction requires index page pools",
								 "relocatable B+Tree compaction cannot reserve a complete LSN publication sequence",
								 "relocatable B+Tree compaction observed a page move while planning evacuation"},
								options);
	}

	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactVectorPool(const RelocatableSlottedPoolCompactionRequest &request,
												  const MetadataPublishOptions &options) {
		requireCompaction(vector_pages_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable vector compaction requires a typed Vector page store");
		const TypedPoolCompactionOperations operations{
				supportsVectorEvacuation,
				[this](MetadataSnapshotPin &snapshot, PageId pageId, SegmentId destination, LogSequenceNumber lsn,
					   const MetadataPublishOptions &publishOptions) {
					VectorPage page = vector_pages_->read(snapshot, pageId);
					snapshot.release();
					static_cast<void>(vector_pages_->evacuate(page, destination, lsn, publishOptions));
				},
		};
		const RetiredPoolReleaser releasePool = [this](SegmentId segmentId, Generation generation,
													   LogSequenceNumber lsn,
													   const MetadataPublishOptions &publishOptions) {
			return releaseRetiredSlottedPool(segmentId, generation, lsn, publishOptions);
		};
		return compactTypedPool(pages_, metadata_, request, operations, releasePool,
								{"relocatable vector compaction requires distinct pools, a bounded page budget, and an LSN",
								 "relocatable vector compaction requires raw or PQ vector page pools",
								 "relocatable vector compaction cannot reserve a complete LSN publication sequence",
								 "relocatable vector compaction observed a page move while planning evacuation"},
								options);
	}

	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactVectorManifestPool(const RelocatableSlottedPoolCompactionRequest &request,
														  const MetadataPublishOptions &options) {
		requireCompaction(vector_manifests_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable vector manifest compaction requires a typed Vector Manifest store");
		const TypedPoolCompactionOperations operations{
				[](SegmentKind kind) { return kind == SegmentKind::SGK_VECTOR_MANIFEST_POOL; },
				[this](MetadataSnapshotPin &snapshot, PageId pageId, SegmentId destination, LogSequenceNumber lsn,
					   const MetadataPublishOptions &publishOptions) {
					snapshot.release();
					static_cast<void>(vector_manifests_->evacuatePage(pageId, destination, lsn, publishOptions));
				},
		};
		const RetiredPoolReleaser releasePool = [this](SegmentId segmentId, Generation generation,
													   LogSequenceNumber lsn,
													   const MetadataPublishOptions &publishOptions) {
			return releaseRetiredSlottedPool(segmentId, generation, lsn, publishOptions);
		};
		return compactTypedPool(
				pages_, metadata_, request, operations, releasePool,
				{"relocatable vector manifest compaction requires distinct pools, a bounded page budget, and an LSN",
				 "relocatable vector manifest compaction requires vector manifest page pools",
				 "relocatable vector manifest compaction cannot reserve a complete LSN publication sequence",
				 "relocatable vector manifest compaction observed a page move while planning evacuation"},
				options);
	}

	RelocatableSlottedPoolCompactionResult
	RelocatableCompactionCoordinator::compactLargeValuePool(const RelocatableSlottedPoolCompactionRequest &request,
																					 const MetadataPublishOptions &options) {
		requireCompaction(large_values_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable LargeValue compaction requires a typed LargeValue store");
		const TypedPoolCompactionOperations operations{
				[](SegmentKind kind) {
					return kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL ||
							   kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				},
				[this](MetadataSnapshotPin &snapshot, PageId pageId, SegmentId destination, LogSequenceNumber lsn,
					   const MetadataPublishOptions &publishOptions) {
					snapshot.release();
					static_cast<void>(large_values_->evacuatePage(pageId, destination, lsn, publishOptions));
				},
		};
		const RetiredPoolReleaser releasePool = [this](SegmentId segmentId, Generation generation,
																			   LogSequenceNumber lsn,
																			   const MetadataPublishOptions &publishOptions) {
			return releaseRetiredSlottedPool(segmentId, generation, lsn, publishOptions);
		};
		return compactTypedPool(
				pages_, metadata_, request, operations, releasePool,
				{"relocatable LargeValue compaction requires distinct pools, a bounded page budget, and an LSN",
				 "relocatable LargeValue compaction requires data or manifest page pools",
				 "relocatable LargeValue compaction cannot reserve a complete LSN publication sequence",
				 "relocatable LargeValue compaction observed a page move while planning evacuation"},
				options);
	}

	bool RelocatableCompactionCoordinator::releaseRetiredSlottedPool(SegmentId segmentId, Generation generation,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		const uint8_t invalid = static_cast<uint8_t>(!segmentId.isValid()) |
								static_cast<uint8_t>(!generation.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		requireCompaction(invalid == 0U, FormatError::FERR_INVALID_HEADER,
						  "relocatable retired slotted-pool release requires an identity, generation, and LSN");
		if (metadata_.activeSnapshotCount() != 0U) {
			return false;
		}
		static_cast<void>(pages_.releaseRetiredEmptyPool(segmentId, generation, checkpointLsn, options));
		return true;
	}

	bool RelocatableCompactionCoordinator::releaseRetiredEntityPool(SegmentId segmentId, Generation generation,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options) {
		const uint8_t invalid = static_cast<uint8_t>(!segmentId.isValid()) |
								static_cast<uint8_t>(!generation.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		requireCompaction(invalid == 0U, FormatError::FERR_INVALID_HEADER,
						  "relocatable retired Entity-pool release requires an identity, generation, and LSN");
		requireCompaction(entity_pages_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable retired Entity-pool release requires a typed Entity-page store");
		if (metadata_.activeSnapshotCount() != 0U) {
			return false;
		}
		static_cast<void>(entity_pages_->releaseRetiredEmptyPool(segmentId, generation, checkpointLsn, options));
		return true;
	}

	RelocatableExtentCompactionResult
	RelocatableCompactionCoordinator::compactCodebookExtent(const RelocatableCodebookExtentCompactionRequest &request,
													  const MetadataPublishOptions &options) {
		const uint8_t invalid =
					static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(request.codebook_id)) |
				static_cast<uint8_t>(!request.extent_id.isValid()) |
				static_cast<uint8_t>(!request.first_destination_slot.isValid()) |
				static_cast<uint8_t>(!request.last_destination_slot.isValid()) |
				static_cast<uint8_t>(request.first_destination_slot.value() > request.last_destination_slot.value()) |
				static_cast<uint8_t>(!request.first_lsn.isValid());
		requireCompaction(invalid == 0U, FormatError::FERR_INVALID_HEADER,
						  "relocatable codebook compaction requires an identity, destination range, and LSN");
		requireCompaction(codebooks_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable codebook compaction requires Codebook and physical extent stores");
		const uint64_t remaining = std::numeric_limits<uint64_t>::max() - request.first_lsn.value();
		requireCompaction(2U <= remaining, FormatError::FERR_ARITHMETIC_OVERFLOW,
						  "relocatable codebook compaction cannot reserve a complete LSN publication sequence");
		const std::optional<PhysicalSegmentRelocation> relocation =
				codebooks_->relocateExtent(request.codebook_id, request.extent_id, request.first_destination_slot,
											   request.last_destination_slot, request.first_lsn, options);
		if (!relocation.has_value()) {
			return {RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY, false, false, request.first_lsn};
		}
		const LogSequenceNumber releaseLsn(request.first_lsn.value() + 1U);
		const bool released = releaseRelocatedExtentSource(relocation->source.segment_id,
														   relocation->destination.generation, releaseLsn, options);
		return {released ? RelocatableCompactionStatus::VCS_COMPLETED
						 : RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS,
				true, released, released ? LogSequenceNumber(releaseLsn.value() + 1U) : releaseLsn};
	}


	bool RelocatableCompactionCoordinator::releaseRelocatedExtentSource(SegmentId segmentId, Generation generation,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		const uint8_t invalid = static_cast<uint8_t>(!segmentId.isValid()) |
								static_cast<uint8_t>(!generation.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		requireCompaction(invalid == 0U, FormatError::FERR_INVALID_HEADER,
						  "relocatable relocated extent source release requires an identity, generation, and LSN");
		requireCompaction(physical_ != nullptr, FormatError::FERR_INVALID_STATE,
						  "relocatable relocated extent source release requires a physical segment allocator");
		if (metadata_.activeSnapshotCount() != 0U) {
			return false;
		}
		static_cast<void>(physical_->releaseRelocatedSource(segmentId, generation, checkpointLsn, options));
		return true;
	}

} // namespace graph::storage
