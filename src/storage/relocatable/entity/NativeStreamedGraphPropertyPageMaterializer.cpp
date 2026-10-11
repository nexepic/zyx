/**
 * @file NativeStreamedGraphPropertyPageMaterializer.cpp
 * @brief Bounded Edge Property-page images for streamed graph publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPropertyPageMaterializer.hpp"

#include <limits>
#include <optional>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateAssignment(const PageOwnerHeader &owner) {
			if (!owner.page_id.isValid() || owner.owner_id == 0U || owner.kind != PageOwnerKind::POK_PROPERTY ||
				owner.flags != 0U || owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ||
				!owner.generation.isValid() || !owner.committed_lsn.isValid()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge Property-page assignment has an invalid metadata-owned identity");
			}
		}

		[[nodiscard]] size_t inlineRootCapacity() {
			return RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		}

		void validateRoot(const PropertyCollectionRoot &root, uint64_t previousEdgeId) {
			if (root.owner_entity_id == 0U || root.owner_entity_id <= previousEdgeId ||
				root.owner_entity_type != Edge::typeId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge Property source lost canonical Edge-owner ordering");
			}
		}

		void validateStoredRoot(std::span<const std::byte> bytes, const PropertyCollectionRoot &source) {
			if (bytes.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge Property LargeValue encoder returned an empty stored root");
			}
			if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
				const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
				if (reference.owner_entity_id != source.owner_entity_id ||
					reference.owner_entity_type != source.owner_entity_type) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge Property LargeValue reference changed its owner identity");
				}
				return;
			}
			const PropertyCollectionRoot decoded = EntityPropertyCollectionRootCodec::decode(bytes);
			if (decoded.owner_entity_id != source.owner_entity_id || decoded.owner_entity_type != source.owner_entity_type ||
				decoded != source) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge Property encoder changed an inline root");
			}
		}

		void visitPages(const NativeStreamedGraphDeferredPublication &publication,
				const NativeStreamedGraphPropertyPageAssignmentSource &assign,
				const NativeStreamedGraphPropertyPageVisitor &visit,
				const NativeStreamedGraphLargePropertyRootEncoder &encodeLargeRoot) {
			if (!assign || !visit || publication.recordCount() == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge Property-page materialization requires a bounded publication and consumers");
			}
			size_t pageOrdinal = 0U;
			uint64_t previousEdgeId = 0U;
			std::optional<NativeStreamedGraphPropertyPage> active;
			std::optional<SlottedPage> page;
			const auto startPage = [&] {
				if (pageOrdinal == std::numeric_limits<size_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native streamed Edge Property-page ordinal overflowed");
				}
				const NativeStreamedGraphPropertyPageAssignment assignment = assign(pageOrdinal);
				validateAssignment(assignment.owner);
				active.emplace();
				active->owner = assignment.owner;
				page.emplace(SlottedPage::create(assignment.owner));
			};
			const auto finishPage = [&] {
				if (!active.has_value() || !page.has_value() || active->property_locations.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge Property materializer attempted to publish an empty page");
				}
				active->payload = page->serialize();
				active->free_bytes = page->stats().contiguous_free_bytes;
				visit(std::move(*active));
				active.reset();
				page.reset();
				++pageOrdinal;
			};

			publication.visitPropertyRoots([&](PropertyCollectionRoot &&root) {
				if (root.entries.empty()) {
					return;
				}
				validateRoot(root, previousEdgeId);
				std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root);
				if (encoded.size() > inlineRootCapacity()) {
					if (!encodeLargeRoot) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native streamed Edge Property root requires same-generation LargeValue materialization");
					}
					encoded = encodeLargeRoot(root, std::move(encoded));
					validateStoredRoot(encoded, root);
					if (encoded.size() > inlineRootCapacity()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native streamed Edge Property LargeValue reference does not fit a Property-page slot");
					}
				}
				if (!page.has_value()) {
					startPage();
				}
				SlottedSlotReference slot;
				try {
					slot = page->insert(encoded);
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE || active->property_locations.empty()) {
						throw;
					}
					finishPage();
					startPage();
					slot = page->insert(encoded);
				}
				if (!slot.isValid()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge Property materializer received an invalid SlottedPage reference");
				}
				active->property_locations.push_back(
						{{EntityPropertyOwnerKind::EPOK_EDGE, root.owner_entity_id}, active->owner.page_id, slot.slot_id,
						 slot.generation});
				previousEdgeId = root.owner_entity_id;
			});
			if (page.has_value()) {
				finishPage();
			}
		}

	} // namespace

	NativeStreamedGraphInlinePropertyPageCount NativeStreamedGraphPropertyPageMaterializer::measureInlinePages(
			const NativeStreamedGraphDeferredPublication &publication) {
		return measurePages(publication, {});
	}

	NativeStreamedGraphInlinePropertyPageCount NativeStreamedGraphPropertyPageMaterializer::measurePages(
			const NativeStreamedGraphDeferredPublication &publication,
			const NativeStreamedGraphLargePropertyRootEncoder &encodeLargeRoot) {
		NativeStreamedGraphInlinePropertyPageCount result;
		materializePages(
				publication,
				[](size_t) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(1U), 1U, PageOwnerKind::POK_PROPERTY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
							 Generation(1U), LogSequenceNumber(1U)}};
				},
				[&result](NativeStreamedGraphPropertyPage &&page) {
					if (result.page_count == std::numeric_limits<size_t>::max() ||
						page.property_locations.size() > std::numeric_limits<size_t>::max() - result.property_count) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "native streamed Edge Property-page measurement overflowed");
					}
					++result.page_count;
					result.property_count += page.property_locations.size();
				},
				encodeLargeRoot);
		return result;
	}

	size_t NativeStreamedGraphPropertyPageMaterializer::countInlinePages(
			const NativeStreamedGraphDeferredPublication &publication) {
		return measureInlinePages(publication).page_count;
	}

	void NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
			const NativeStreamedGraphDeferredPublication &publication,
			const NativeStreamedGraphPropertyPageAssignmentSource &assign,
			const NativeStreamedGraphPropertyPageVisitor &visit) {
		materializePages(publication, assign, visit, {});
	}

	void NativeStreamedGraphPropertyPageMaterializer::materializePages(
			const NativeStreamedGraphDeferredPublication &publication,
			const NativeStreamedGraphPropertyPageAssignmentSource &assign,
			const NativeStreamedGraphPropertyPageVisitor &visit,
			const NativeStreamedGraphLargePropertyRootEncoder &encodeLargeRoot) {
		visitPages(publication, assign, visit, encodeLargeRoot);
	}

} // namespace graph::storage
