/**
 * @file PropertyCollectionRootStore.cpp
 * @brief Slotted-page placement and directory publication for Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "graph/debug/PerfTrace.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwRootStoreError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void validateOptions(const PropertyCollectionRootStoreOptions &options) {
			if (!options.first_slot.isValid() || !options.last_slot.isValid() ||
				options.first_slot > options.last_slot || options.page_owner_id == 0U ||
				options.maximum_large_value_dedup_candidates == 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property collection-root store has invalid physical slots or page owner");
			}
		}

		void validatePlannerSnapshot(const RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot) {
			if (!metadata.ownsSnapshot(snapshot)) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property planning requires this store's active metadata snapshot");
			}
		}

		[[nodiscard]] bool sameCatalogEntry(const CatalogEntry &left, const CatalogEntry &right) {
			const uint8_t mismatch = static_cast<uint8_t>(left.segment_id != right.segment_id) |
									 static_cast<uint8_t>(left.physical_slot != right.physical_slot) |
									 static_cast<uint8_t>(left.kind != right.kind) |
									 static_cast<uint8_t>(left.owner_id != right.owner_id) |
									 static_cast<uint8_t>(left.generation != right.generation) |
									 static_cast<uint8_t>(left.state != right.state) |
									 static_cast<uint8_t>(left.committed_lsn != right.committed_lsn);
			return mismatch == 0U;
		}

		[[nodiscard]] EntityPropertyKey entityKeyForRoot(const PropertyCollectionRoot &root) {
			const EntityPropertyOwnerKind ownerKind = root.owner_entity_type == Node::typeId
															  ? EntityPropertyOwnerKind::EPOK_NODE
															  : EntityPropertyOwnerKind::EPOK_EDGE;
			if (root.owner_entity_type != Node::typeId && root.owner_entity_type != Edge::typeId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property root has an unknown logical owner type");
			}
			const EntityPropertyKey key{ownerKind, root.owner_entity_id};
			if (!key.isValid()) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property root has an invalid logical owner identity");
			}
			return key;
		}

		void validateStoredEntityPropertyRecord(std::span<const std::byte> bytes, const EntityPropertyKey &key,
												std::span<const NativeGraphLargeValueMutation> largeValues,
												std::span<const LargeValueRef> reusedLargeValues) {
			if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
				// The page contains a logical reference; the root bytes are prepared
				// in the LargeValue frame and published by the same metadata batch.
				const EntityPropertyLargeValueReference reference =
						EntityPropertyLargeValueReferenceCodec::decode(bytes);
				const EntityPropertyKey referenceKey =
						entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
				const auto matchingValue =
						std::ranges::find_if(largeValues, [&reference](const NativeGraphLargeValueMutation &mutation) {
							return mutation.reference == reference.value;
						});
				const bool reused = std::ranges::find(reusedLargeValues, reference.value) != reusedLargeValues.end();
				const uint8_t invalid = static_cast<uint8_t>(referenceKey != key) |
										static_cast<uint8_t>(matchingValue == largeValues.end() && !reused);
				if (invalid != 0U) {
					throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"Entity Property reference disagrees with its LargeValue mutation");
				}
				return;
			}
			const PropertyCollectionRoot root = EntityPropertyCollectionRootCodec::decode(bytes);
			if (entityKeyForRoot(root) != key || root.entries.empty()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property stored record has an invalid canonical root");
			}
		}

		[[nodiscard]] PropertyCollectionRoot canonicalEntityRoot(const PropertyCollectionRoot &root) {
			static_cast<void>(entityKeyForRoot(root));
			return root;
		}

	} // namespace

	PropertyCollectionRootStore::PropertyCollectionRootStore(SlottedPagePool &pages,
															 RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument("Property collection-root store requires one metadata store");
		}
	}

	PropertyCollectionRootStore::PropertyCollectionRootStore(SlottedPagePool &pages, LargeValueStore &largeValues,
															 RelocatableMetadataStore &metadata) :
		pages_(pages), large_values_(&largeValues), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_) || !large_values_->usesMetadataStore(metadata_)) {
			throw std::invalid_argument("Property collection-root store requires one metadata store");
		}
	}

	PropertyCollectionRoot PropertyCollectionRootStore::readEntity(const MetadataSnapshotPin &snapshot,
																   const EntityPropertyKey &key) const {
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property read requires a valid logical owner key");
		}
		const EntityPropertyLocation location = metadata_.findEntityPropertyLocation(snapshot, key);
		const SlottedPage page = pages_.read(snapshot, location.page_id);
		return decodeEntityRoot(snapshot, key, location, page);
	}

	std::optional<PropertyCollectionRoot>
	PropertyCollectionRootStore::readEntityOptional(const MetadataSnapshotPin &snapshot,
													const EntityPropertyKey &key) const {
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"optional Entity Property read requires a valid logical owner key");
		}
		const std::optional<EntityPropertyLocation> location =
				metadata_.findEntityPropertyLocationOptional(snapshot, key);
		if (!location.has_value()) {
			return std::nullopt;
		}
		const SlottedPage page = pages_.read(snapshot, location->page_id);
		return decodeEntityRoot(snapshot, key, *location, page);
	}

	PropertyCollectionRootScanCursor
	PropertyCollectionRootStore::openScanCursor(const MetadataSnapshotPin &snapshot) const {
		if (!metadata_.ownsSnapshot(snapshot)) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property scan cursor requires this store's active metadata snapshot");
		}
		return PropertyCollectionRootScanCursor(*this, snapshot.superblock().generation);
	}

	PropertyCollectionRootScanCursor::PropertyCollectionRootScanCursor(
			PropertyCollectionRootScanCursor &&other) noexcept :
		store_(std::exchange(other.store_, nullptr)), generation_(std::exchange(other.generation_, Generation{})),
		current_page_id_(std::exchange(other.current_page_id_, PageId{})),
		current_page_(std::move(other.current_page_)) {
		other.current_page_.reset();
	}

	PropertyCollectionRootScanCursor &
	PropertyCollectionRootScanCursor::operator=(PropertyCollectionRootScanCursor &&other) noexcept {
		if (this != &other) {
			store_ = std::exchange(other.store_, nullptr);
			generation_ = std::exchange(other.generation_, Generation{});
			current_page_id_ = std::exchange(other.current_page_id_, PageId{});
			current_page_ = std::move(other.current_page_);
			other.current_page_.reset();
		}
		return *this;
	}

	PropertyCollectionRoot PropertyCollectionRootStore::decodeEntityRoot(const MetadataSnapshotPin &snapshot,
																		 const EntityPropertyKey &key,
																		 const EntityPropertyLocation &location,
																		 const SlottedPage &page) const {
		const uint8_t invalidLocation =
				static_cast<uint8_t>(location.key != key) | static_cast<uint8_t>(location.page_id != page.pageId());
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location disagrees with its decoded Property page");
		}
		if (page.ownerKind() != PageOwnerKind::POK_PROPERTY) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location does not resolve to a Property page");
		}
		const std::vector<std::byte> bytes = page.read({location.page_id, location.slot_id, location.slot_generation});
		PropertyCollectionRoot root;
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			const uint8_t invalidReference =
					static_cast<uint8_t>(referenceKey != key) |
					static_cast<uint8_t>(reference.value.kind != LargeValueKind::LVK_BYTES) |
					static_cast<uint8_t>(reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
			if (invalidReference != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference has an invalid owner or content type");
			}
			const std::vector<std::byte> payload = large_values_->read(snapshot, reference.value);
			if (EntityPropertyLargeValuePayloadCodec::hasMagic(payload)) {
				root.owner_entity_id = reference.owner_entity_id;
				root.owner_entity_type = reference.owner_entity_type;
				root.entries = EntityPropertyLargeValuePayloadCodec::decode(payload);
			} else {
				// Accept pre-owner-neutral values so an already-created native file is
				// never silently decoded as a shared payload.
				root = EntityPropertyCollectionRootCodec::decode(payload);
				if (root.owner_entity_id != reference.owner_entity_id ||
					root.owner_entity_type != reference.owner_entity_type) {
					throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"Entity Property LargeValue root owner disagrees with its reference");
				}
			}
		} else {
			root = EntityPropertyCollectionRootCodec::decode(bytes);
		}
		const EntityPropertyKey decodedKey = entityKeyForRoot(root);
		if (decodedKey != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location resolves to another logical owner");
		}
		return root;
	}

	void PropertyCollectionRootScanCursor::validateSnapshot(const MetadataSnapshotPin &snapshot) const {
		if (store_ == nullptr || !store_->metadata_.ownsSnapshot(snapshot) ||
			snapshot.superblock().generation != generation_) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property scan cursor requires its original active metadata generation");
		}
	}

	PropertyCollectionRoot PropertyCollectionRootScanCursor::readLocation(const MetadataSnapshotPin &snapshot,
																		  const EntityPropertyKey &key,
																		  const EntityPropertyLocation &location) {
		validateSnapshot(snapshot);
		if (location.page_id != current_page_id_) {
			current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
			current_page_id_ = location.page_id;
		}
		return store_->decodeEntityRoot(snapshot, key, location, *current_page_);
	}

	PropertyCollectionRoot PropertyCollectionRootScanCursor::readEntity(const MetadataSnapshotPin &snapshot,
																		const EntityPropertyKey &key) {
		validateSnapshot(snapshot);
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property scan read requires a valid logical owner key");
		}
		return readLocation(snapshot, key, store_->metadata_.findEntityPropertyLocation(snapshot, key));
	}

	std::optional<PropertyCollectionRoot>
	PropertyCollectionRootScanCursor::readEntityOptional(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key) {
		debug::ScopedPerfTimer timer("native_graph.read.property_optional_lookup");
		validateSnapshot(snapshot);
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"optional Entity Property scan read requires a valid logical owner key");
		}
		const std::optional<EntityPropertyLocation> location =
				store_->metadata_.findEntityPropertyLocationOptional(snapshot, key);
		return location.has_value() ? std::optional(readLocation(snapshot, key, *location)) : std::nullopt;
	}

	EntityPropertyRootMutationPlan
	PropertyCollectionRootStore::planEntityRootReplacement(const MetadataSnapshotPin &snapshot,
														   const PropertyCollectionRoot &root,
														   const PropertyCollectionRootStoreOptions &options) const {
		validatePlannerSnapshot(metadata_, snapshot);
		return planEntityRootReplacementWithLargeValueId(snapshot, root, options,
														 LargeValueId(snapshot.superblock().next_large_value_id));
	}

	std::vector<EntityPropertyRootMutationPlan>
	PropertyCollectionRootStore::planEntityRootReplacements(const MetadataSnapshotPin &snapshot,
															std::span<const PropertyCollectionRoot> roots,
															const PropertyCollectionRootStoreOptions &options) const {
		validatePlannerSnapshot(metadata_, snapshot);
		validateOptions(options);
		if (roots.empty()) {
			return {};
		}
		// Batch roots are ordered and share one pinned snapshot. Materialize the
		// authoritative directory once so each root avoids a repeated radix-route
		// traversal during large graph writes.
		std::map<EntityPropertyKey, EntityPropertyLocation> existingLocations;
		metadata_.visitEntityPropertyLocations(snapshot, [&existingLocations](const EntityPropertyLocation &location) {
			if (!existingLocations.emplace(location.key, location).second) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property Directory contains duplicate owner keys");
			}
		});
		std::vector<SegmentId> currentPropertyPools;
		for (const CatalogEntry &pool: metadata_.findCurrentPropertyPagePools(snapshot, options.page_owner_id)) {
			currentPropertyPools.push_back(pool.segment_id);
		}
		std::vector<EntityPropertyRootMutationPlan> plans;
		plans.reserve(roots.size());
		// A pinned snapshot is immutable. New roots with equal encoded sizes therefore
		// have the same best-fit source page; cache that selection for this batch.
		std::map<size_t, std::optional<SlottedPagePlacement>> placementCache;
		EntityPropertyBatchPageCache pageCache;
		uint64_t nextLargeValueId = snapshot.superblock().next_large_value_id;
		uint64_t previousDirectoryId = 0U;
		for (const PropertyCollectionRoot &root: roots) {
			const EntityPropertyKey key = entityKeyForRoot(canonicalEntityRoot(root));
			if (key.directoryId() <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property batch roots must be strictly ordered by owner key");
			}
			previousDirectoryId = key.directoryId();
			const EntityPropertyRootMutationPlan plan =
					planEntityRootReplacementWithLargeValueId(snapshot, root, options, LargeValueId(nextLargeValueId),
																						 &existingLocations, &placementCache, &currentPropertyPools, &pageCache);
			if (!plan.large_values.empty()) {
				if (nextLargeValueId == std::numeric_limits<uint64_t>::max()) {
					nextLargeValueId = 0U;
				} else {
					++nextLargeValueId;
				}
			}
			plans.push_back(plan);
		}

		// Every plan above is built against the same pinned snapshot. Several owner
		// roots can therefore target the same source page. Replaying their final
		// logical state together produces one COW frame per source page, which keeps
		// multi-row CREATE/SET/REMOVE transactions both atomic and conflict-free.
		std::map<uint64_t, std::vector<size_t>> mutationsByPage;
		for (size_t ordinal = 0U; ordinal < plans.size(); ++ordinal) {
			const EntityPropertyRootMutationPlan &plan = plans[ordinal];
			if (!plan.page_replacement.has_value()) {
				continue;
			}
			const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
			const size_t memberMutationCount = request.upserted_properties.size() + request.erased_properties.size() +
											   request.relocated_properties.size();
			const bool validMemberMutation = memberMutationCount == 1U;
			// ZYX_COV_EXCL_NEXT: the private single-root planner emits exactly one mutation.
			if (!validMemberMutation) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property batch planner received a malformed single-root page mutation");
			}
			mutationsByPage[request.expected_location.page_id.value()].push_back(ordinal);
		}

		std::set<size_t> absorbedPlans;
		std::vector<EntityPropertyRootMutationPlan> coalesced;
		coalesced.reserve(plans.size());
		for (size_t ordinal = 0U; ordinal < plans.size(); ++ordinal) {
			if (absorbedPlans.contains(ordinal)) {
				continue;
			}
			const EntityPropertyRootMutationPlan &candidate = plans[ordinal];
			if (candidate.page_replacement.has_value()) {
				const auto group = mutationsByPage.find(candidate.page_replacement->expected_location.page_id.value());
				// ZYX_COV_EXCL_NEXT: every replacement was inserted above, and non-front members are already absorbed.
				if (group != mutationsByPage.end() && group->second.size() > 1U && group->second.front() == ordinal) {
					EntityPropertyRootMutationPlan merged = candidate;
					NativeGraphPropertyPageReplacementRequest request = *candidate.page_replacement;
					request.upserted_properties.clear();
					request.erased_properties.clear();
					request.relocated_properties.clear();
					request.retire_page = false;
					SlottedPage page = pages_.read(snapshot, request.expected_location.page_id);
					merged.large_values.clear();
					merged.reused_large_values.clear();
					std::vector<EntityPropertyRootMutationPlan> overflowPlans;
					for (const size_t memberOrdinal: group->second) {
						const EntityPropertyRootMutationPlan &member = plans[memberOrdinal];
						const NativeGraphPropertyPageReplacementRequest &memberRequest = *member.page_replacement;
						const uint8_t inconsistentSource =
								static_cast<uint8_t>(
										!sameCatalogEntry(memberRequest.expected_pool, request.expected_pool)) |
								static_cast<uint8_t>(memberRequest.expected_location != request.expected_location) |
								static_cast<uint8_t>(memberRequest.expected_page_owner_id !=
													 request.expected_page_owner_id) |
								static_cast<uint8_t>(memberRequest.expected_owner_lsn != request.expected_owner_lsn);
						// ZYX_COV_EXCL_NEXT: members come from one pinned source page.
						if (inconsistentSource != 0U) {
							throwRootStoreError(
									FormatError::FERR_INVALID_STATE,
									"Entity Property batch coalescing found inconsistent Property page sources");
						}
						const auto existingIterator = existingLocations.find(member.key);
						const std::optional<EntityPropertyLocation> existing =
								existingIterator == existingLocations.end()
										? std::nullopt
										: std::optional<EntityPropertyLocation>(existingIterator->second);
						if (!memberRequest.upserted_properties.empty()) {
							const EntityPropertyLocation &provisional = memberRequest.upserted_properties.front();
							const SlottedSlotReference provisionalSlot{provisional.page_id, provisional.slot_id,
																	   provisional.slot_generation};
							const SlottedPage memberPage = SlottedPage::open(member.replacement_payload);
							const std::vector<std::byte> stored = memberPage.read(provisionalSlot);
							if (existing.has_value()) {
								// ZYX_COV_EXCL_NEXT: both locations came from the same directory entry.
								if (existing->page_id != request.expected_location.page_id) {
									throwRootStoreError(
											FormatError::FERR_INVALID_STATE,
											"Entity Property batch coalescing found a root on a foreign source page");
								}
								page.erase({existing->page_id, existing->slot_id, existing->slot_generation});
							}
							try {
								const SlottedSlotReference slot = page.insert(stored);
								request.upserted_properties.push_back(
										{member.key, request.expected_location.page_id, slot.slot_id, slot.generation});
								merged.large_values.insert(merged.large_values.end(), member.large_values.begin(),
														   member.large_values.end());
								merged.reused_large_values.insert(merged.reused_large_values.end(),
																  member.reused_large_values.begin(),
																  member.reused_large_values.end());
							} catch (const FormatException &error) {
								// ZYX_COV_EXCL_NEXT: only capacity can fail, and an erased record always fits again.
								if (error.error() != FormatError::FERR_INVALID_STATE || existing.has_value()) {
									throw;
								}
								EntityPropertyRootMutationPlan overflow = member;
								overflow.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
								overflow.page_replacement.reset();
								overflow.replacement_payload.clear();
								overflow.replacement_free_bytes = 0U;
								overflow.encoded_root = stored;
								overflowPlans.push_back(std::move(overflow));
							}
						} else if (!memberRequest.erased_properties.empty()) {
							// ZYX_COV_EXCL_NEXT: erase plans originate from this exact directory entry.
							if (!existing.has_value() || existing->page_id != request.expected_location.page_id) {
								throwRootStoreError(
										FormatError::FERR_INVALID_STATE,
										"Entity Property batch coalescing lost an erased root's source page");
							}
							page.erase({existing->page_id, existing->slot_id, existing->slot_generation});
							request.erased_properties.push_back(member.key);
						} else {
							// ZYX_COV_EXCL_NEXT: relocation plans retain the validated source and canonical encoded
							// root.
							if (!existing.has_value() || existing->page_id != request.expected_location.page_id ||
								memberRequest.relocated_properties.size() != 1U ||
								memberRequest.relocated_properties.front() != member.key ||
								member.encoded_root.empty()) {
								throwRootStoreError(
										FormatError::FERR_INVALID_STATE,
										"Entity Property batch coalescing lost a relocated root's source page");
							}
							page.erase({existing->page_id, existing->slot_id, existing->slot_generation});
							request.relocated_properties.push_back(member.key);
							if (memberOrdinal == ordinal) {
								merged.large_values.insert(merged.large_values.end(), member.large_values.begin(),
														   member.large_values.end());
								merged.reused_large_values.insert(merged.reused_large_values.end(),
																  member.reused_large_values.begin(),
																  member.reused_large_values.end());
							} else {
								EntityPropertyRootMutationPlan destination = member;
								destination.page_replacement.reset();
								destination.replacement_payload.clear();
								destination.replacement_free_bytes = 0U;
								overflowPlans.push_back(std::move(destination));
							}
						}
						if (memberOrdinal != ordinal) {
							absorbedPlans.insert(memberOrdinal);
						}
					}
					if (!request.upserted_properties.empty() || !request.erased_properties.empty() ||
						!request.relocated_properties.empty()) {
						request.retire_page = page.stats().live_slot_count == 0U;
						merged.page_replacement = std::move(request);
						if (merged.page_replacement->retire_page) {
							merged.replacement_payload.clear();
							merged.replacement_free_bytes = 0U;
						} else {
							merged.replacement_payload = page.serialize();
							merged.replacement_free_bytes = page.stats().contiguous_free_bytes;
						}
						coalesced.push_back(std::move(merged));
					}
					for (EntityPropertyRootMutationPlan &overflow: overflowPlans) {
						coalesced.push_back(std::move(overflow));
					}
					continue;
				}
			}
			coalesced.push_back(candidate);
		}
		return coalesced;
	}

	EntityPropertyRootMutationPlan PropertyCollectionRootStore::planEntityRootReplacementWithLargeValueId(
			const MetadataSnapshotPin &snapshot, const PropertyCollectionRoot &inputRoot,
			const PropertyCollectionRootStoreOptions &options, LargeValueId nextLargeValueId,
			const std::map<EntityPropertyKey, EntityPropertyLocation> *existingLocations,
			std::map<size_t, std::optional<SlottedPagePlacement>> *placementCache,
			const std::vector<SegmentId> *currentPropertyPools, EntityPropertyBatchPageCache *pageCache) const {
		validateOptions(options);
		const PropertyCollectionRoot root = canonicalEntityRoot(inputRoot);
		const EntityPropertyKey key = entityKeyForRoot(root);
		EntityPropertyRootMutationPlan plan;
		plan.key = key;
		const std::optional<EntityPropertyLocation> existing = [&]() -> std::optional<EntityPropertyLocation> {
			if (existingLocations != nullptr) {
				const auto iterator = existingLocations->find(key);
				return iterator == existingLocations->end() ? std::nullopt
																				 : std::optional<EntityPropertyLocation>(iterator->second);
			}
			return metadata_.findEntityPropertyLocationOptional(snapshot, key);
		}();
		const auto readPage = [&](PageId pageId) {
			if (pageCache == nullptr) {
				return pages_.read(snapshot, pageId);
			}
			auto [cached, inserted] = pageCache->pages.try_emplace(pageId);
			if (inserted) {
				cached->second.emplace(pages_.read(snapshot, pageId));
			}
			return cached->second->clone();
		};
		enum class ExistingPageMutationKind : uint8_t {
			EPMK_UPSERT = 1U,
			EPMK_ERASE = 2U,
			EPMK_RELOCATE = 3U,
		};

		const auto prepareExistingPage = [&](const EntityPropertyLocation &current, SlottedPage page,
											 std::optional<SlottedSlotReference> inserted,
											 ExistingPageMutationKind mutationKind) {
			PageLocation source;
			CatalogEntry pool;
			if (pageCache == nullptr) {
				source = metadata_.findPageLocation(snapshot, current.page_id);
				pool = metadata_.findCatalog(snapshot, source.segment_id);
			} else {
				auto [location, inserted] = pageCache->locations.try_emplace(current.page_id);
				if (inserted) {
					location->second = metadata_.findPageLocation(snapshot, current.page_id);
					pageCache->pools.emplace(current.page_id,
									 metadata_.findCatalog(snapshot, location->second.segment_id));
				}
				source = location->second;
				pool = pageCache->pools.at(current.page_id);
			}
			const uint8_t invalidSource = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
										  static_cast<uint8_t>(pool.owner_id != options.page_owner_id) |
										  static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										  static_cast<uint8_t>(source.segment_id != pool.segment_id) |
										  static_cast<uint8_t>(source.generation != pool.generation) |
										  static_cast<uint8_t>(page.ownerKind() != PageOwnerKind::POK_PROPERTY) |
										  static_cast<uint8_t>(page.ownerId() != options.page_owner_id) |
										  static_cast<uint8_t>(!page.ownerCommittedLsn().isValid());
			// ZYX_COV_EXCL_NEXT: all fields were authenticated by the same pinned page read.
			if (invalidSource != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property root plan selected a stale or foreign Property page");
			}
			NativeGraphPropertyPageReplacementRequest replacement;
			replacement.expected_pool = pool;
			replacement.expected_location = source;
			replacement.expected_page_owner_id = options.page_owner_id;
			replacement.expected_owner_lsn = page.ownerCommittedLsn();
			replacement.first_slot = options.first_slot;
			replacement.last_slot = options.last_slot;
			if (mutationKind == ExistingPageMutationKind::EPMK_ERASE) {
				replacement.erased_properties = {key};
				replacement.retire_page = page.stats().live_slot_count == 0U;
			} else if (mutationKind == ExistingPageMutationKind::EPMK_RELOCATE) {
				replacement.relocated_properties = {key};
				replacement.retire_page = page.stats().live_slot_count == 0U;
			} else {
				// ZYX_COV_EXCL_NEXT: successful SlottedPage::insert always returns a valid reference.
				if (!inserted.has_value() || !inserted->isValid()) {
					throwRootStoreError(
							FormatError::FERR_INVALID_STATE,
							"Entity Property root plan did not receive a replacement slotted-page reference");
				}
				replacement.upserted_properties = {{key, current.page_id, inserted->slot_id, inserted->generation}};
			}
			plan.kind = mutationKind == ExistingPageMutationKind::EPMK_RELOCATE
								? EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED
								: EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE;
			plan.page_replacement = std::move(replacement);
			if (!plan.page_replacement->retire_page) {
				plan.replacement_payload = page.serialize();
				plan.replacement_free_bytes = page.stats().contiguous_free_bytes;
			}
		};

		if (root.entries.empty()) {
			if (!existing.has_value()) {
				return plan;
			}
			SlottedPage page = readPage(existing->page_id);
			page.erase({existing->page_id, existing->slot_id, existing->slot_generation});
			prepareExistingPage(*existing, std::move(page), std::nullopt, ExistingPageMutationKind::EPMK_ERASE);
			return plan;
		}

		if (existing.has_value() && readEntity(snapshot, key) == root) {
			return plan;
		}
		const std::vector<std::byte> inlineRoot = EntityPropertyCollectionRootCodec::encode(root);
		std::vector<std::byte> encoded = inlineRoot;
		const size_t maximumBytes =
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		if (encoded.size() > maximumBytes) {
			if (large_values_ == nullptr || !nextLargeValueId.isValid()) {
				throwRootStoreError(
						FormatError::FERR_INVALID_STATE,
						"Entity Property root exceeds the inline page limit but has no LargeValue placement");
			}
			const std::vector<std::byte> largeValuePayload = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
			const std::optional<LargeValueRef> reused = large_values_->findContentDuplicate(
					snapshot, largeValuePayload, LargeValueKind::LVK_BYTES, RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE,
					options.maximum_large_value_dedup_candidates);
			const LargeValueRef reference =
					reused.has_value() ? *reused
									 : large_values_->planReference(nextLargeValueId, Generation(1U), largeValuePayload,
																  LargeValueKind::LVK_BYTES,
																	  RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE,
																	  options.large_value_compression);
			EntityPropertyLargeValueReference storedReference;
			storedReference.owner_entity_id = root.owner_entity_id;
			storedReference.owner_entity_type = root.owner_entity_type;
			storedReference.value = reference;
			encoded = EntityPropertyLargeValueReferenceCodec::encode(storedReference);
			if (reused.has_value()) {
				plan.reused_large_values.push_back(reference);
			} else {
				plan.large_values.push_back(
						{.reference = reference,
						 .logical_bytes = largeValuePayload,
						 .first_data_slot = options.first_slot,
						 .last_data_slot = options.last_slot,
						 .first_manifest_slot = options.first_slot,
						 .last_manifest_slot = options.last_slot,
						 .compression = options.large_value_compression,
						 .extent_class = LargeValueExtentClass::LVEC_32K,
						 .planned_data_page_count = large_values_->plannedDataPageCount(
								 largeValuePayload, options.large_value_compression, LargeValueExtentClass::LVEC_32K),
						 .planned_manifest_page_count = large_values_->plannedManifestPageCount(
								 largeValuePayload, options.large_value_compression, LargeValueExtentClass::LVEC_32K)});
			}
			// ZYX_COV_EXCL_NEXT: the fixed LargeValue reference is smaller than an empty slot capacity.
			if (encoded.size() > maximumBytes) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference cannot fit in a Property page");
			}
		}

		if (existing.has_value()) {
			SlottedPage page = readPage(existing->page_id);
			page.erase({existing->page_id, existing->slot_id, existing->slot_generation});
			SlottedSlotReference replacement;
			try {
				replacement = page.insert(encoded);
			} catch (const FormatException &error) {
				// ZYX_COV_EXCL_NEXT: a validated page insertion can fail here only for capacity.
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				plan.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
				plan.encoded_root = encoded;
				prepareExistingPage(*existing, std::move(page), std::nullopt, ExistingPageMutationKind::EPMK_RELOCATE);
				return plan;
			}
			prepareExistingPage(*existing, std::move(page), replacement, ExistingPageMutationKind::EPMK_UPSERT);
			return plan;
		}

		std::vector<SegmentId> pools;
		if (currentPropertyPools != nullptr) {
			pools = *currentPropertyPools;
		} else {
			for (const CatalogEntry &pool: metadata_.findCurrentPropertyPagePools(snapshot, options.page_owner_id)) {
				pools.push_back(pool.segment_id);
			}
		}
		if (pools.empty()) {
			plan.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
			plan.encoded_root = encoded;
			return plan;
		}
		std::optional<SlottedPagePlacement> placement;
		if (!existing.has_value() && placementCache != nullptr) {
			auto [cached, inserted] = placementCache->try_emplace(encoded.size());
			if (inserted) {
				cached->second = pages_.findBestFitPageForRecord(
						snapshot, pools, PageOwnerKind::POK_PROPERTY, static_cast<uint32_t>(encoded.size()));
			}
			placement = cached->second;
		} else {
			placement = pages_.findBestFitPageForRecord(snapshot, pools, PageOwnerKind::POK_PROPERTY,
																 static_cast<uint32_t>(encoded.size()));
		}
		if (!placement.has_value()) {
			plan.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
			plan.encoded_root = encoded;
			return plan;
		}
		const PageLocation source = metadata_.findPageLocation(snapshot, placement->page_id);
		const CatalogEntry pool = metadata_.findCatalog(snapshot, source.segment_id);
		SlottedPage page = readPage(placement->page_id);
		const uint8_t invalidPlacement = static_cast<uint8_t>(source.segment_id != placement->segment_id) |
										 static_cast<uint8_t>(pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
										 static_cast<uint8_t>(pool.owner_id != options.page_owner_id) |
										 static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										 static_cast<uint8_t>(source.generation != pool.generation) |
										 static_cast<uint8_t>(page.ownerKind() != PageOwnerKind::POK_PROPERTY) |
										 static_cast<uint8_t>(page.ownerId() != options.page_owner_id) |
										 static_cast<uint8_t>(!page.ownerCommittedLsn().isValid());
		if (invalidPlacement != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property root plan selected a stale or foreign Property placement");
		}
		SlottedSlotReference inserted;
		try {
			inserted = page.insert(encoded);
		} catch (const FormatException &error) {
			if (error.error() != FormatError::FERR_INVALID_STATE) {
				throw;
			}
			plan.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
			plan.encoded_root = encoded;
			return plan;
		}
		EntityPropertyLocation provisional{key, source.page_id, inserted.slot_id, inserted.generation};
		prepareExistingPage(provisional, std::move(page), inserted, ExistingPageMutationKind::EPMK_UPSERT);
		return plan;
	}

	void PropertyCollectionRootStore::preparePlannedEntityRootReplacement(
			const EntityPropertyRootMutationPlan &plan,
			NativeGraphPropertyPageReplacementMaterialization &materialization, LogSequenceNumber checkpointLsn) const {
		const bool existingPagePlan = plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE;
		// ZYX_COV_EXCL_NEXT: a source-bearing new-page plan is a relocation only when both private planner fields
		// exist.
		const bool relocationPlan = plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED &&
									plan.page_replacement.has_value() && !plan.encoded_root.empty();
		if ((!existingPagePlan && !relocationPlan) || !plan.page_replacement.has_value() || !checkpointLsn.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property preparation requires a page-source plan and checkpoint LSN");
		}
		const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
		const bool samePoolDestination = request.expected_pool.segment_id == materialization.pool.segment_id &&
										 request.expected_pool.physical_slot == materialization.pool.physical_slot &&
										 request.expected_pool.kind == materialization.pool.kind &&
										 request.expected_pool.owner_id == materialization.pool.owner_id &&
										 request.expected_pool.generation == materialization.pool.generation &&
										 request.expected_pool.state == materialization.pool.state &&
										 request.expected_pool.committed_lsn == materialization.pool.committed_lsn;
		const bool hasSourceMutation = !request.upserted_properties.empty() || !request.erased_properties.empty() ||
									   !request.relocated_properties.empty();
		const bool containsPlanKey =
				std::ranges::any_of(
						request.upserted_properties,
						[key = plan.key](const EntityPropertyLocation &location) { return location.key == key; }) ||
				std::ranges::find(request.erased_properties, plan.key) != request.erased_properties.end() ||
				std::ranges::find(request.relocated_properties, plan.key) != request.relocated_properties.end();
		const uint8_t invalidPlan =
				static_cast<uint8_t>(!materialization.destination_is_new_pool && !samePoolDestination) |
				static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
				static_cast<uint8_t>(materialization.pool.owner_id != request.expected_page_owner_id) |
				static_cast<uint8_t>(materialization.destination_is_new_pool &&
									 (materialization.pool.generation != Generation(1U) ||
									  materialization.pool.committed_lsn != checkpointLsn)) |
				static_cast<uint8_t>(materialization.destination_initializes_new_pool &&
									 !materialization.destination_is_new_pool) |
				static_cast<uint8_t>(request.expected_location != materialization.expected_location) |
				static_cast<uint8_t>(request.expected_page_owner_id != materialization.page_owner_id) |
				static_cast<uint8_t>(request.expected_owner_lsn != materialization.expected_owner_lsn) |
				static_cast<uint8_t>(request.retire_page != materialization.retire_page) |
				static_cast<uint8_t>(!plan.key.isValid()) | static_cast<uint8_t>(!hasSourceMutation) |
				static_cast<uint8_t>(!containsPlanKey) |
				static_cast<uint8_t>(existingPagePlan && !plan.encoded_root.empty()) |
				static_cast<uint8_t>(relocationPlan && std::ranges::find(request.relocated_properties, plan.key) ==
															   request.relocated_properties.end()) |
				static_cast<uint8_t>(request.retire_page &&
									 (!request.upserted_properties.empty() ||
									  (request.erased_properties.empty() && request.relocated_properties.empty()) ||
									  !plan.replacement_payload.empty() || plan.replacement_free_bytes != 0U)) |
				static_cast<uint8_t>(!request.retire_page &&
									 (plan.replacement_payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ||
									  plan.replacement_free_bytes >
											  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES));
		if (invalidPlan != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property plan changed before its page COW preparation");
		}
		std::set<uint64_t> mutationKeys;
		for (const EntityPropertyLocation &location: request.upserted_properties) {
			if (!location.key.isValid() || !mutationKeys.insert(location.key.directoryId()).second) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property page-source plan has overlapping owner-key mutations");
			}
		}
		for (const EntityPropertyKey &key: request.erased_properties) {
			if (!key.isValid() || !mutationKeys.insert(key.directoryId()).second) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property page-source plan has overlapping owner-key mutations");
			}
		}
		for (const EntityPropertyKey &key: request.relocated_properties) {
			if (!key.isValid() || !mutationKeys.insert(key.directoryId()).second) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property page-source plan has overlapping owner-key mutations");
			}
		}

		std::optional<SlottedPage> replacementPage;
		if (!request.retire_page) {
			replacementPage.emplace(SlottedPage::open(plan.replacement_payload));
			if (replacementPage->pageId() != request.expected_location.page_id) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property replacement payload has an invalid page identity");
			}
			if (replacementPage->stats().contiguous_free_bytes != plan.replacement_free_bytes) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property replacement payload has invalid free-space metadata");
			}
		}

		std::set<uint64_t> referencedNewLargeValues;
		const auto validateReachableRecord = [&](std::span<const std::byte> stored, const EntityPropertyKey &key) {
			validateStoredEntityPropertyRecord(stored, key, plan.large_values, plan.reused_large_values);
			if (!EntityPropertyLargeValueReferenceCodec::hasMagic(stored)) {
				return;
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(stored);
			const bool newlyMaterialized =
					std::ranges::any_of(plan.large_values, [&reference](const NativeGraphLargeValueMutation &mutation) {
						return mutation.reference == reference.value;
					});
			if (newlyMaterialized && !referencedNewLargeValues.insert(reference.value.value_id.value()).second) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property plan repeats one new LargeValue mutation");
			}
		};
		if (!request.upserted_properties.empty()) {
			for (const EntityPropertyLocation &location: request.upserted_properties) {
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
						static_cast<uint8_t>(location.slot_generation == 0U);
				if (invalidLocation != 0U) {
					throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"native Entity Property existing-page plan has an invalid upsert location");
				}
				const std::vector<std::byte> stored =
						replacementPage->read({location.page_id, location.slot_id, location.slot_generation});
				validateReachableRecord(stored, location.key);
			}
		}
		if (relocationPlan) {
			validateReachableRecord(plan.encoded_root, plan.key);
		}
		if (referencedNewLargeValues.size() != plan.large_values.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property plan has an unreachable LargeValue mutation");
		}
		if (request.upserted_properties.empty() && !relocationPlan &&
			(!plan.large_values.empty() || !plan.reused_large_values.empty())) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property erase plan cannot carry a LargeValue mutation");
		}
		if (request.retire_page) {
			if (materialization.destination_is_new_pool) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property retirement cannot allocate a fresh destination pool");
			}
			const std::array<PageLocation, 0U> noSources{};
			const std::array<SharedPagePoolPreparedReplacement, 0U> noReplacements{};
			const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
			std::array<PageLocation, 0U> noReplacementLocations{};
			std::array<PageLocation, 0U> noAppendLocations{};
			const std::array<SharedPagePoolPreparedRetirement, 1U> retirements{{
					{materialization.expected_location, request.expected_page_owner_id, PageOwnerKind::POK_PROPERTY,
					 request.expected_owner_lsn},
			}};
			pages_.prepareMutationPages(materialization.pool, noSources, noReplacements, noAppends,
										noReplacementLocations, noAppendLocations, checkpointLsn, retirements);
			return;
		}
		const PageOwnerHeader owner{.page_id = request.expected_location.page_id,
									.owner_id = request.expected_page_owner_id,
									.kind = PageOwnerKind::POK_PROPERTY,
									.payload_bytes = static_cast<uint32_t>(plan.replacement_payload.size()),
									.generation = materialization.pool.generation,
									.committed_lsn = checkpointLsn};
		const SharedPagePoolPreparedReplacement replacement{owner, request.expected_owner_lsn, plan.replacement_payload,
															plan.replacement_free_bytes};
		std::array<PageLocation, 1U> replacementLocations{materialization.replacement};
		if (materialization.destination_is_new_pool && materialization.destination_initializes_new_pool) {
			const std::array<SharedPagePoolPreparedAppend, 1U> initial{{
					{owner, plan.replacement_payload, plan.replacement_free_bytes},
			}};
			pages_.prepareNewPoolPages(materialization.pool, initial, replacementLocations, checkpointLsn);
			const std::array<PageLocation, 0U> noSources{};
			const std::array<SharedPagePoolPreparedReplacement, 0U> noReplacements{};
			const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
			std::array<PageLocation, 0U> noReplacementLocations{};
			std::array<PageLocation, 0U> noAppendLocations{};
			const std::array<SharedPagePoolPreparedRetirement, 1U> retirements{{
					{materialization.expected_location, request.expected_page_owner_id, PageOwnerKind::POK_PROPERTY,
					 request.expected_owner_lsn},
			}};
			pages_.prepareMutationPages(request.expected_pool, noSources, noReplacements, noAppends,
										noReplacementLocations, noAppendLocations, checkpointLsn, retirements);
		} else if (materialization.destination_is_new_pool) {
			const SharedPagePoolPreparedCrossPoolReplacement crossPoolReplacement{
					request.expected_pool,	  materialization.expected_location, request.expected_owner_lsn, owner,
					plan.replacement_payload, plan.replacement_free_bytes};
			pages_.prepareCrossPoolReplacementPages(materialization.pool, std::span(&crossPoolReplacement, 1U),
													replacementLocations, checkpointLsn);
		} else {
			const std::array<PageLocation, 1U> expected{materialization.expected_location};
			const std::array<SharedPagePoolPreparedReplacement, 1U> replacements{replacement};
			const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
			std::array<PageLocation, 0U> noAppendLocations{};
			pages_.prepareMutationPages(materialization.pool, expected, replacements, noAppends, replacementLocations,
										noAppendLocations, checkpointLsn);
		}
		materialization.replacement = replacementLocations.front();
	}

	void PropertyCollectionRootStore::preparePlannedLargeValue(const NativeGraphLargeValueMutation &mutation,
															   LargeValueMaterializationResult &materialization,
															   LogSequenceNumber checkpointLsn) const {
		if (large_values_ == nullptr) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property LargeValue preparation requires a configured LargeValue store");
		}
		large_values_->prepareNativeGraphMaterialization(mutation, materialization, checkpointLsn);
	}

	void PropertyCollectionRootStore::preparePlannedEntityRootPageBatch(
			std::span<const EntityPropertyRootMutationPlan> plans,
			std::span<const std::vector<size_t>> pagePlanOrdinals,
			NativeGraphPropertyPageBatchMaterialization &materialization, LogSequenceNumber checkpointLsn) const {
		const size_t maximumBytes =
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		const bool validBatchKind =
				materialization.kind == NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL ||
				materialization.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		const uint8_t invalidMaterialization =
				static_cast<uint8_t>(plans.empty()) | static_cast<uint8_t>(pagePlanOrdinals.empty()) |
				static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!validBatchKind) |
				static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
				static_cast<uint8_t>(materialization.pool.owner_id != materialization.page_owner_id) |
				static_cast<uint8_t>(!materialization.pool.generation.isValid()) |
				static_cast<uint8_t>(materialization.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL &&
									 materialization.pool.generation != Generation(1U)) |
				static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!materialization.pool.committed_lsn.isValid()) |
				static_cast<uint8_t>(materialization.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL &&
									 materialization.pool.committed_lsn != checkpointLsn) |
				static_cast<uint8_t>(materialization.pages.size() != pagePlanOrdinals.size()) |
				static_cast<uint8_t>(materialization.pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidMaterialization != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"native Entity Property page-batch plan changed before preparation");
		}
		std::vector<SharedPagePoolPreparedAppend> preparedPages;
		std::vector<PageLocation> locations;
		preparedPages.reserve(materialization.pages.size());
		locations.reserve(materialization.pages.size());
		uint64_t previousKey = 0U;
		for (size_t pageOrdinal = 0U; pageOrdinal < pagePlanOrdinals.size(); ++pageOrdinal) {
			const std::vector<size_t> &planOrdinals = pagePlanOrdinals[pageOrdinal];
			NativeGraphPropertyPageInitialMaterialization &initial = materialization.pages[pageOrdinal];
			const uint8_t invalidPage =
					static_cast<uint8_t>(planOrdinals.empty()) |
					static_cast<uint8_t>(initial.location.page_id.isValid() == false) |
					static_cast<uint8_t>(initial.location.segment_id != materialization.pool.segment_id) |
					static_cast<uint8_t>(initial.location.generation != materialization.pool.generation) |
					static_cast<uint8_t>(initial.location.page_index != 0U) |
					static_cast<uint8_t>(initial.location.free_bytes != 0U) |
					static_cast<uint8_t>(initial.upserted_properties.size() != planOrdinals.size());
			if (invalidPage != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"native Entity Property page batch has an invalid provisional page");
			}
			const PageOwnerHeader owner{.page_id = initial.location.page_id,
										.owner_id = materialization.page_owner_id,
										.kind = PageOwnerKind::POK_PROPERTY,
										.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										.generation = materialization.pool.generation,
										.committed_lsn = checkpointLsn};
			SlottedPage page = SlottedPage::create(owner);
			for (size_t index = 0U; index < planOrdinals.size(); ++index) {
				if (planOrdinals[index] >= plans.size()) {
					throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"native Entity Property page-batch ordinal exceeds its pinned plan array");
				}
				const EntityPropertyRootMutationPlan &plan = plans[planOrdinals[index]];
				const EntityPropertyLocation &expected = initial.upserted_properties[index];
				const uint64_t directoryId = plan.key.isValid() ? plan.key.directoryId() : 0U;
				const bool relocationPlan = plan.page_replacement.has_value() &&
											std::ranges::find(plan.page_replacement->relocated_properties, plan.key) !=
													plan.page_replacement->relocated_properties.end();
				const bool invalidRelocationSource =
						relocationPlan &&
						(plan.page_replacement->retire_page
								 ? (!plan.page_replacement->upserted_properties.empty() ||
									(plan.page_replacement->erased_properties.empty() &&
									 plan.page_replacement->relocated_properties.empty()) ||
									!plan.replacement_payload.empty() || plan.replacement_free_bytes != 0U)
								 : (plan.replacement_payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ||
									plan.replacement_free_bytes >
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES));
				const uint8_t invalidPlan =
						static_cast<uint8_t>(plan.kind !=
											 EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED) |
						static_cast<uint8_t>(!plan.key.isValid()) | static_cast<uint8_t>(directoryId <= previousKey) |
						static_cast<uint8_t>(plan.page_replacement.has_value() && !relocationPlan) |
						static_cast<uint8_t>(!relocationPlan && !plan.replacement_payload.empty()) |
						static_cast<uint8_t>(!relocationPlan && plan.replacement_free_bytes != 0U) |
						static_cast<uint8_t>(invalidRelocationSource) |
						static_cast<uint8_t>(plan.encoded_root.empty()) |
						static_cast<uint8_t>(plan.encoded_root.size() > maximumBytes) |
						static_cast<uint8_t>(expected.key != plan.key) |
						static_cast<uint8_t>(expected.page_id != initial.location.page_id) |
						static_cast<uint8_t>(expected.slot_id != std::numeric_limits<uint16_t>::max()) |
						static_cast<uint8_t>(expected.slot_generation != 0U);
				if (invalidPlan != 0U) {
					throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"native Entity Property page batch has an invalid owner-keyed root");
				}
				validateStoredEntityPropertyRecord(plan.encoded_root, plan.key, plan.large_values,
												   plan.reused_large_values);
				const SlottedSlotReference slot = page.insert(plan.encoded_root);
				initial.upserted_properties[index] = {plan.key, initial.location.page_id, slot.slot_id,
													  slot.generation};
				previousKey = directoryId;
			}
			preparedPages.push_back({owner, page.serialize(), page.stats().contiguous_free_bytes});
			locations.push_back(initial.location);
		}
		if (materialization.kind == NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL) {
			pages_.prepareAppendPages(materialization.pool, preparedPages, locations, checkpointLsn);
		} else {
			pages_.prepareNewPoolPages(materialization.pool, preparedPages, locations, checkpointLsn);
		}
		for (size_t pageOrdinal = 0U; pageOrdinal < materialization.pages.size(); ++pageOrdinal) {
			NativeGraphPropertyPageInitialMaterialization &initial = materialization.pages[pageOrdinal];
			initial.location = locations[pageOrdinal];
			for (EntityPropertyLocation &property: initial.upserted_properties) {
				property.page_id = initial.location.page_id;
			}
		}
	}


} // namespace graph::storage
