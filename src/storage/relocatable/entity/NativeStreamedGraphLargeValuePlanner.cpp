/**
 * @file NativeStreamedGraphLargeValuePlanner.cpp
 * @brief Bounded LargeValue planning for streamed graph Property roots.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.hpp"

#include <array>
#include <limits>
#include <memory>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkedAdd(size_t &target, size_t value, const char *message) {
			if (value > std::numeric_limits<size_t>::max() - target) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			target += value;
		}

		[[nodiscard]] size_t inlineRootCapacity() {
			return RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		}

		void validateOverflowRoot(const PropertyCollectionRoot &root, uint64_t previousEdgeId) {
			if (root.owner_entity_id == 0U || root.owner_entity_id <= previousEdgeId ||
				root.owner_entity_type != Edge::typeId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue source lost canonical Edge-owner ordering");
			}
		}

		[[nodiscard]] EntityPropertyLargeValueReference
		planReference(const LargeValueStore &store, LargeValueId valueId, const PropertyCollectionRoot &root,
					  std::span<const std::byte> payload, const NativeStreamedGraphLargeValuePlanningOptions &options) {
			EntityPropertyLargeValueReference result;
			result.owner_entity_id = root.owner_entity_id;
			result.owner_entity_type = root.owner_entity_type;
			result.value = store.planReference(valueId, Generation(1U), payload, LargeValueKind::LVK_BYTES,
											   RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, options.compression,
											   options.extent_class);
			return result;
		}

		[[nodiscard]] uint64_t decodeReferenceMapKey(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue reference map has an invalid key width");
			}
			uint64_t value = 0U;
			for (const std::byte byte: key) {
				value = (value << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(byte));
			}
			return value;
		}

		class CanonicalReferenceCursor final {
		public:
			CanonicalReferenceCursor(const NativeStreamedGraphLargeValueReferenceMapSource &source,
								 LargeValueId firstValueId, size_t referenceCount) :
				first_value_id_(firstValueId), reference_count_(referenceCount), has_map_(static_cast<bool>(source)) {
				if (!has_map_) {
					return;
				}
				cursor_ = source();
				if (!cursor_) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native streamed Edge LargeValue reference map did not open a cursor");
				}
			}

			[[nodiscard]] LargeValueId resolve(LargeValueId candidate) {
				if (resolved_ == reference_count_) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue reference map has more roots than planned");
				}
				if (!has_map_) {
					++resolved_;
					return candidate;
				}
				const std::optional<BTreePageEntry> entry = cursor_();
				if (!entry.has_value() || decodeReferenceMapKey(entry->key) != candidate.value() ||
					entry->value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue reference map diverged from its Property replay");
				}
				const LargeValueId canonical(entry->value_or_child);
				if (canonical < first_value_id_ || canonical > candidate) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue reference map has an invalid canonical identity");
				}
				++resolved_;
				if (resolved_ == reference_count_ && cursor_().has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue reference map has trailing identities");
				}
				return canonical;
			}

		private:
			LargeValueId first_value_id_{};
			size_t reference_count_ = 0U;
			size_t resolved_ = 0U;
			bool has_map_ = false;
			OrderedBTreeEntryCursor cursor_;
		};

	} // namespace

	NativeStreamedGraphLargeValuePlan
	NativeStreamedGraphLargeValuePlanner::measure(const NativeStreamedGraphDeferredPublication &publication,
												  const LargeValueStore &store,
												  const NativeStreamedGraphLargeValuePlanningOptions &options) {
		NativeStreamedGraphLargeValuePlan result;
		result.property_pages = NativeStreamedGraphPropertyPageMaterializer::measurePages(
				publication,
				[&result, &store, &options](const PropertyCollectionRoot &root, std::vector<std::byte> &&) {
					if (result.value_count == std::numeric_limits<size_t>::max() ||
						result.value_count >= std::numeric_limits<uint64_t>::max()) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "native streamed Edge LargeValue plan exhausted its logical value namespace");
					}
					const LargeValueId valueId(static_cast<uint64_t>(result.value_count) + 1U);
					const std::vector<std::byte> payload = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
					const EntityPropertyLargeValueReference reference =
							planReference(store, valueId, root, payload, options);
					checkedAdd(result.data_page_count,
							   store.plannedDataPageCount(payload, options.compression, options.extent_class),
							   "native streamed Edge LargeValue data-page count overflowed");
					checkedAdd(result.manifest_page_count,
							   store.plannedManifestPageCount(payload, options.compression, options.extent_class),
							   "native streamed Edge LargeValue Manifest-page count overflowed");
					++result.reference_count;
					++result.value_count;
					return EntityPropertyLargeValueReferenceCodec::encode(reference);
				});
		return result;
	}

	NativeStreamedGraphLargeValuePlan NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
			const NativeStreamedGraphLargeValuePlanningOptions &options,
			const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences) {
		if (!firstValueId.isValid() || !canonicalReferences || plan.reference_count == 0U ||
			plan.value_count != plan.reference_count) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue canonical plan requires an undeduplicated bounded source plan");
		}
		NativeStreamedGraphLargeValuePlan result = plan;
		result.value_count = 0U;
		result.data_page_count = 0U;
		result.manifest_page_count = 0U;
		size_t observedReferences = 0U;
		uint64_t previousEdgeId = 0U;
		const NativeStreamedGraphLargePropertyRootEncoder encoder = makeRootEncoder(
				store, firstValueId, plan, options,
				[&](NativeStreamedGraphLargeValueItem &&item) {
					const LargeValueId candidate(firstValueId.value() + static_cast<uint64_t>(observedReferences));
					if (item.stored_reference.value.value_id == candidate) {
						checkedAdd(result.data_page_count, item.planned_data_page_count,
								   "native streamed Edge LargeValue canonical data-page count overflowed");
						checkedAdd(result.manifest_page_count, item.planned_manifest_page_count,
								   "native streamed Edge LargeValue canonical Manifest-page count overflowed");
						if (result.value_count == std::numeric_limits<size_t>::max()) {
							fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "native streamed Edge LargeValue canonical value count overflowed");
						}
						++result.value_count;
					}
					++observedReferences;
				},
				canonicalReferences);
		publication.visitPropertyRoots([&](PropertyCollectionRoot &&root) {
			if (root.entries.empty()) {
				return;
			}
			validateOverflowRoot(root, previousEdgeId);
			std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root);
			if (encoded.size() > inlineRootCapacity()) {
				static_cast<void>(encoder(root, std::move(encoded)));
			}
			previousEdgeId = root.owner_entity_id;
		});
		if (observedReferences != plan.reference_count || result.value_count == 0U ||
			result.value_count > result.reference_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue canonical plan diverged from its sealed source");
		}
		return result;
	}

	NativeStreamedGraphLargePropertyRootEncoder
	NativeStreamedGraphLargeValuePlanner::makeRootEncoder(const LargeValueStore &store, LargeValueId firstValueId,
														  const NativeStreamedGraphLargeValuePlan &plan,
														  const NativeStreamedGraphLargeValuePlanningOptions &options,
														  const NativeStreamedGraphLargeValueItemVisitor &visit,
														  const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences) {
		if (!firstValueId.isValid() || plan.reference_count == 0U || plan.value_count == 0U ||
			plan.value_count > plan.reference_count || !visit ||
			plan.reference_count - 1U > std::numeric_limits<uint64_t>::max() - firstValueId.value()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue encoder requires a bounded durable identity range and visitor");
		}
		struct State {
			size_t emitted = 0U;
			CanonicalReferenceCursor references;

			State(const NativeStreamedGraphLargeValueReferenceMapSource &source, LargeValueId firstValueId,
				  size_t referenceCount) : references(source, firstValueId, referenceCount) {}
		};
		auto state = std::make_shared<State>(canonicalReferences, firstValueId, plan.reference_count);
		return [&store, firstValueId, plan, options, visit, state](const PropertyCollectionRoot &root,
																				 std::vector<std::byte> &&) mutable {
			if (state->emitted == plan.reference_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue source has more overflow roots than its measured plan");
			}
			const LargeValueId candidateId(firstValueId.value() + static_cast<uint64_t>(state->emitted));
			const LargeValueId valueId = state->references.resolve(candidateId);
			const std::vector<std::byte> payload = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
			const EntityPropertyLargeValueReference reference = planReference(store, valueId, root, payload, options);
			NativeStreamedGraphLargeValueItem item;
			item.stored_reference = reference;
			item.planned_data_page_count = store.plannedDataPageCount(payload, options.compression, options.extent_class);
			item.planned_manifest_page_count =
					store.plannedManifestPageCount(payload, options.compression, options.extent_class);
			item.logical_bytes = std::move(payload);
			visit(std::move(item));
			++state->emitted;
			return EntityPropertyLargeValueReferenceCodec::encode(reference);
		};
	}

	void NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
			const NativeStreamedGraphLargeValuePlanningOptions &options,
			const NativeStreamedGraphPropertyPageAssignmentSource &assign,
			const NativeStreamedGraphPropertyPageVisitor &visitPage,
			const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences) {
		if (!assign || !visitPage || (plan.reference_count != 0U && !firstValueId.isValid())) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue Property replay requires complete page consumers");
		}
		size_t emittedValues = 0U;
		size_t observedDataPages = 0U;
		size_t observedManifestPages = 0U;
		size_t observedPropertyPages = 0U;
		size_t observedProperties = 0U;
		NativeStreamedGraphLargePropertyRootEncoder encoder;
		if (plan.reference_count != 0U) {
			encoder =
					makeRootEncoder(store, firstValueId, plan, options, [&](NativeStreamedGraphLargeValueItem &&item) {
						if (emittedValues == plan.reference_count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge LargeValue replay exceeded its measured value count");
						}
						const LargeValueId candidate(firstValueId.value() + static_cast<uint64_t>(emittedValues));
						if (item.stored_reference.value.value_id == candidate) {
							checkedAdd(observedDataPages, item.planned_data_page_count,
									   "native streamed Edge LargeValue replay data-page count overflowed");
							checkedAdd(observedManifestPages, item.planned_manifest_page_count,
									   "native streamed Edge LargeValue replay Manifest-page count overflowed");
						}
						++emittedValues;
					}, canonicalReferences);
		}
		NativeStreamedGraphPropertyPageMaterializer::materializePages(
				publication, assign,
				[&](NativeStreamedGraphPropertyPage &&page) {
					if (observedPropertyPages == std::numeric_limits<size_t>::max() ||
						page.property_locations.size() > std::numeric_limits<size_t>::max() - observedProperties) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "native streamed Edge LargeValue replay Property-page count overflowed");
					}
					++observedPropertyPages;
					observedProperties += page.property_locations.size();
					visitPage(std::move(page));
				},
				encoder);
		if (emittedValues != plan.reference_count || observedDataPages != plan.data_page_count ||
			observedManifestPages != plan.manifest_page_count ||
			observedPropertyPages != plan.property_pages.page_count ||
			observedProperties != plan.property_pages.property_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue replay diverged from its measured publication plan");
		}
	}

	void NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
			const NativeStreamedGraphLargeValuePlanningOptions &options,
			const NativeStreamedGraphLargeValueItemVisitor &visitItem,
			const NativeStreamedGraphLargeValueReferenceMapSource &canonicalReferences) {
		if (!visitItem || (plan.reference_count != 0U && !firstValueId.isValid())) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue replay requires a bounded durable identity range and consumer");
		}
		size_t emittedValues = 0U;
		size_t observedDataPages = 0U;
		size_t observedManifestPages = 0U;
		uint64_t previousEdgeId = 0U;
		NativeStreamedGraphLargePropertyRootEncoder encoder;
		if (plan.reference_count != 0U) {
			encoder =
					makeRootEncoder(store, firstValueId, plan, options, [&](NativeStreamedGraphLargeValueItem &&item) {
						if (emittedValues == plan.reference_count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge LargeValue replay exceeded its measured value count");
						}
						const LargeValueId candidate(firstValueId.value() + static_cast<uint64_t>(emittedValues));
						if (item.stored_reference.value.value_id == candidate) {
							checkedAdd(observedDataPages, item.planned_data_page_count,
									   "native streamed Edge LargeValue replay data-page count overflowed");
							checkedAdd(observedManifestPages, item.planned_manifest_page_count,
									   "native streamed Edge LargeValue replay Manifest-page count overflowed");
							visitItem(std::move(item));
						}
						++emittedValues;
					}, canonicalReferences);
		}
		publication.visitPropertyRoots([&](PropertyCollectionRoot &&root) {
			if (root.entries.empty()) {
				return;
			}
			validateOverflowRoot(root, previousEdgeId);
			std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root);
			if (encoded.size() > inlineRootCapacity()) {
				if (!encoder) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue source exceeds an empty measured plan");
				}
				static_cast<void>(encoder(root, std::move(encoded)));
			}
			previousEdgeId = root.owner_entity_id;
		});
		if (emittedValues != plan.reference_count || observedDataPages != plan.data_page_count ||
			observedManifestPages != plan.manifest_page_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue physical replay diverged from its measured publication plan");
		}
	}

} // namespace graph::storage
