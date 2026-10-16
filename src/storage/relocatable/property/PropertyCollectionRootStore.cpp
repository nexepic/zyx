/**
 * @file PropertyCollectionRootStore.cpp
 * @brief Slotted-page placement and directory publication for Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/PropertyRowChunkPolicy.hpp"

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

#include "storage/relocatable/property/PropertyBatchOrder.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {
		[[nodiscard]] size_t pageReadAdmissionBytes(const MetadataSnapshotPin &snapshot) {
			return snapshot.readContext() && snapshot.readContext()->memoryOwner() ? 0U : RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		}


		template<typename Result>
		Result recordSelectedValues(Result result) {
			if (debug::PerfTrace::areValuesEnabled()) {
				int64_t count = 0;
				if constexpr (requires { result.has_value(); }) {
					count = result.has_value() ? 1 : 0;
				} else {
					for (const auto &entry : result) {
						if (entry.has_value() && count < std::numeric_limits<int64_t>::max()) ++count;
					}
				}
				debug::PerfTrace::addValue("native_graph.read.selected_values_decoded", count);
			}
			return result;
		}

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

		struct LargeValuePropertyDirectory {
			LargeValueManifest manifest;
			std::vector<std::byte> header_and_directory;
			std::vector<EntityPropertyLargeValueEntryLocation> locations;
			LargeValueStore::RangeReadCache range_cache;
		};

		size_t checkedLargeValueBuffers(size_t retained, size_t additional) {
			if (additional > std::numeric_limits<size_t>::max() - retained)
				throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "selected LargeValue buffers overflowed");
			return retained + additional;
		}

		[[nodiscard]] size_t manifestBufferBytes(const LargeValueManifest &manifest) {
			return checkedLargeValueBuffers(manifest.media_type.capacity(),
					checkedLargeValueBuffers(manifest.chunks.capacity() * sizeof(LargeValueChunk),
							manifest.children.capacity() * sizeof(LargeValueManifestChild)));
		}

		[[nodiscard]] LargeValuePropertyDirectory readLargeValuePropertyDirectory(
				const LargeValueStore &store, const MetadataSnapshotPin &snapshot, const LargeValueRef &reference,
				const SelectedPropertyReadAdmission *bufferAdmission = nullptr,
				const CancellationCheckpoint *cancellation = nullptr) {
			if (cancellation) cancellation->check();
			debug::PerfTrace::addValue("native_graph.read.property_large_value_directory_reads", 1);
			LargeValueManifest manifest = store.manifest(snapshot, reference,
					bufferAdmission ? *bufferAdmission : SelectedPropertyReadAdmission{});
			const size_t manifestBytes = manifestBufferBytes(manifest);
			const SelectedPropertyReadAdmission admitHeader = [&](size_t bytes) {
				if (bufferAdmission) (*bufferAdmission)(checkedLargeValueBuffers(manifestBytes, bytes));
			};
			if (manifest.logical_bytes < RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES) {
				throwRootStoreError(FormatError::FERR_BUFFER_TOO_SMALL,
									"Entity Property LargeValue payload is truncated");
			}
			LargeValuePropertyDirectory result;
			const std::vector<std::byte> header = store.readRangeFromManifestCached(
					snapshot, manifest, 0U, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES, result.range_cache,
				bufferAdmission ? admitHeader : SelectedPropertyReadAdmission{});
			const uint32_t entryCount = LittleEndianCodec::readU32(header, 8U);
			const uint64_t directoryBytes = static_cast<uint64_t>(entryCount) * sizeof(uint32_t) * 2U;
			const uint64_t headerBytes = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + directoryBytes;
			if (headerBytes > manifest.logical_bytes) {
				throwRootStoreError(FormatError::FERR_BUFFER_TOO_SMALL,
									"Entity Property LargeValue directory exceeds its manifest");
			}
			const SelectedPropertyReadAdmission admitDirectoryRead = [&](size_t bytes) {
				if (bufferAdmission) (*bufferAdmission)(checkedLargeValueBuffers(checkedLargeValueBuffers(manifestBytes, header.capacity()), bytes));
			};
			result.header_and_directory = store.readRangeFromManifestCached(snapshot, manifest, 0U, headerBytes, result.range_cache,
				bufferAdmission ? admitDirectoryRead : SelectedPropertyReadAdmission{});
			const SelectedPropertyReadAdmission admitLocations = [&](size_t bytes) {
				if (bufferAdmission) (*bufferAdmission)(checkedLargeValueBuffers(result.range_cache.retainedBytes(), checkedLargeValueBuffers(checkedLargeValueBuffers(checkedLargeValueBuffers(manifestBytes, header.capacity()), result.header_and_directory.capacity()), bytes)));
			};
			result.locations = EntityPropertyLargeValuePayloadCodec::decodeDirectory(result.header_and_directory, bufferAdmission ? &admitLocations : nullptr, cancellation);
			const uint64_t payloadBytes = LittleEndianCodec::readU32(result.header_and_directory, 12U);
			if (headerBytes + payloadBytes - directoryBytes != manifest.logical_bytes) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property LargeValue manifest length disagrees with its payload header");
			}
			result.manifest = std::move(manifest);
			if (bufferAdmission) (*bufferAdmission)(checkedLargeValueBuffers(manifestBytes, checkedLargeValueBuffers(result.range_cache.retainedBytes(), checkedLargeValueBuffers(result.header_and_directory.capacity(), result.locations.capacity() * sizeof(EntityPropertyLargeValueEntryLocation)))));
			return result;
		}

		[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>> readSelectedLargeValueProperties(
				const LargeValueStore &store, const MetadataSnapshotPin &snapshot, const LargeValueRef &reference,
				LargeValuePropertyDirectory &directory, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *bufferAdmission = nullptr) {
			std::vector<std::optional<PropertyCollectionRootEntry>> result;
			const size_t directoryBytes = checkedLargeValueBuffers(manifestBufferBytes(directory.manifest), checkedLargeValueBuffers(directory.header_and_directory.capacity(), directory.locations.capacity() * sizeof(EntityPropertyLargeValueEntryLocation)));
			const auto admit = [&](size_t bytes) { if (bufferAdmission) (*bufferAdmission)(checkedLargeValueBuffers(directoryBytes, bytes)); };
			if (keyTokens.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<PropertyCollectionRootEntry>))
				throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "selected LargeValue result slots overflowed");
			admit(checkedLargeValueBuffers(keyTokens.size() * sizeof(std::optional<PropertyCollectionRootEntry>), directory.range_cache.retainedBytes()));
			result.reserve(keyTokens.size());
			const size_t resultBytes = result.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>);
			admit(checkedLargeValueBuffers(resultBytes, directory.range_cache.retainedBytes()));
			for (const uint32_t token: keyTokens) {
				const auto found = std::ranges::lower_bound(directory.locations, token, {},
																	&EntityPropertyLargeValueEntryLocation::key_token);
				if (found == directory.locations.end() || found->key_token != token) {
					result.emplace_back(std::nullopt);
					continue;
				}
				if (admission != nullptr) (*admission)(found->length);
				const uint64_t offset = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + found->offset;
				const std::vector<std::byte> entry = store.readRangeFromManifestCached(
						snapshot, directory.manifest, offset, found->length, directory.range_cache,
					bufferAdmission ? SelectedPropertyReadAdmission([&](size_t bytes) { admit(checkedLargeValueBuffers(resultBytes, bytes)); }) : SelectedPropertyReadAdmission{});
				result.emplace_back(EntityPropertyLargeValuePayloadCodec::decodeEntry(entry, token));
			}
			admit(checkedLargeValueBuffers(resultBytes, directory.range_cache.retainedBytes()));
			return result;
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
																   const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) const {
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property read requires a valid logical owner key");
		}
		const EntityPropertyLocation location = metadata_.findEntityPropertyLocation(snapshot, key);
		const SlottedPage page = pages_.read(snapshot, location.page_id);
		const std::vector<std::byte> bytes = page.read({location.page_id, location.slot_id, location.slot_generation});
		return decodeEntityRoot(snapshot, key, location, page.pageId(), page.ownerKind(), bytes, admission, slotsAdmission);
	}

	std::optional<PropertyCollectionRoot>
	PropertyCollectionRootStore::readEntityOptional(const MetadataSnapshotPin &snapshot,
													const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) const {
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
		const std::vector<std::byte> bytes =
				page.read({location->page_id, location->slot_id, location->slot_generation});
		return decodeEntityRoot(snapshot, key, *location, page.pageId(), page.ownerKind(), bytes, admission, slotsAdmission);
	}

	std::optional<PropertyCollectionRootEntry>
	PropertyCollectionRootStore::readEntityPropertyOptional(const MetadataSnapshotPin &snapshot,
																		 const EntityPropertyKey &key,
																		 const uint32_t keyToken) const {
		if (!key.isValid() || keyToken == 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"selective Entity Property read requires a valid owner key and nonzero token");
		}
		const std::optional<EntityPropertyLocation> location =
				metadata_.findEntityPropertyLocationOptional(snapshot, key);
		if (!location.has_value()) {
			return std::nullopt;
		}
		if (std::optional<SlottedPagePinnedRead> pinned = pages_.readPinned(snapshot, location->page_id);
			pinned.has_value()) {
			return decodeSelectedEntityProperty(snapshot, key, *location, pinned->page.pageId(),
															pinned->page.ownerKind(),
															pinned->page.read({location->page_id, location->slot_id,
																					location->slot_generation}),
															keyToken);
		}
		const SlottedPage page = pages_.read(snapshot, location->page_id);
		const std::vector<std::byte> bytes =
				page.read({location->page_id, location->slot_id, location->slot_generation});
		return decodeSelectedEntityProperty(snapshot, key, *location, page.pageId(), page.ownerKind(), bytes, keyToken);
	}

	std::optional<bool> PropertyCollectionRootStore::readEntityPropertyExists(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key, const uint32_t keyToken, const bool requireNonNull) const {
		if (!key.isValid() || keyToken == 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property presence read requires a valid owner key and nonzero token");
		}
		const std::optional<EntityPropertyLocation> location =
			metadata_.findEntityPropertyLocationOptional(snapshot, key);
		if (!location.has_value()) {
			return false;
		}
		if (std::optional<SlottedPagePinnedRead> pinned = pages_.readPinned(snapshot, location->page_id);
				pinned.has_value()) {
			return decodeEntityPropertyExists(snapshot, key, *location, pinned->page.pageId(), pinned->page.ownerKind(),
														pinned->page.read({location->page_id, location->slot_id, location->slot_generation}), keyToken, requireNonNull);
		}
		const SlottedPage page = pages_.read(snapshot, location->page_id);
		const std::vector<std::byte> bytes =
				page.read({location->page_id, location->slot_id, location->slot_generation});
		return decodeEntityPropertyExists(snapshot, key, *location, page.pageId(), page.ownerKind(), bytes, keyToken, requireNonNull);
	}

	void PropertyCollectionRootStore::readEntityPropertiesOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys, const uint32_t keyToken,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!metadata_.ownsSnapshot(snapshot) || keyToken == 0U || keys.size() != results.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"sorted selected Entity Property reads require one snapshot, token, and result per owner");
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		if (keys.empty()) {
			return;
		}

		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key: keys) {
			const uint64_t directoryId = key.directoryId();
			if (directoryId <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"sorted selected Entity Property reads require strictly ordered valid owners");
			}
			previousDirectoryId = directoryId;
		}

		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		metadata_.findEntityPropertyLocationsSorted(snapshot, keys, locations, cancellation);
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation);

		std::optional<SlottedPagePinnedRead> pinned;
		std::optional<SlottedPage> copied;
		PageId currentPage{};
		for (const size_t index: present) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const EntityPropertyLocation &location = *locations[index];
			if (location.page_id != currentPage) {
				pinned.reset();
				copied.reset();
				currentPage = location.page_id;
				pinned = pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (!pinned.has_value()) {
					copied.emplace(pages_.read(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL));
				}
				debug::PerfTrace::addValue("native_graph.read.property_selected_batch_pages", 1);
			}
			if (pinned.has_value()) {
				const std::span<const std::byte> bytes =
						pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
				results[index] = decodeSelectedEntityProperty(snapshot, keys[index], location, pinned->page.pageId(),
																			pinned->page.ownerKind(), bytes, keyToken);
			} else {
				const std::vector<std::byte> bytes =
						copied->read({location.page_id, location.slot_id, location.slot_generation});
				results[index] = decodeSelectedEntityProperty(snapshot, keys[index], location, copied->pageId(),
																			copied->ownerKind(), bytes, keyToken);
			}
		}
		debug::PerfTrace::addValue("native_graph.read.property_selected_batch_owners",
									 static_cast<int64_t>(keys.size()));
	}

	void PropertyCollectionRootStore::readEntityPropertiesOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<const uint32_t> keyTokens,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!metadata_.ownsSnapshot(snapshot) || keyTokens.empty() ||
			keys.size() > std::numeric_limits<size_t>::max() / keyTokens.size() ||
			results.size() != keys.size() * keyTokens.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"multi-column Entity Property reads require one snapshot and a rectangular result shape");
		}
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key: keys) {
			const uint64_t directoryId = key.directoryId();
			if (directoryId <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"multi-column Entity Property reads require strictly ordered valid owners");
			}
			previousDirectoryId = directoryId;
		}
		uint32_t previousToken = 0U;
		for (const uint32_t keyToken: keyTokens) {
			if (keyToken == 0U || keyToken <= previousToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"multi-column Entity Property reads require strictly increasing nonzero tokens");
			}
			previousToken = keyToken;
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		if (keys.empty()) {
			return;
		}

		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		metadata_.findEntityPropertyLocationsSorted(snapshot, keys, locations, cancellation);
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation);

		std::optional<SlottedPagePinnedRead> pinned;
		std::optional<SlottedPage> copied;
		PageId currentPage{};
		// A single requested token is the dominant predicate/projection shape. Keep
		// this path scalar so each owner avoids allocating a temporary one-element
		// vector before the caller places the value in its column.
		const bool scalarSelection = keyTokens.size() == 1U;
		for (const size_t index: present) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const EntityPropertyLocation &location = *locations[index];
			if (location.page_id != currentPage) {
				pinned.reset();
				copied.reset();
				currentPage = location.page_id;
				pinned = pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (!pinned.has_value()) {
					copied.emplace(pages_.read(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL));
				}
				debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_pages", 1);
			}
			std::vector<std::byte> copiedBytes;
			std::span<const std::byte> bytes;
			if (pinned.has_value()) {
				bytes = pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
			} else {
				copiedBytes = copied->read({location.page_id, location.slot_id, location.slot_generation});
				bytes = copiedBytes;
			}
			const PageId pageId = pinned.has_value() ? pinned->page.pageId() : copied->pageId();
			const PageOwnerKind ownerKind = pinned.has_value() ? pinned->page.ownerKind() : copied->ownerKind();
			if (scalarSelection) {
				std::optional<PropertyCollectionRootEntry> selected =
						decodeSelectedEntityProperty(snapshot, keys[index], location, pageId, ownerKind, bytes, keyTokens.front());
				results[index] = std::move(selected);
				continue;
			}
			std::vector<std::optional<PropertyCollectionRootEntry>> selected =
					decodeSelectedEntityProperties(snapshot, keys[index], location, pageId, ownerKind, bytes, keyTokens);
			if (selected.size() != keyTokens.size()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"multi-column Entity Property decoder returned an invalid result shape");
			}
			std::move(selected.begin(), selected.end(), results.begin() + index * keyTokens.size());
		}
		debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_owners",
									 static_cast<int64_t>(keys.size()));
	}

	std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
	PropertyCollectionRootStore::decodeSelectedEntityPropertiesAndCount(
		const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
		const EntityPropertyLocation &location, PageId pageId, PageOwnerKind ownerKind,
		std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
		const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *transientBufferAdmission) const {
		const uint8_t invalidLocation = static_cast<uint8_t>(location.key != key) |
				static_cast<uint8_t>(location.page_id != pageId) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_PROPERTY);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"combined Entity Property location does not resolve to its Property page");
		}
		std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>> decoded;
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			if (referenceKey != key || reference.value.kind != LargeValueKind::LVK_BYTES ||
					reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference has an invalid owner or content type");
			}
			LargeValuePropertyDirectory directory =
					readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value, transientBufferAdmission);
			decoded = {directory.locations.size(),
						readSelectedLargeValueProperties(*large_values_, snapshot, reference.value, directory, keyTokens, admission, transientBufferAdmission)};
		} else {
			const PropertyCollectionRoot owner = EntityPropertyCollectionRootCodec::decodeOwner(bytes);
			if (entityKeyForRoot(owner) != key) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property location resolves to another logical owner");
			}
			decoded = EntityPropertyCollectionRootCodec::decodeSelectedAndCount(bytes, keyTokens, admission, transientBufferAdmission);
		}
		if (transientBufferAdmission) (*transientBufferAdmission)(decoded.second.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>));
		decoded.second = recordSelectedValues(std::move(decoded.second));
		return decoded;
	}

	void PropertyCollectionRootStore::readEntityPropertiesAndCountsOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<const uint32_t> keyTokens,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const std::span<std::optional<size_t>> counts,
			const CancellationCheckpoint *cancellation, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *transientBufferAdmission) const {
		if (!metadata_.ownsSnapshot(snapshot) || keyTokens.empty() ||
			keys.size() > std::numeric_limits<size_t>::max() / keyTokens.size() ||
			results.size() != keys.size() * keyTokens.size() || counts.size() != keys.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"combined Entity Property reads require one snapshot and rectangular result shapes");
		}
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key: keys) {
			if (!key.isValid() || key.directoryId() <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"combined Entity Property reads require strictly ordered valid owners");
			}
			previousDirectoryId = key.directoryId();
		}
		uint32_t previousToken = 0U;
		for (const uint32_t token: keyTokens) {
			if (token == 0U || token <= previousToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"combined Entity Property reads require strictly increasing nonzero tokens");
			}
			previousToken = token;
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		std::fill(counts.begin(), counts.end(), std::nullopt);
		if (keys.empty()) {
			return;
		}

		if (keys.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<EntityPropertyLocation>))
			throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property owner batch slots overflowed");
		size_t ownerBytes = keys.size() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		ownerBytes = locations.capacity() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		const SelectedPropertyReadAdmission admitTransient = [&](size_t bytes) {
			if (transientBufferAdmission) (*transientBufferAdmission)(checkedLargeValueBuffers(ownerBytes, bytes));
		};
		metadata_.findEntityPropertyLocationsSorted(snapshot, keys, locations, cancellation);
		for (size_t index = 0U; index < locations.size(); ++index)
			if (!locations[index].has_value()) counts[index] = size_t{0U};
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation, transientBufferAdmission ? &admitTransient : nullptr);
		ownerBytes = checkedLargeValueBuffers(ownerBytes, present.capacity() * sizeof(size_t));

		std::optional<SlottedPagePinnedRead> pinned;
		std::optional<SlottedPage> copied;
		PageId currentPage{};
		for (const size_t index: present) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const EntityPropertyLocation &location = *locations[index];
			if (location.page_id != currentPage) {
				pinned.reset();
				copied.reset();
				currentPage = location.page_id;
				pinned = pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (!pinned.has_value()) {
					copied.emplace(pages_.read(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL));
				}
				debug::PerfTrace::addValue("native_graph.read.property_combined_batch_pages", 1);
			}
			std::vector<std::byte> copiedBytes;
			std::span<const std::byte> bytes;
			if (pinned.has_value()) {
				bytes = pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
			} else {
				copiedBytes = copied->read({location.page_id, location.slot_id, location.slot_generation});
				bytes = copiedBytes;
			}
			const PageId pageId = pinned.has_value() ? pinned->page.pageId() : copied->pageId();
			const PageOwnerKind ownerKind = pinned.has_value() ? pinned->page.ownerKind() : copied->ownerKind();
			auto decoded = decodeSelectedEntityPropertiesAndCount(snapshot, keys[index], location,
				pageId, ownerKind, bytes, keyTokens, admission, transientBufferAdmission ? &admitTransient : nullptr);
			std::vector<std::optional<PropertyCollectionRootEntry>> selected = std::move(decoded.second);
			if (selected.size() != keyTokens.size()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"combined Entity Property decoder returned an invalid result shape");
			}
			if (cancellation != nullptr) cancellation->check();
			counts[index] = decoded.first;
			std::move(selected.begin(), selected.end(), results.begin() + index * keyTokens.size());
		}
		std::vector<std::optional<EntityPropertyLocation>>().swap(locations);
		std::vector<size_t>().swap(present);
		if (transientBufferAdmission) (*transientBufferAdmission)(0U);
		debug::PerfTrace::addValue("native_graph.read.property_combined_batch_owners",
								 static_cast<int64_t>(keys.size()));
		// Preserve the established scalar/column diagnostics as compatibility
		// aliases while the combined operation exposes its own structural counter.
		debug::PerfTrace::addValue("native_graph.read.property_selected_batch_owners",
								 static_cast<int64_t>(keys.size()));
		debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_owners",
								 static_cast<int64_t>(keys.size()));
	}

	size_t PropertyCollectionRootStore::decodeEntityPropertyCount(
			const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
			const EntityPropertyLocation &location, const PageId pageId, const PageOwnerKind ownerKind,
			const std::span<const std::byte> bytes, const SelectedPropertyReadAdmission *transientBufferAdmission) const {
		const uint8_t invalidLocation = static_cast<uint8_t>(location.key != key) |
				static_cast<uint8_t>(location.page_id != pageId) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_PROPERTY);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property count location does not resolve to its Property page");
		}
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			if (referenceKey != key || reference.value.kind != LargeValueKind::LVK_BYTES ||
				reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
										"Entity Property LargeValue reference has an invalid owner or content type");
			}
			size_t count;
			{
				auto directory = readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value, transientBufferAdmission);
				count = directory.locations.size();
			}
			if (transientBufferAdmission) (*transientBufferAdmission)(0U);
			return count;
		}
		const PropertyCollectionRoot owner = EntityPropertyCollectionRootCodec::decodeOwner(bytes);
		if (entityKeyForRoot(owner) != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property count location resolves to another logical owner");
		}
		return EntityPropertyCollectionRootCodec::entryCount(bytes);
	}

	std::optional<size_t> PropertyCollectionRootStore::readEntityPropertyCount(
			const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key) const {
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property count read requires a valid logical owner key");
		}
		const std::optional<EntityPropertyLocation> location =
				metadata_.findEntityPropertyLocationOptional(snapshot, key);
		if (!location.has_value()) {
			return 0U;
		}
		if (std::optional<SlottedPagePinnedRead> pinned = pages_.readPinned(snapshot, location->page_id);
				pinned.has_value()) {
			return decodeEntityPropertyCount(snapshot, key, *location, pinned->page.pageId(), pinned->page.ownerKind(),
																pinned->page.read({location->page_id, location->slot_id, location->slot_generation}));
		}
		const SlottedPage page = pages_.read(snapshot, location->page_id);
		const std::vector<std::byte> bytes =
				page.read({location->page_id, location->slot_id, location->slot_generation});
		return decodeEntityPropertyCount(snapshot, key, *location, page.pageId(), page.ownerKind(), bytes);
	}

	std::vector<uint32_t> PropertyCollectionRootStore::readEntityPropertyTokens(
		const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
		const SelectedPropertyReadAdmission &tokenAdmission,
		const SelectedPropertyReadAdmission &transientAdmission, const CancellationCheckpoint *cancellation) const {
		if (cancellation) cancellation->check();
		if (!metadata_.ownsSnapshot(snapshot) || !key.isValid() || !tokenAdmission || !transientAdmission) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
				"Entity Property token read requires an owned snapshot, valid owner and admissions");
		}
		const auto location = metadata_.findEntityPropertyLocationOptional(snapshot, key);
		if (!location) return {};
		const auto decode = [&](PageId pageId, PageOwnerKind ownerKind, std::span<const std::byte> bytes) {
			if (location->key != key || location->page_id != pageId || ownerKind != PageOwnerKind::POK_PROPERTY) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
					"Entity Property token location does not resolve to its Property page");
			}
			if (!EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
				if (entityKeyForRoot(EntityPropertyCollectionRootCodec::decodeOwner(bytes)) != key)
					throwRootStoreError(FormatError::FERR_INVALID_STATE, "Entity Property token root has another owner");
				return EntityPropertyCollectionRootCodec::decodeTokens(bytes, &tokenAdmission, cancellation);
			}
			if (large_values_ == nullptr)
				throwRootStoreError(FormatError::FERR_INVALID_STATE, "Entity Property token read requires a LargeValue store");
			const auto reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			if (entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}}) != key ||
				reference.value.kind != LargeValueKind::LVK_BYTES ||
				reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE)
				throwRootStoreError(FormatError::FERR_INVALID_STATE, "Entity Property token reference has invalid ownership or content type");
			std::vector<uint32_t> tokens;
			{
				auto directory = readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value, &transientAdmission, cancellation);
				tokenAdmission(directory.locations.size() * sizeof(uint32_t));
				if (cancellation) cancellation->check();
				tokens.reserve(directory.locations.size());
				tokenAdmission(tokens.capacity() * sizeof(uint32_t));
				if (cancellation) cancellation->check();
				for (const auto &entry : directory.locations) {
					if (cancellation) cancellation->check();
					tokens.push_back(entry.key_token);
				}
			}
			transientAdmission(0U);
			return tokens;
		};
		if (auto pinned = pages_.readPinned(snapshot, location->page_id); pinned) {
			return decode(pinned->page.pageId(), pinned->page.ownerKind(),
				pinned->page.read({location->page_id, location->slot_id, location->slot_generation}));
		}
		const auto page = pages_.read(snapshot, location->page_id);
		const auto bytes = page.read({location->page_id, location->slot_id, location->slot_generation});
		return decode(page.pageId(), page.ownerKind(), bytes);
	}

	void PropertyCollectionRootStore::readEntityPropertyCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<std::optional<size_t>> results, const CancellationCheckpoint *cancellation) const {
		if (!metadata_.ownsSnapshot(snapshot) || keys.size() != results.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"sorted Entity Property counts require one snapshot and result per owner");
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key: keys) {
			if (!key.isValid() || key.directoryId() <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"sorted Entity Property counts require strictly ordered valid owners");
			}
			previousDirectoryId = key.directoryId();
		}
		if (keys.empty()) {
			return;
		}

		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		metadata_.findEntityPropertyLocationsSorted(snapshot, keys, locations, cancellation);
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation);

		std::optional<SlottedPagePinnedRead> pinned;
		std::optional<SlottedPage> copied;
		PageId currentPage{};
		for (const size_t index: present) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const EntityPropertyLocation &location = *locations[index];
			if (location.page_id != currentPage) {
				pinned.reset();
				copied.reset();
				currentPage = location.page_id;
				pinned = pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (!pinned.has_value()) {
					copied.emplace(pages_.read(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL));
				}
				debug::PerfTrace::addValue("native_graph.read.property_count_batch_pages", 1);
			}
			if (pinned.has_value()) {
				results[index] = decodeEntityPropertyCount(snapshot, keys[index], location, pinned->page.pageId(),
															 pinned->page.ownerKind(),
															 pinned->page.read({location.page_id, location.slot_id, location.slot_generation}));
			} else {
				const std::vector<std::byte> bytes =
						copied->read({location.page_id, location.slot_id, location.slot_generation});
				results[index] = decodeEntityPropertyCount(snapshot, keys[index], location, copied->pageId(),
															 copied->ownerKind(), bytes);
			}
		}
		for (size_t index = 0U; index < results.size(); ++index) {
			if (!results[index].has_value()) {
				results[index] = 0U;
			}
		}
		debug::PerfTrace::addValue("native_graph.read.property_count_batch_owners",
									 static_cast<int64_t>(keys.size()));
	}

	PropertyCollectionRootScanCursor
	PropertyCollectionRootStore::openScanCursor(const MetadataSnapshotPin &snapshot) const {
		return openScanCursor(snapshot, nullptr);
	}

	PropertyCollectionRootScanCursor
	PropertyCollectionRootStore::openScanCursor(const MetadataSnapshotPin &snapshot,
											 const CancellationCheckpoint *cancellation) const {
		if (!metadata_.ownsSnapshot(snapshot)) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property scan cursor requires this store's active metadata snapshot");
		}
		return PropertyCollectionRootScanCursor(*this, snapshot.superblock().generation, cancellation);
	}

	PropertyCollectionRootScanCursor::PropertyCollectionRootScanCursor(
			PropertyCollectionRootScanCursor &&other) noexcept :
		store_(std::exchange(other.store_, nullptr)), generation_(std::exchange(other.generation_, Generation{})),
		cancellation_(std::exchange(other.cancellation_, nullptr)),
		current_page_id_(std::exchange(other.current_page_id_, PageId{})),
		current_page_(std::move(other.current_page_)), directory_cursor_(std::move(other.directory_cursor_)),
		pending_directory_location_(std::move(other.pending_directory_location_)),
		directory_metadata_pages_reported_(std::exchange(other.directory_metadata_pages_reported_, 0U)),
		last_requested_directory_id_(std::exchange(other.last_requested_directory_id_, 0U)),
		directory_stream_enabled_(std::exchange(other.directory_stream_enabled_, false)) {
		other.current_page_.reset();
		other.directory_cursor_.reset();
		other.pending_directory_location_.reset();
	}

	PropertyCollectionRootScanCursor &
	PropertyCollectionRootScanCursor::operator=(PropertyCollectionRootScanCursor &&other) noexcept {
		if (this != &other) {
			store_ = std::exchange(other.store_, nullptr);
			generation_ = std::exchange(other.generation_, Generation{});
			cancellation_ = std::exchange(other.cancellation_, nullptr);
			current_page_id_ = std::exchange(other.current_page_id_, PageId{});
			current_page_ = std::move(other.current_page_);
			directory_cursor_ = std::move(other.directory_cursor_);
			pending_directory_location_ = std::move(other.pending_directory_location_);
			directory_metadata_pages_reported_ = std::exchange(other.directory_metadata_pages_reported_, 0U);
			last_requested_directory_id_ = std::exchange(other.last_requested_directory_id_, 0U);
			directory_stream_enabled_ = std::exchange(other.directory_stream_enabled_, false);
			other.current_page_.reset();
			other.directory_cursor_.reset();
			other.pending_directory_location_.reset();
		}
		return *this;
	}

	PropertyCollectionRoot PropertyCollectionRootStore::decodeEntityRoot(const MetadataSnapshotPin &snapshot,
																							 const EntityPropertyKey &key,
																							 const EntityPropertyLocation &location,
																							 const PageId pageId, const PageOwnerKind ownerKind,
																							 const std::span<const std::byte> bytes, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) const {
		const uint8_t invalidLocation =
				static_cast<uint8_t>(location.key != key) | static_cast<uint8_t>(location.page_id != pageId);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location disagrees with its decoded Property page");
		}
		if (ownerKind != PageOwnerKind::POK_PROPERTY) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location does not resolve to a Property page");
		}
		debug::PerfTrace::addValue("native_graph.read.property_full_decode", 1);
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
			size_t payloadBytes = 0U;
			const SelectedPropertyReadAdmission admitPayload = [&](size_t bytes) {
				payloadBytes = bytes;
				if (slotsAdmission != nullptr) (*slotsAdmission)(bytes);
			};
            const std::vector<std::byte> payload = large_values_->read(snapshot, reference.value,
                slotsAdmission ? admitPayload : SelectedPropertyReadAdmission{});
			if (!EntityPropertyLargeValuePayloadCodec::hasMagic(payload)) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property LargeValue payload is not the canonical owner-neutral encoding");
			}
			root.owner_entity_id = reference.owner_entity_id;
			root.owner_entity_type = reference.owner_entity_type;
			const SelectedPropertyReadAdmission combinedSlots = [&](size_t bytes) {
				if (bytes > std::numeric_limits<size_t>::max() - payloadBytes) {
					throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue payload and slots size overflowed");
				}
				if (slotsAdmission != nullptr) (*slotsAdmission)(payloadBytes + bytes);
			};
			root.entries = EntityPropertyLargeValuePayloadCodec::decode(payload, admission, slotsAdmission ? &combinedSlots : nullptr);
		} else {
			if ((admission != nullptr || slotsAdmission != nullptr) && entityKeyForRoot(EntityPropertyCollectionRootCodec::decodeOwner(bytes)) != key) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE, "Entity Property Directory location resolves to another logical owner");
			}
			root = EntityPropertyCollectionRootCodec::decode(bytes, admission, slotsAdmission);
		}
		if (slotsAdmission != nullptr) (*slotsAdmission)(root.entries.capacity() * sizeof(PropertyCollectionRootEntry));
		const EntityPropertyKey decodedKey = entityKeyForRoot(root);
		if (decodedKey != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location resolves to another logical owner");
		}
		return root;
	}

	std::optional<PropertyCollectionRootEntry>
	PropertyCollectionRootStore::decodeSelectedEntityProperty(const MetadataSnapshotPin &snapshot,
																											 const EntityPropertyKey &key,
																											 const EntityPropertyLocation &location,
																											 const PageId pageId, const PageOwnerKind ownerKind,
																											 const std::span<const std::byte> bytes,
																											 const uint32_t keyToken) const {
		const uint8_t invalidLocation =
				static_cast<uint8_t>(location.key != key) | static_cast<uint8_t>(location.page_id != pageId) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_PROPERTY);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property Directory location does not resolve to its Property page");
		}
		debug::PerfTrace::addValue("native_graph.read.property_selected_decode", 1);
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			const uint8_t invalidReference = static_cast<uint8_t>(referenceKey != key) |
					static_cast<uint8_t>(reference.value.kind != LargeValueKind::LVK_BYTES) |
					static_cast<uint8_t>(reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
			if (invalidReference != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference has an invalid owner or content type");
			}
			const std::array<uint32_t, 1U> tokens{keyToken};
			LargeValuePropertyDirectory directory =
					readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value);
			return recordSelectedValues(readSelectedLargeValueProperties(*large_values_, snapshot, reference.value, directory, tokens).front());
		}

		const PropertyCollectionRoot owner = EntityPropertyCollectionRootCodec::decodeOwner(bytes);
		if (entityKeyForRoot(owner) != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property Directory location resolves to another logical owner");
		}
		return recordSelectedValues(EntityPropertyCollectionRootCodec::decodeSelected(bytes, keyToken));
	}

	bool PropertyCollectionRootStore::decodeEntityPropertyExists(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key,
																	 const EntityPropertyLocation &location, const PageId pageId,
																	 const PageOwnerKind ownerKind,
																	 const std::span<const std::byte> bytes,
																	 const uint32_t keyToken, const bool requireNonNull) const {
		const uint8_t invalidLocation =
				static_cast<uint8_t>(location.key != key) | static_cast<uint8_t>(location.page_id != pageId) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_PROPERTY);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property presence location does not resolve to its Property page");
		}
		debug::PerfTrace::addValue("native_graph.read.property_presence_probe", 1);
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			const uint8_t invalidReference = static_cast<uint8_t>(referenceKey != key) |
					static_cast<uint8_t>(reference.value.kind != LargeValueKind::LVK_BYTES) |
					static_cast<uint8_t>(reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
			if (invalidReference != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference has an invalid owner or content type");
			}
			auto directory = readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value);
			const auto found = std::ranges::lower_bound(directory.locations, keyToken, {},
				&EntityPropertyLargeValueEntryLocation::key_token);
			if (found == directory.locations.end() || found->key_token != keyToken) return false;
			if (!requireNonNull) return true;
			constexpr size_t prefixBytes = sizeof(uint32_t) + CanonicalPropertyValueCodec::HEADER_BYTES;
			if (found->length < prefixBytes) {
				throwRootStoreError(FormatError::FERR_BUFFER_TOO_SMALL, "Entity Property value header is truncated");
			}
			const auto prefix = large_values_->readRangeFromManifestCached(snapshot, directory.manifest,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + found->offset, prefixBytes, directory.range_cache);
			if (LittleEndianCodec::readU32(prefix, 0U) != keyToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER, "Entity Property value token disagrees with its directory");
			}
			return !CanonicalPropertyValueCodec::isNullHeader(std::span<const std::byte>(prefix).subspan(sizeof(uint32_t)),
				found->length - sizeof(uint32_t));
		}
		const PropertyCollectionRoot owner = EntityPropertyCollectionRootCodec::decodeOwner(bytes);
		if (entityKeyForRoot(owner) != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property Directory location resolves to another logical owner");
		}
		return EntityPropertyCollectionRootCodec::containsToken(bytes, keyToken, requireNonNull);
	}

	std::vector<std::optional<PropertyCollectionRootEntry>>
	PropertyCollectionRootStore::decodeSelectedEntityProperties(
			const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
			const EntityPropertyLocation &location, const PageId pageId, const PageOwnerKind ownerKind,
			const std::span<const std::byte> bytes, const std::span<const uint32_t> keyTokens,
			const SelectedPropertyReadAdmission *admission) const {
		if (keyTokens.empty()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"multi-column Entity Property decoding requires selected tokens");
		}
		const uint8_t invalidLocation =
				static_cast<uint8_t>(location.key != key) | static_cast<uint8_t>(location.page_id != pageId) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_PROPERTY);
		if (invalidLocation != 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location does not resolve to its Property page");
		}
		debug::PerfTrace::addValue("native_graph.read.property_selected_column_decode", 1);
		// Keep the aggregate selected-decode counter meaningful for callers that
		// compare scalar and multi-column reads, while the column-specific counter
		// retains the finer-grained batch diagnostic.
		debug::PerfTrace::addValue("native_graph.read.property_selected_decode", 1);
		if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			if (large_values_ == nullptr) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference requires a configured LargeValue store");
			}
			const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
			const EntityPropertyKey referenceKey =
					entityKeyForRoot({reference.owner_entity_id, reference.owner_entity_type, {}});
			const uint8_t invalidReference = static_cast<uint8_t>(referenceKey != key) |
					static_cast<uint8_t>(reference.value.kind != LargeValueKind::LVK_BYTES) |
					static_cast<uint8_t>(reference.value.media_type != RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
			if (invalidReference != 0U) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Entity Property LargeValue reference has an invalid owner or content type");
			}
			LargeValuePropertyDirectory directory =
					readLargeValuePropertyDirectory(*large_values_, snapshot, reference.value);
			return recordSelectedValues(readSelectedLargeValueProperties(*large_values_, snapshot, reference.value, directory, keyTokens, admission));
		}

		const PropertyCollectionRoot owner = EntityPropertyCollectionRootCodec::decodeOwner(bytes);
		if (entityKeyForRoot(owner) != key) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property Directory location resolves to another logical owner");
		}
		return recordSelectedValues(EntityPropertyCollectionRootCodec::decodeSelected(bytes, keyTokens, admission));
	}

	void PropertyCollectionRootScanCursor::validateSnapshot(const MetadataSnapshotPin &snapshot) const {
		if (store_ == nullptr || !store_->metadata_.ownsSnapshot(snapshot) ||
			snapshot.superblock().generation != generation_) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property scan cursor requires its original active metadata generation");
		}
	}

	void PropertyCollectionRootScanCursor::accountDirectoryPages() {
		if (!directory_cursor_.has_value()) {
			return;
		}
		const size_t current = directory_cursor_->metadataPagesRead();
		if (current > directory_metadata_pages_reported_) {
			debug::PerfTrace::addValue("native_graph.read.property_directory_metadata_pages",
									   static_cast<int64_t>(current - directory_metadata_pages_reported_));
			directory_metadata_pages_reported_ = current;
		}
	}

	std::optional<EntityPropertyLocation>
	PropertyCollectionRootScanCursor::findLocationOptional(const MetadataSnapshotPin &snapshot,
														  const EntityPropertyKey &key) {
		validateSnapshot(snapshot);
		if (cancellation_ != nullptr) {
			cancellation_->check();
		}
		const uint64_t requestedDirectoryId = key.directoryId();
		if (!directory_stream_enabled_ || requestedDirectoryId <= last_requested_directory_id_) {
			// Restart the ordered stream instead of permanently degrading this
			// query-owned cursor to point lookups. Operators commonly consume the
			// same snapshot in several batches whose owner ranges start over; a
			// restart preserves correctness and lets the next monotonic run share
			// one root descent and one decoded Property page again.
			directory_stream_enabled_ = true;
			directory_cursor_.reset();
			pending_directory_location_.reset();
			last_requested_directory_id_ = 0U;
		}
		if (!directory_cursor_.has_value()) {
			directory_cursor_.emplace(store_->metadata_.openEntityPropertyLocationRangeCursor(
					snapshot, requestedDirectoryId, std::numeric_limits<uint64_t>::max(), cancellation_));
			debug::PerfTrace::addValue("native_graph.read.property_directory_refill", 1);
		}
		last_requested_directory_id_ = requestedDirectoryId;
		if (pending_directory_location_.has_value() &&
			pending_directory_location_->key.directoryId() < requestedDirectoryId) {
			pending_directory_location_.reset();
		}
		if (!pending_directory_location_.has_value()) {
				const std::optional<LocationLeafEntry> entry = directory_cursor_->nextAtOrAfter(requestedDirectoryId);
				accountDirectoryPages();
				if (entry.has_value()) {
					pending_directory_location_.emplace(EntityPropertyKey::fromDirectoryId(entry->logical_id),
												 PageId(entry->segment_id.value()), entry->first_page,
												 static_cast<uint32_t>(entry->generation.value()));
			}
		}
		if (pending_directory_location_.has_value() &&
			pending_directory_location_->key.directoryId() == requestedDirectoryId) {
			std::optional<EntityPropertyLocation> result = std::move(pending_directory_location_);
			pending_directory_location_.reset();
			return result;
		}
		return std::nullopt;
	}

	PropertyCollectionRoot PropertyCollectionRootScanCursor::readLocation(const MetadataSnapshotPin &snapshot,
																		  const EntityPropertyKey &key,
																		  const EntityPropertyLocation &location, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		validateSnapshot(snapshot);
		if (cancellation_ != nullptr) {
			cancellation_->check();
		}
		if (location.page_id != current_page_id_) {
			if (slotsAdmission) (*slotsAdmission)(checkedLargeValueBuffers(
					current_page_ ? current_page_->unaccountedReadBufferBytes() : 0U, pageReadAdmissionBytes(snapshot)));
			current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
			current_page_id_ = location.page_id;
			debug::PerfTrace::addValue("native_graph.read.property_full_cursor_pages", 1);
		}
		const size_t pageBytes = current_page_->unaccountedReadBufferBytes();
		const SelectedPropertyReadAdmission admitSlot = [&](size_t bytes) {
			if (slotsAdmission) (*slotsAdmission)(checkedLargeValueBuffers(pageBytes, bytes));
		};
		PropertyCollectionRoot result;
		{
			const std::vector<std::byte> bytes = current_page_->read(
					{location.page_id, location.slot_id, location.slot_generation},
					slotsAdmission ? admitSlot : SelectedPropertyReadAdmission{});
			const SelectedPropertyReadAdmission admitDecode = [&](size_t retained) {
				admitSlot(checkedLargeValueBuffers(bytes.capacity(), retained));
			};
			result = store_->decodeEntityRoot(snapshot, key, location, current_page_->pageId(),
					current_page_->ownerKind(), bytes, admission, slotsAdmission ? &admitDecode : nullptr);
		}
		// The retained cursor page belongs to the session; this callback reports read temporaries.
		if (slotsAdmission) (*slotsAdmission)(result.entries.capacity() * sizeof(PropertyCollectionRootEntry));
		if (cancellation_ != nullptr) cancellation_->check();
		return result;
	}

	PropertyCollectionRoot PropertyCollectionRootScanCursor::readEntity(const MetadataSnapshotPin &snapshot,
																const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		validateSnapshot(snapshot);
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property scan read requires a valid logical owner key");
		}
		const std::optional<EntityPropertyLocation> location = findLocationOptional(snapshot, key);
		if (!location.has_value()) {
			throwRootStoreError(FormatError::FERR_INVALID_STATE,
								"Entity Property scan read requires a present Property root");
		}
		return readLocation(snapshot, key, *location, admission, slotsAdmission);
	}

	std::optional<PropertyCollectionRoot>
	PropertyCollectionRootScanCursor::readEntityOptional(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		debug::ScopedPerfTimer timer("native_graph.read.property_optional_lookup");
		validateSnapshot(snapshot);
		if (!key.isValid()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"optional Entity Property scan read requires a valid logical owner key");
		}
		const std::optional<EntityPropertyLocation> location = findLocationOptional(snapshot, key);
		return location.has_value() ? std::optional(readLocation(snapshot, key, *location, admission, slotsAdmission)) : std::nullopt;
	}

	std::optional<PropertyCollectionRootEntry>
	PropertyCollectionRootScanCursor::readEntityPropertyOptional(const MetadataSnapshotPin &snapshot,
																																				const EntityPropertyKey &key,
																																				const uint32_t keyToken) {
		validateSnapshot(snapshot);
		if (!key.isValid() || keyToken == 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"selective Entity Property scan read requires a valid owner key and nonzero token");
		}
		const std::optional<EntityPropertyLocation> location = findLocationOptional(snapshot, key);
		return location.has_value() ? readSelectedLocation(snapshot, key, *location, keyToken) : std::nullopt;
	}

	bool PropertyCollectionRootScanCursor::readEntityPropertyExists(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key, const uint32_t keyToken, const bool requireNonNull) {
		validateSnapshot(snapshot);
		if (!key.isValid() || keyToken == 0U) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Entity Property presence scan requires a valid owner key and nonzero token");
		}
		const std::optional<EntityPropertyLocation> location = findLocationOptional(snapshot, key);
		if (!location.has_value()) {
			return false;
		}
		return readSelectedPresenceLocation(snapshot, key, *location, keyToken, requireNonNull);
	}

	void PropertyCollectionRootScanCursor::readEntityPropertiesOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<const uint32_t> keyTokens,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const bool columnBatch, const SelectedPropertyReadAdmission *admission) {
			validateSnapshot(snapshot);
		if (keyTokens.empty() || keys.size() > std::numeric_limits<size_t>::max() / keyTokens.size() ||
			results.size() != keys.size() * keyTokens.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Property scan batch requires a rectangular owner/token result shape");
		}
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key : keys) {
			const uint64_t directoryId = key.directoryId();
			if (directoryId <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan batch owners must be strictly ordered");
			}
			previousDirectoryId = directoryId;
		}
		uint32_t previousToken = 0U;
		for (const uint32_t token : keyTokens) {
			if (token == 0U || token <= previousToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan batch tokens must be strictly increasing");
			}
			previousToken = token;
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		for (size_t ownerIndex = 0U; ownerIndex < keys.size(); ++ownerIndex) {
			if (cancellation_ != nullptr) {
				cancellation_->check();
			}
			locations[ownerIndex] = findLocationOptional(snapshot, keys[ownerIndex]);
		}
		// Directory results are owner ordered, but COW publication can place those
		// owners on non-monotonic physical pages. Resolve the whole range first,
		// then read each distinct page once and restore owner-major output order.
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation_);
		size_t pagesRead = 0U;
		PageId previousPage{};
		std::optional<SlottedPagePinnedRead> pinned;
		SlottedPage *copied = nullptr;
		bool tryPinned = true;
		for (const size_t ownerIndex : present) {
			validateSnapshot(snapshot);
			if (cancellation_ != nullptr) cancellation_->check();
			const EntityPropertyLocation &location = *locations[ownerIndex];
			if (previousPage != location.page_id) {
				previousPage = location.page_id;
				++pagesRead;
				pinned.reset();
				copied = nullptr;
				if (current_page_.has_value() && current_page_id_ == location.page_id) {
					copied = &*current_page_;
				} else {
					if (tryPinned)
						pinned = store_->pages_.readPinned(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (!pinned.has_value()) {
						// Disabled or saturated pools cannot lend an image. Avoid
						// repeating a failed pin route for every remaining page.
						tryPinned = false;
						current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
						current_page_id_ = location.page_id;
						copied = &*current_page_;
					}
				}
			}
			std::vector<std::optional<PropertyCollectionRootEntry>> decoded;
			if (pinned.has_value()) {
				const auto bytes = pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
				decoded = store_->decodeSelectedEntityProperties(snapshot, keys[ownerIndex], location,
						pinned->page.pageId(), pinned->page.ownerKind(), bytes, keyTokens, admission);
				debug::PerfTrace::addValue("native_graph.read.property_selected_borrowed_owners", 1);
			} else {
				const auto bytes = copied->read({location.page_id, location.slot_id, location.slot_generation});
				decoded = store_->decodeSelectedEntityProperties(snapshot, keys[ownerIndex], location,
						copied->pageId(), copied->ownerKind(), bytes, keyTokens, admission);
				debug::PerfTrace::addValue("native_graph.read.property_selected_slot_copy_bytes", static_cast<int64_t>(bytes.size()));
			}
			if (cancellation_ != nullptr) cancellation_->check();
			if (decoded.size() != keyTokens.size()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Property scan batch decoder changed its column cardinality");
			}
			std::move(decoded.begin(), decoded.end(), results.begin() +
											static_cast<std::ptrdiff_t>(ownerIndex * keyTokens.size()));
		}
		// Keep the established one-page owned cursor cache across batches;
		// never retain a buffer-pool pin beyond this read. Only the final page
		// is copied, while borrowed slots were decoded directly above.
		if (pinned.has_value()) {
            // Results above are already decoded. Retaining an extra image is
            // optional and must not turn a valid bounded read into rejection.
            current_page_.reset(); current_page_id_ = {};
            try {
                auto retained = SlottedPage::open(pinned->contents.owner, pinned->contents.payload(),
                    utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
                        (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_copy_bytes",
                    static_cast<int64_t>(pinned->contents.payload().size()));
                current_page_.emplace(std::move(retained)); current_page_id_ = previousPage;
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_declined", 1);
            } catch (const query::execution::MaterializedByteLimitExceeded &) {
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_declined", 1);
            }
		}
		if (columnBatch) {
			debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_pages",
										static_cast<int64_t>(pagesRead));
			debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_owners",
										static_cast<int64_t>(keys.size()));
		} else {
			debug::PerfTrace::addValue("native_graph.read.property_selected_batch_pages",
										static_cast<int64_t>(pagesRead));
			debug::PerfTrace::addValue("native_graph.read.property_selected_batch_owners",
										static_cast<int64_t>(keys.size()));
		}
	}

	void PropertyCollectionRootScanCursor::readEntityPropertiesAndCountsOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<const uint32_t> keyTokens,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const std::span<std::optional<size_t>> counts, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *transientBufferAdmission) {
		validateSnapshot(snapshot);
		if (keyTokens.empty() || keys.size() > std::numeric_limits<size_t>::max() / keyTokens.size() ||
			results.size() != keys.size() * keyTokens.size() || counts.size() != keys.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Property scan combined batch requires a rectangular result shape");
		}
		uint32_t previousToken = 0U;
		for (const uint32_t token: keyTokens) {
			if (token == 0U || token <= previousToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan combined batch tokens must be strictly increasing");
			}
			previousToken = token;
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		std::fill(counts.begin(), counts.end(), std::nullopt);
		if (keys.empty()) {
			return;
		}

		// An owned image retained by an earlier unbudgeted read is optional.
		// Release it before a budgeted batch so a warm session can use the same
		// borrowed-page path and memory allowance as a cold session.
		if (transientBufferAdmission != nullptr) {
			current_page_.reset();
			current_page_id_ = PageId{};
		}

		if (keys.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<EntityPropertyLocation>))
			throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property owner batch slots overflowed");
		size_t ownerBytes = keys.size() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		ownerBytes = locations.capacity() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		const SelectedPropertyReadAdmission admitTransient = [&](size_t bytes) {
			if (transientBufferAdmission) (*transientBufferAdmission)(checkedLargeValueBuffers(ownerBytes, bytes));
		};
		for (size_t index = 0U; index < keys.size(); ++index) {
			if (!keys[index].isValid()) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan combined batch contains an invalid owner key");
			}
			locations[index] = findLocationOptional(snapshot, keys[index]);
			if (!locations[index].has_value()) {
				counts[index] = size_t{0U};
			}
		}
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation_, transientBufferAdmission ? &admitTransient : nullptr);
		ownerBytes = checkedLargeValueBuffers(ownerBytes, present.capacity() * sizeof(size_t));
		std::optional<SlottedPagePinnedRead> pinned;
		SlottedPage *copied = nullptr;
		PageId currentPage{};
		for (const size_t ownerIndex: present) {
			const EntityPropertyLocation &location = *locations[ownerIndex];
			if (location.page_id != currentPage) {
				pinned.reset();
				copied = nullptr;
				currentPage = location.page_id;
				if (currentPage == current_page_id_) {
					copied = &*current_page_;
				} else {
					debug::PerfTrace::addValue("native_graph.read.property_combined_page_acquisition", 1);
					pinned = store_->pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (!pinned.has_value()) {
						admitTransient(checkedLargeValueBuffers(
								current_page_ ? current_page_->unaccountedReadBufferBytes() : 0U, pageReadAdmissionBytes(snapshot)));
						current_page_.emplace(store_->pages_.read(snapshot, currentPage));
						current_page_id_ = currentPage;
						copied = &*current_page_;
					}
				}
			}
			std::vector<std::byte> copiedBytes;
			std::span<const std::byte> bytes;
			PageId pageId;
			PageOwnerKind ownerKind;
			if (pinned.has_value()) {
				bytes = pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
				pageId = pinned->page.pageId();
				ownerKind = pinned->page.ownerKind();
			} else {
				const size_t copiedPageBytes = copied->unaccountedReadBufferBytes();
				const SelectedPropertyReadAdmission admitSlot = [&](size_t retained) {
					admitTransient(checkedLargeValueBuffers(copiedPageBytes, retained));
				};
				copiedBytes = copied->read({location.page_id, location.slot_id, location.slot_generation},
						transientBufferAdmission ? admitSlot : SelectedPropertyReadAdmission{});
				bytes = copiedBytes;
				pageId = copied->pageId();
				ownerKind = copied->ownerKind();
			}
			debug::PerfTrace::addValue("native_graph.read.property_selected_column_decode", 1);
			debug::PerfTrace::addValue("native_graph.read.property_selected_decode", 1);
			const SelectedPropertyReadAdmission admitDecode = [&](size_t retained) {
				admitTransient(checkedLargeValueBuffers(
						checkedLargeValueBuffers(copied ? copied->unaccountedReadBufferBytes() : 0U, copiedBytes.capacity()), retained));
			};
			auto decoded = store_->decodeSelectedEntityPropertiesAndCount(snapshot, keys[ownerIndex], location,
				pageId, ownerKind, bytes, keyTokens, admission, transientBufferAdmission ? &admitDecode : nullptr);
			const size_t count = decoded.first;
			auto selected = std::move(decoded.second);
			if (selected.size() != keyTokens.size()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Property scan combined decoder changed its column cardinality");
			}
			if (cancellation_ != nullptr) cancellation_->check();
			counts[ownerIndex] = count;
			std::move(selected.begin(), selected.end(), results.begin() +
									static_cast<std::ptrdiff_t>(ownerIndex * keyTokens.size()));
		}
		// A budgeted batch must not copy a borrowed page solely to populate an
		// optional session cache. The buffer pool already owns the authenticated
		// image and can lend it again on the next call without a retained pin.
		if (pinned.has_value() && transientBufferAdmission == nullptr) {
			try {
				// Retain only an owned image across calls, so the session does not keep a pool pin.
				admitTransient(checkedLargeValueBuffers(current_page_ ? current_page_->unaccountedReadBufferBytes() : 0U,
						pinned->contents.payload().size()));
				current_page_.emplace(SlottedPage::open(pinned->contents.owner, pinned->contents.payload(),
					utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr)));
				current_page_id_ = currentPage;
				pinned.reset();
				admitTransient(current_page_->unaccountedReadBufferBytes());
			} catch (...) {
				std::fill(results.begin(), results.end(), std::nullopt);
				std::fill(counts.begin(), counts.end(), std::nullopt);
				throw;
			}
		}
		std::vector<std::optional<EntityPropertyLocation>>().swap(locations);
		std::vector<size_t>().swap(present);
		if (transientBufferAdmission) (*transientBufferAdmission)(0U);
		debug::PerfTrace::addValue("native_graph.read.property_combined_batch_owners",
								 static_cast<int64_t>(keys.size()));
		// Preserve the established selected-batch metric for callers and tests
		// while exposing the combined operation's more specific counter above.
		debug::PerfTrace::addValue("native_graph.read.property_selected_batch_owners",
								 static_cast<int64_t>(keys.size()));
		debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_owners",
									 static_cast<int64_t>(keys.size()));
	}

	void PropertyCollectionRootScanCursor::readEntityPropertyCountsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<std::optional<size_t>> results, const SelectedPropertyReadAdmission *transientBufferAdmission) {
		validateSnapshot(snapshot);
		if (keys.size() != results.size()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Property scan count batch requires one result per owner");
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key : keys) {
			if (!key.isValid() || key.directoryId() <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan count owners must be strictly ordered and valid");
			}
			previousDirectoryId = key.directoryId();
		}
		if (keys.empty()) {
			return;
		}

		if (keys.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<EntityPropertyLocation>))
			throwRootStoreError(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property count owner slots overflowed");
		size_t ownerBytes = keys.size() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		ownerBytes = locations.capacity() * sizeof(std::optional<EntityPropertyLocation>);
		if (transientBufferAdmission) (*transientBufferAdmission)(ownerBytes);
		const SelectedPropertyReadAdmission admitTransient = [&](size_t bytes) {
			if (transientBufferAdmission) (*transientBufferAdmission)(checkedLargeValueBuffers(ownerBytes, bytes));
		};
		for (size_t index = 0U; index < keys.size(); ++index) {
			if (cancellation_ != nullptr) {
				cancellation_->check();
			}
			locations[index] = findLocationOptional(snapshot, keys[index]);
			if (!locations[index].has_value()) {
				results[index] = size_t{0U};
			}
		}

		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation_, transientBufferAdmission ? &admitTransient : nullptr);
		ownerBytes = checkedLargeValueBuffers(ownerBytes, present.capacity() * sizeof(size_t));

		std::optional<SlottedPagePinnedRead> borrowed;
		PageId borrowedPage{};
		if (transientBufferAdmission != nullptr) {
			current_page_.reset();
			current_page_id_ = PageId{};
		}
		PageId currentPage = current_page_id_;
		for (const size_t index : present) {
			if (cancellation_ != nullptr) {
				cancellation_->check();
			}
			const EntityPropertyLocation &location = *locations[index];
			if (transientBufferAdmission != nullptr) {
				if (location.page_id != borrowedPage) {
					borrowed.reset();
					borrowedPage = location.page_id;
					borrowed = store_->pages_.readPinned(snapshot, borrowedPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (borrowed.has_value()) {
						debug::PerfTrace::addValue("native_graph.read.property_count_batch_pages", 1);
					}
				}
				if (borrowed.has_value()) {
					const auto bytes = borrowed->page.read({location.page_id, location.slot_id, location.slot_generation});
					results[index] = store_->decodeEntityPropertyCount(snapshot, keys[index], location,
							borrowed->page.pageId(), borrowed->page.ownerKind(), bytes, &admitTransient);
					continue;
				}
			}

			if (location.page_id != currentPage) {
				if (transientBufferAdmission) (*transientBufferAdmission)(checkedLargeValueBuffers(ownerBytes,
						checkedLargeValueBuffers(current_page_ ? current_page_->unaccountedReadBufferBytes() : 0U,
								pageReadAdmissionBytes(snapshot))));
				currentPage = location.page_id;
				// Keep an owned transient image in the cursor after the authenticated
				// pin is released. This preserves the one-page cursor locality without
				// retaining a Buffer Pool frame for the lifetime of a query.
				if (std::optional<SlottedPagePinnedRead> pinned =
						store_->pages_.readPinned(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL);
						pinned.has_value()) {
					current_page_.emplace(SlottedPage::open(pinned->contents.owner, pinned->contents.payload(),
							utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr)));
				} else {
					current_page_.emplace(store_->pages_.read(snapshot, currentPage, BufferPoolReadHint::BPRH_SEQUENTIAL));
				}
				current_page_id_ = currentPage;
				debug::PerfTrace::addValue("native_graph.read.property_count_batch_pages", 1);
			}
			const size_t pageBytes = current_page_->unaccountedReadBufferBytes();
			const auto admitSlot = [&](size_t bytes) {
				if (transientBufferAdmission) (*transientBufferAdmission)(checkedLargeValueBuffers(
						checkedLargeValueBuffers(ownerBytes, pageBytes), bytes));
			};
			const std::vector<std::byte> bytes = current_page_->read(
					{location.page_id, location.slot_id, location.slot_generation},
					transientBufferAdmission ? SelectedPropertyReadAdmission(admitSlot) : SelectedPropertyReadAdmission{});
			const SelectedPropertyReadAdmission admitDecode = [&](size_t decoded) {
				admitSlot(checkedLargeValueBuffers(bytes.capacity(), decoded));
			};
			results[index] = store_->decodeEntityPropertyCount(
					snapshot, keys[index], location, current_page_->pageId(), current_page_->ownerKind(), bytes, transientBufferAdmission ? &admitDecode : nullptr);
		}
		for (std::optional<size_t> &result : results) {
			if (!result.has_value()) {
				result = size_t{0U};
			}
		}
		std::vector<std::optional<EntityPropertyLocation>>().swap(locations);
		std::vector<size_t>().swap(present);
		if (transientBufferAdmission) (*transientBufferAdmission)(0U);
		debug::PerfTrace::addValue("native_graph.read.property_count_batch_owners",
								 static_cast<int64_t>(keys.size()));
	}

	std::optional<PropertyCollectionRootEntry>
	PropertyCollectionRootScanCursor::readSelectedLocation(const MetadataSnapshotPin &snapshot,
																			  const EntityPropertyKey &key,
																			  const EntityPropertyLocation &location,
																			  const uint32_t keyToken) {
		validateSnapshot(snapshot);
		if (cancellation_ != nullptr) {
			cancellation_->check();
		}
		if (location.page_id != current_page_id_) {
			current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
			current_page_id_ = location.page_id;
		}
		const std::vector<std::byte> bytes =
				current_page_->read({location.page_id, location.slot_id, location.slot_generation});
		auto result = store_->decodeSelectedEntityProperty(snapshot, key, location, current_page_->pageId(),
																																												 current_page_->ownerKind(), bytes, keyToken);
		if (cancellation_ != nullptr) cancellation_->check();
		return result;
	}

	bool PropertyCollectionRootScanCursor::readSelectedPresenceLocation(const MetadataSnapshotPin &snapshot,
																	 const EntityPropertyKey &key,
																	 const EntityPropertyLocation &location,
																	 const uint32_t keyToken, const bool requireNonNull) {
		validateSnapshot(snapshot);
		if (cancellation_ != nullptr) {
			cancellation_->check();
		}
		if (location.page_id != current_page_id_) {
			current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
			current_page_id_ = location.page_id;
		}
		const std::vector<std::byte> bytes =
				current_page_->read({location.page_id, location.slot_id, location.slot_generation});
		return store_->decodeEntityPropertyExists(snapshot, key, location, current_page_->pageId(),
																																												 current_page_->ownerKind(), bytes, keyToken, requireNonNull);
	}

	std::vector<std::optional<PropertyCollectionRootEntry>>
	PropertyCollectionRootScanCursor::readSelectedLocation(
			const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
			const EntityPropertyLocation &location, const std::span<const uint32_t> keyTokens,
			const SelectedPropertyReadAdmission *admission) {
		validateSnapshot(snapshot);
		if (cancellation_ != nullptr) {
			cancellation_->check();
		}
		if (keyTokens.empty()) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Property scan selected location requires at least one token");
		}
		if (location.page_id != current_page_id_) {
			current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
			current_page_id_ = location.page_id;
		}
		const std::vector<std::byte> bytes =
				current_page_->read({location.page_id, location.slot_id, location.slot_generation});
		auto result = store_->decodeSelectedEntityProperties(snapshot, key, location, current_page_->pageId(),
																																												 current_page_->ownerKind(), bytes, keyTokens, admission);
		if (cancellation_ != nullptr) cancellation_->check();
		return result;
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
		// Resolve only the caller's ordered owners through one retained directory
		// route. Import-sized batches must not copy unrelated Property locations.
		std::vector<EntityPropertyKey> rootKeys;
		rootKeys.reserve(roots.size());
		uint64_t previousDirectoryId = 0U;
		for (const PropertyCollectionRoot &root: roots) {
			const EntityPropertyKey key = entityKeyForRoot(canonicalEntityRoot(root));
			if (key.directoryId() <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Entity Property batch roots must be strictly ordered by owner key");
			}
			previousDirectoryId = key.directoryId();
			rootKeys.push_back(key);
		}
		std::vector<std::optional<EntityPropertyLocation>> resolvedLocations(rootKeys.size());
		metadata_.findEntityPropertyLocationsSorted(snapshot, rootKeys, resolvedLocations);
		std::map<EntityPropertyKey, EntityPropertyLocation> existingLocations;
		for (const std::optional<EntityPropertyLocation> &location: resolvedLocations) {
			if (location.has_value()) {
				existingLocations.emplace(location->key, *location);
			}
		}
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
		for (const PropertyCollectionRoot &root: roots) {
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
			const auto compression = PropertyRowChunkPolicy::select(largeValuePayload, options.large_value_compression, options.adaptive_row_chunks);
			const std::optional<LargeValueRef> reused = large_values_->findContentDuplicate(
					snapshot, largeValuePayload, LargeValueKind::LVK_BYTES, RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE,
					options.maximum_large_value_dedup_candidates);
			const LargeValueRef reference =
					reused.has_value() ? *reused
									 : large_values_->planReference(nextLargeValueId, Generation(1U), largeValuePayload,
																  LargeValueKind::LVK_BYTES,
																	  RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE,
																	  compression);
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
						 .compression = compression,
						 .extent_class = LargeValueExtentClass::LVEC_32K,
						 .planned_data_page_count = large_values_->plannedDataPageCount(
								 largeValuePayload, compression, LargeValueExtentClass::LVEC_32K),
						 .planned_manifest_page_count = large_values_->plannedManifestPageCount(
								 largeValuePayload, compression, LargeValueExtentClass::LVEC_32K)});
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
		debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare.property_replacement");
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

	void PropertyCollectionRootStore::preparePlannedLargeValues(
			std::span<const NativeGraphLargeValueMutation> mutations,
			std::span<LargeValueMaterializationResult> materializations, LogSequenceNumber checkpointLsn) const {
		debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare.large_value");
		if (mutations.empty() && materializations.empty()) return;
		if (large_values_ == nullptr)
			throwRootStoreError(FormatError::FERR_INVALID_STATE, "LargeValue batch preparation requires a store");
		large_values_->prepareNativeGraphMaterializations(mutations, materializations, checkpointLsn);
	}

	void PropertyCollectionRootStore::preparePlannedLargeValue(const NativeGraphLargeValueMutation &mutation,
															   LargeValueMaterializationResult &materialization,
															   LogSequenceNumber checkpointLsn) const {
		debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare.large_value");
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
		debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare.property_batch");
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
