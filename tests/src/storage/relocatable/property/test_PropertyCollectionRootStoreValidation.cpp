/**
 * @file test_PropertyCollectionRootStoreValidation.cpp
 * @brief Fail-closed planner and materializer tests for owner-keyed Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/utils/Sha256.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph::storage;

namespace graph::storage {

	class PropertyCollectionRootStoreTestAccess final {
	public:
		[[nodiscard]] static PropertyCollectionRoot decodeEntityRoot(
				const PropertyCollectionRootStore &store, const MetadataSnapshotPin &snapshot,
				const EntityPropertyKey &key, const EntityPropertyLocation &location, const SlottedPage &page) {
			return store.decodeEntityRoot(snapshot, key, location, page);
		}
	};

} // namespace graph::storage

namespace {

	Superblock initialSuperblock(uint64_t databaseId = 0x3141592653589793ULL) {
		Superblock result;
		result.database_id = DatabaseId(databaseId);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	PropertyCollectionRoot root(uint64_t nodeId, uint32_t token, std::string value = "validated Property root") {
		return {nodeId, graph::Node::typeId, {{token, graph::PropertyValue(std::move(value))}}};
	}

	EntityPageNewPoolRequest nodePool() {
		return {.segment_id = SegmentId(31U),
				.pool_owner_id = 901U,
				.generation = Generation(1U),
				.page_id = PageId(101U),
				.first_slot = PhysicalSlotId(40U),
				.last_slot = PhysicalSlotId(40U)};
	}

	PropertyCollectionRootStoreOptions options() {
		return {PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	LargeValueRef fakeLargeValueRef(uint64_t valueId = 1U) {
		const std::array logical{std::byte{0x41U}};
		return {.value_id = LargeValueId(valueId),
				.generation = Generation(1U),
				.logical_bytes = logical.size(),
				.content_hash = graph::utils::Sha256::digest(logical),
				.kind = LargeValueKind::LVK_BYTES,
				.codec = LargeValueCodec::LVC_RAW,
				.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE};
	}

	EntityPropertyLargeValueReference largeValueRecord(uint64_t nodeId, uint64_t valueId = 1U) {
		return {.owner_entity_id = nodeId,
				.owner_entity_type = graph::Node::typeId,
				.value = fakeLargeValueRef(valueId)};
	}

	LargeValueWriteOptions largeValueOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(60U);
		result.last_data_slot = PhysicalSlotId(79U);
		result.first_manifest_slot = PhysicalSlotId(80U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
		return result;
	}

} // namespace

class PropertyCollectionRootStoreValidationTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> pages_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("property_root_validation_" + to_string(boost::uuids::random_generator()()));
		file_ = std::make_shared<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	void publishNodeAndRoot(const PropertyCollectionRoot &propertyRoot) {
		ASSERT_TRUE(entities_->allocateNode(nodePool(), propertyRoot.owner_entity_id, NodeEntityRecord{},
											LogSequenceNumber(2U))
						.has_value());
		static_cast<void>(test::publishPropertyRoot(*entities_, *properties_, *metadata_, propertyRoot, options()));
	}

	NativeGraphPropertyPageBatchMaterialization
	newPoolMaterialization(const EntityPropertyRootMutationPlan &plan,
						   LogSequenceNumber checkpointLsn = LogSequenceNumber(2U),
						   SegmentId segmentId = SegmentId(32U), PhysicalSlotId physicalSlot = PhysicalSlotId(41U),
						   PageId pageId = PageId(201U)) const {
		NativeGraphPropertyPageBatchMaterialization result;
		result.kind = NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		result.pool = {.segment_id = segmentId,
					   .physical_slot = physicalSlot,
					   .kind = SegmentKind::SGK_PROPERTY_PAGE_POOL,
					   .owner_id = options().page_owner_id,
					   .generation = Generation(1U),
					   .state = SegmentState::SGS_SEALED,
					   .committed_lsn = checkpointLsn};
		result.page_owner_id = result.pool.owner_id;
		NativeGraphPropertyPageInitialMaterialization page;
		page.location = {.page_id = pageId,
						 .segment_id = result.pool.segment_id,
						 .page_index = 0U,
						 .generation = result.pool.generation};
		page.upserted_properties = {
				{plan.key, page.location.page_id, std::numeric_limits<uint16_t>::max(), 0U}};
		result.pages.push_back(std::move(page));
		return result;
	}

	NativeGraphPropertyPageReplacementMaterialization
	replacementMaterialization(const EntityPropertyRootMutationPlan &plan) const {
		const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
		return {.pool = request.expected_pool,
				.expected_location = request.expected_location,
				.replacement = {},
				.page_owner_id = request.expected_page_owner_id,
				.expected_owner_lsn = request.expected_owner_lsn,
				.retire_page = request.retire_page};
	}

	LogSequenceNumber nextCheckpointLsn() const {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const uint64_t current = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		return LogSequenceNumber(current + 1U);
	}

	EntityPropertyLocation publishPropertyPayload(const EntityPropertyKey &key, std::span<const std::byte> payload,
												 PhysicalSlotId physicalSlot) {
		const PageId pageId = metadata_->allocateLogicalPageId(nextCheckpointLsn());
		const std::vector<SegmentId> segmentIds = metadata_->allocateLogicalSegmentIds(1U, nextCheckpointLsn());
		if (segmentIds.size() != 1U) {
			throw std::logic_error("Property validation test expected one logical pool identity");
		}
		const LogSequenceNumber pageLsn = nextCheckpointLsn();
		const PageOwnerHeader owner{pageId,
								 options().page_owner_id,
								 PageOwnerKind::POK_PROPERTY,
								 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								 Generation(1U),
								 pageLsn};
		SlottedPage page = SlottedPage::create(owner);
		const SlottedSlotReference slot = page.insert(payload);
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentIds.front();
		request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		request.pool_owner_id = owner.owner_id;
		request.generation = owner.generation;
		request.page_id = pageId;
		request.page_owner_id = owner.owner_id;
		request.page_owner_kind = owner.kind;
		const std::optional<SlottedPagePoolAllocation> allocation =
				pages_->allocatePage(request, page, physicalSlot, physicalSlot, pageLsn);
		if (!allocation.has_value()) {
			throw std::logic_error("Property validation test could not allocate its typed payload page");
		}
		const EntityPropertyLocation location{key, pageId, slot.slot_id, slot.generation};
		static_cast<void>(metadata_->upsertEntityPropertyLocations(std::span(&location, 1U), {}, nextCheckpointLsn()));
		return location;
	}

	EntityPropertyLocation appendPropertyPayload(PageId pageId, const EntityPropertyKey &key,
												std::span<const std::byte> payload) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		SlottedPage page = pages_->read(snapshot, pageId);
		snapshot.release();
		const SlottedSlotReference slot = page.insert(payload);
		static_cast<void>(pages_->replacePage(page, nextCheckpointLsn()));
		const EntityPropertyLocation location{key, pageId, slot.slot_id, slot.generation};
		static_cast<void>(metadata_->upsertEntityPropertyLocations(std::span(&location, 1U), {}, nextCheckpointLsn()));
		return location;
	}
};

TEST_F(PropertyCollectionRootStoreValidationTest, PlannersRejectForeignSnapshotsInvalidOptionsAndRoots) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PropertyCollectionRoot valid = root(1001U, 3U);
	MetadataSnapshotPin invalid;
	expectFormatError([&] { static_cast<void>(properties_->planEntityRootReplacement(invalid, valid, options())); },
					  FormatError::FERR_INVALID_STATE);
	const std::array<PropertyCollectionRoot, 0U> noRoots{};
	expectFormatError([&] { static_cast<void>(properties_->planEntityRootReplacements(invalid, noRoots, options())); },
					  FormatError::FERR_INVALID_STATE);

	std::array<PropertyCollectionRootStoreOptions, 5U> invalidOptions{
			options(), options(), options(), options(), options()};
	invalidOptions[0].first_slot = {};
	invalidOptions[1].last_slot = {};
	invalidOptions[2].first_slot = PhysicalSlotId(49U);
	invalidOptions[2].last_slot = PhysicalSlotId(48U);
	invalidOptions[3].page_owner_id = 0U;
	invalidOptions[4].maximum_large_value_dedup_candidates = 0U;
	for (const PropertyCollectionRootStoreOptions &invalidOption: invalidOptions) {
		expectFormatError(
				[&] { static_cast<void>(properties_->planEntityRootReplacement(snapshot, valid, invalidOption)); },
				FormatError::FERR_INVALID_HEADER);
	}

	std::array<PropertyCollectionRoot, 3U> invalidRoots{valid, valid, valid};
	invalidRoots[0].owner_entity_type = 999U;
	invalidRoots[1].owner_entity_id = 0U;
	invalidRoots[2].owner_entity_id = std::numeric_limits<uint64_t>::max();
	for (const PropertyCollectionRoot &invalidRoot: invalidRoots) {
		expectFormatError(
				[&] { static_cast<void>(properties_->planEntityRootReplacement(snapshot, invalidRoot, options())); },
				FormatError::FERR_INVALID_HEADER);
	}

	const std::array duplicateRoots{valid, valid};
	expectFormatError(
			[&] { static_cast<void>(properties_->planEntityRootReplacements(snapshot, duplicateRoots, options())); },
			FormatError::FERR_INVALID_HEADER);
	const std::array reversedRoots{root(1002U, 5U), valid};
	expectFormatError(
			[&] { static_cast<void>(properties_->planEntityRootReplacements(snapshot, reversedRoots, options())); },
			FormatError::FERR_INVALID_HEADER);

	const EntityPropertyKey invalidKey{EntityPropertyOwnerKind::EPOK_NODE, 0U};
	expectFormatError([&] { static_cast<void>(properties_->readEntity(snapshot, invalidKey)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(properties_->readEntityOptional(snapshot, invalidKey)); },
					  FormatError::FERR_INVALID_HEADER);
	PropertyCollectionRootScanCursor cursor = properties_->openScanCursor(snapshot);
	expectFormatError([&] { static_cast<void>(cursor.readEntityOptional(snapshot, invalidKey)); },
					  FormatError::FERR_INVALID_HEADER);

	const size_t maximumInline =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
	const PropertyCollectionRoot oversized = root(1001U, 3U, std::string(maximumInline + 256U, 'x'));
	expectFormatError([&] { static_cast<void>(properties_->planEntityRootReplacement(snapshot, oversized, options())); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, ConstructorsRejectForeignMetadataDomains) {
	const std::filesystem::path foreignPath = std::filesystem::temp_directory_path() /
			("property_root_foreign_" + to_string(boost::uuids::random_generator()()));
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock(0x2718281828459045ULL));
	EXPECT_THROW(static_cast<void>(PropertyCollectionRootStore(*pages_, *foreignMetadata)), std::invalid_argument);

	SlottedPagePool foreignPages(foreignIo, *foreignMetadata);
	LargeValueStore localLargeValues(io_, *metadata_);
	EXPECT_THROW(static_cast<void>(PropertyCollectionRootStore(foreignPages, localLargeValues, *metadata_)),
				 std::invalid_argument);

	LargeValueStore foreignLargeValues(foreignIo, *foreignMetadata);
	EXPECT_THROW(static_cast<void>(PropertyCollectionRootStore(*pages_, foreignLargeValues, *metadata_)),
				 std::invalid_argument);

	foreignMetadata.reset();
	foreignIo.reset();
	foreignFile->close();
	foreignFile.reset();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(PropertyCollectionRootStoreValidationTest, DecodeAndMovedCursorRejectForeignState) {
	const PropertyCollectionRoot original = root(1001U, 3U, "cursor root");
	publishNodeAndRoot(original);
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, original.owner_entity_id};
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyLocation location = metadata_->findEntityPropertyLocation(snapshot, key);
	const SlottedPage page = pages_->read(snapshot, location.page_id);

	const EntityPropertyKey foreignKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U};
	expectFormatError(
			[&] {
				static_cast<void>(PropertyCollectionRootStoreTestAccess::decodeEntityRoot(
						*properties_, snapshot, foreignKey, location, page));
			},
			FormatError::FERR_INVALID_STATE);

	const PageOwnerHeader stateOwner{PageId(999U),
								 options().page_owner_id,
								 PageOwnerKind::POK_STATE,
								 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								 Generation(1U),
								 LogSequenceNumber(2U)};
	SlottedPage statePage = SlottedPage::create(stateOwner);
	const SlottedSlotReference stateSlot = statePage.insert(EntityPropertyCollectionRootCodec::encode(original));
	const EntityPropertyLocation stateLocation{key, stateOwner.page_id, stateSlot.slot_id, stateSlot.generation};
	expectFormatError(
			[&] {
				static_cast<void>(PropertyCollectionRootStoreTestAccess::decodeEntityRoot(
						*properties_, snapshot, key, stateLocation, statePage));
			},
			FormatError::FERR_INVALID_STATE);

	PropertyCollectionRootScanCursor first = properties_->openScanCursor(snapshot);
	PropertyCollectionRootScanCursor second(std::move(first));
	expectFormatError([&] { static_cast<void>(first.readEntity(snapshot, key)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(second.readEntity(snapshot, key), original);

	PropertyCollectionRootScanCursor third = properties_->openScanCursor(snapshot);
	third = std::move(second);
	expectFormatError([&] { static_cast<void>(second.readEntity(snapshot, key)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(third.readEntity(snapshot, key), original);
	PropertyCollectionRootScanCursor *self = &third;
	third = std::move(*self);
	EXPECT_EQ(third.readEntity(snapshot, key), original);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, ReadsRejectUnusableTypedPropertyPayloads) {
	const EntityPropertyKey missingStoreKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	static_cast<void>(publishPropertyPayload(
			missingStoreKey, EntityPropertyLargeValueReferenceCodec::encode(largeValueRecord(1001U)),
			PhysicalSlotId(41U)));
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	expectFormatError([&] { static_cast<void>(properties_->readEntity(snapshot, missingStoreKey)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();

	LargeValueStore largeValues(io_, *metadata_);
	PropertyCollectionRootStore propertiesWithLargeValues(*pages_, largeValues, *metadata_);
	EntityPropertyLargeValueReference wrongKind = largeValueRecord(1002U, 2U);
	wrongKind.value.kind = LargeValueKind::LVK_UTF8_TEXT;
	const EntityPropertyKey wrongKindKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U};
	static_cast<void>(publishPropertyPayload(wrongKindKey, EntityPropertyLargeValueReferenceCodec::encode(wrongKind),
												 PhysicalSlotId(42U)));

	EntityPropertyLargeValueReference wrongMedia = largeValueRecord(1003U, 3U);
	wrongMedia.value.media_type = "application/octet-stream";
	const EntityPropertyKey wrongMediaKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	static_cast<void>(publishPropertyPayload(wrongMediaKey, EntityPropertyLargeValueReferenceCodec::encode(wrongMedia),
												  PhysicalSlotId(43U)));

	const EntityPropertyKey mismatchedInlineKey{EntityPropertyOwnerKind::EPOK_NODE, 1004U};
	static_cast<void>(publishPropertyPayload(
			mismatchedInlineKey, EntityPropertyCollectionRootCodec::encode(root(1005U, 7U, "foreign inline owner")),
			PhysicalSlotId(44U)));

	snapshot = metadata_->openSnapshot();
	expectFormatError([&] { static_cast<void>(propertiesWithLargeValues.readEntity(snapshot, wrongKindKey)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(propertiesWithLargeValues.readEntity(snapshot, wrongMediaKey)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(properties_->readEntity(snapshot, mismatchedInlineKey)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, ReadsRejectLargeValuePayloadOwnedByAnotherEntity) {
	LargeValueStore largeValues(io_, *metadata_);
	PropertyCollectionRootStore propertiesWithLargeValues(*pages_, largeValues, *metadata_);
	const PropertyCollectionRoot foreignRoot = root(1002U, 5U, "foreign LargeValue root");
	const std::vector<std::byte> logicalBytes = EntityPropertyCollectionRootCodec::encode(foreignRoot);
	const LargeValueWriteResult written =
			largeValues.write(logicalBytes, largeValueOptions(), nextCheckpointLsn());

	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const EntityPropertyLargeValueReference storedReference{
			key.entity_id, graph::Node::typeId, written.reference};
	static_cast<void>(publishPropertyPayload(
			key, EntityPropertyLargeValueReferenceCodec::encode(storedReference), PhysicalSlotId(41U)));

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	expectFormatError([&] { static_cast<void>(propertiesWithLargeValues.readEntity(snapshot, key)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();

	PropertyCollectionRoot foreignTypeRoot = root(1003U, 7U, "foreign LargeValue owner type");
	foreignTypeRoot.owner_entity_type = graph::Edge::typeId;
	const std::vector<std::byte> foreignTypeBytes =
			EntityPropertyCollectionRootCodec::encode(foreignTypeRoot);
	const LargeValueWriteResult foreignTypeValue =
			largeValues.write(foreignTypeBytes, largeValueOptions(), nextCheckpointLsn());
	const EntityPropertyKey foreignTypeKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	const EntityPropertyLargeValueReference foreignTypeReference{
			foreignTypeKey.entity_id, graph::Node::typeId, foreignTypeValue.reference};
	static_cast<void>(publishPropertyPayload(
			foreignTypeKey, EntityPropertyLargeValueReferenceCodec::encode(foreignTypeReference), PhysicalSlotId(42U)));
	snapshot = metadata_->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(propertiesWithLargeValues.readEntity(snapshot, foreignTypeKey)); },
			FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, BatchPlannerRejectsLargeValueIdentityExhaustionWithoutPublishing) {
	const std::filesystem::path exhaustedPath = std::filesystem::temp_directory_path() /
			("property_root_exhaustion_" + to_string(boost::uuids::random_generator()()));
	{
		auto exhaustedFile = std::make_shared<std::fstream>(
				exhaustedPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(exhaustedFile->is_open());
		auto exhaustedIo =
				std::make_shared<StorageIO>(exhaustedFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		Superblock exhaustedSuperblock = initialSuperblock(0x1618033988749894ULL);
		exhaustedSuperblock.next_large_value_id = std::numeric_limits<uint64_t>::max();
		auto exhaustedMetadata = RelocatableMetadataStore::initializeNew(exhaustedIo, exhaustedSuperblock);
		SlottedPagePool exhaustedPages(exhaustedIo, *exhaustedMetadata);
		LargeValueStore exhaustedLargeValues(exhaustedIo, *exhaustedMetadata);
		PropertyCollectionRootStore exhaustedProperties(
				exhaustedPages, exhaustedLargeValues, *exhaustedMetadata);

		MetadataSnapshotPin snapshot = exhaustedMetadata->openSnapshot();
		const Superblock before = snapshot.superblock();
		const size_t maximumInline =
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		const std::array oversizedRoots{
				root(1001U, 3U, std::string(maximumInline + 256U, 'a')),
				root(1002U, 5U, std::string(maximumInline + 256U, 'b')),
		};
		expectFormatError(
				[&] {
					static_cast<void>(exhaustedProperties.planEntityRootReplacements(
							snapshot, oversizedRoots, options()));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();

		MetadataSnapshotPin after = exhaustedMetadata->openSnapshot();
		EXPECT_EQ(after.superblock(), before);
		EXPECT_FALSE(exhaustedMetadata
						 ->findLargeValueManifestOptional(after, LargeValueId(std::numeric_limits<uint64_t>::max()))
						 .has_value());
		after.release();
	}
	std::error_code error;
	std::filesystem::remove(exhaustedPath, error);
}

TEST_F(PropertyCollectionRootStoreValidationTest, PlannerPreservesNoOpsAndSplitsCoalescingOverflow) {
	const PropertyCollectionRoot initial = root(1001U, 3U, "existing root");
	publishNodeAndRoot(initial);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyRootMutationPlan noOp = properties_->planEntityRootReplacement(snapshot, initial, options());
	EXPECT_EQ(noOp.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE);

	const EntityPropertyLocation initialLocation = metadata_->findEntityPropertyLocation(
			snapshot, {EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id});
	const uint32_t freeBytes = pages_->read(snapshot, initialLocation.page_id).stats().contiguous_free_bytes;
	const auto sizedRoot = [](uint64_t nodeId, uint32_t token, size_t encodedBytes, char fill) {
		PropertyCollectionRoot result = root(nodeId, token, "");
		const size_t baseBytes = EntityPropertyCollectionRootCodec::encode(result).size();
		if (encodedBytes <= baseBytes) {
			throw std::logic_error("Property validation test requested an undersized encoded root");
		}
		result.entries.front().value = graph::PropertyValue(std::string(encodedBytes - baseBytes, fill));
		return result;
	};
	const size_t targetBytes = freeBytes / 2U;
	const PropertyCollectionRoot second = sizedRoot(1002U, 5U, targetBytes, 'a');
	const PropertyCollectionRoot third = sizedRoot(1003U, 7U, targetBytes, 'b');
	ASSERT_LE(EntityPropertyCollectionRootCodec::encode(second).size() + RSS_SLOTTED_PAGE_SLOT_BYTES, freeBytes);
	ASSERT_GT(2U * (EntityPropertyCollectionRootCodec::encode(second).size() + RSS_SLOTTED_PAGE_SLOT_BYTES), freeBytes);
	const std::array roots{second, third};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(snapshot, roots, options());
	ASSERT_EQ(plans.size(), 2U);
	EXPECT_EQ(std::ranges::count(plans, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE,
								  &EntityPropertyRootMutationPlan::kind),
			  1);
	EXPECT_EQ(std::ranges::count(plans, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED,
								  &EntityPropertyRootMutationPlan::kind),
			  1);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, PlannerMovesGrowingRootWhenSharedPageCannotFit) {
	const PropertyCollectionRoot initial = root(1001U, 3U, "small root");
	publishNodeAndRoot(initial);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyLocation initialLocation = metadata_->findEntityPropertyLocation(
			snapshot, {EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id});
	snapshot.release();

	PropertyCollectionRoot blocker = root(1002U, 5U, "");
	const size_t blockerBase = EntityPropertyCollectionRootCodec::encode(blocker).size();
	blocker.entries.front().value = graph::PropertyValue(std::string(2500U - blockerBase, 'b'));
	const EntityPropertyKey blockerKey{EntityPropertyOwnerKind::EPOK_NODE, blocker.owner_entity_id};
	const EntityPropertyLocation blockerLocation = appendPropertyPayload(
			initialLocation.page_id, {EntityPropertyOwnerKind::EPOK_NODE, blocker.owner_entity_id},
			EntityPropertyCollectionRootCodec::encode(blocker));

	PropertyCollectionRoot replacement = root(1001U, 3U, "");
	const size_t replacementBase = EntityPropertyCollectionRootCodec::encode(replacement).size();
	replacement.entries.front().value = graph::PropertyValue(std::string(2000U - replacementBase, 'r'));
	snapshot = metadata_->openSnapshot();
	const EntityPropertyRootMutationPlan plan =
			properties_->planEntityRootReplacement(snapshot, replacement, options());
	EXPECT_EQ(plan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	EXPECT_FALSE(plan.encoded_root.empty());
	ASSERT_TRUE(plan.page_replacement.has_value());
	EXPECT_EQ(plan.page_replacement->relocated_properties, std::vector<EntityPropertyKey>{plan.key});

	const EntityPageBatchResult published =
			test::publishPropertyRoot(*entities_, *properties_, *metadata_, replacement, options());
	ASSERT_EQ(published.properties.size(), 1U);
	EXPECT_NE(published.properties.front().page_id, initialLocation.page_id);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(current, plan.key), replacement);
	EXPECT_EQ(properties_->readEntity(current, blockerKey), blocker);
	const EntityPropertyLocation relocated = metadata_->findEntityPropertyLocation(current, plan.key);
	EXPECT_NE(relocated.page_id, initialLocation.page_id);
	EXPECT_EQ(metadata_->findEntityPropertyLocation(current, blockerKey), blockerLocation);
	EXPECT_EQ(metadata_->findPageLocation(current, initialLocation.page_id).page_id, initialLocation.page_id);
	const SlottedPage currentSource = pages_->read(current, initialLocation.page_id);
	expectFormatError(
			[&] {
				static_cast<void>(currentSource.read(
						{initialLocation.page_id, initialLocation.slot_id, initialLocation.slot_generation}));
			},
			FormatError::FERR_INVALID_STATE);
	current.release();

	// The old snapshot retains the pre-relocation Property Directory and source frame.
	EXPECT_EQ(properties_->readEntity(snapshot, plan.key), initial);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, BatchRelocationRetiresAnEmptiedSourcePageAtomically) {
	const auto sizedRoot = [](uint64_t nodeId, uint32_t token, size_t encodedBytes, char fill) {
		PropertyCollectionRoot result = root(nodeId, token, "");
		const size_t baseBytes = EntityPropertyCollectionRootCodec::encode(result).size();
		if (encodedBytes <= baseBytes) {
			throw std::logic_error("Property relocation test requested an undersized encoded root");
		}
		result.entries.front().value = graph::PropertyValue(std::string(encodedBytes - baseBytes, fill));
		return result;
	};

	const PropertyCollectionRoot first = sizedRoot(1001U, 3U, 1700U, 'a');
	const PropertyCollectionRoot second = sizedRoot(1002U, 5U, 1700U, 'b');
	publishNodeAndRoot(first);
	MetadataSnapshotPin nodeSnapshot = metadata_->openSnapshot();
	static_cast<void>(entities_->insertNode(nodeSnapshot, nodePool().page_id, second.owner_entity_id,
										  NodeEntityRecord{}, nextCheckpointLsn()));
	nodeSnapshot.release();

	MetadataSnapshotPin locationSnapshot = metadata_->openSnapshot();
	const EntityPropertyKey firstKey{EntityPropertyOwnerKind::EPOK_NODE, first.owner_entity_id};
	const EntityPropertyLocation firstLocation = metadata_->findEntityPropertyLocation(locationSnapshot, firstKey);
	locationSnapshot.release();
	const EntityPropertyKey secondKey{EntityPropertyOwnerKind::EPOK_NODE, second.owner_entity_id};
	static_cast<void>(appendPropertyPayload(firstLocation.page_id, secondKey,
										 EntityPropertyCollectionRootCodec::encode(second)));

	const PropertyCollectionRoot firstReplacement = sizedRoot(1001U, 3U, 2400U, 'x');
	const PropertyCollectionRoot secondReplacement = sizedRoot(1002U, 5U, 2400U, 'y');
	const std::array replacements{firstReplacement, secondReplacement};
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(oldSnapshot, replacements, options());
	ASSERT_EQ(plans.size(), 2U);
	ASSERT_TRUE(plans.front().page_replacement.has_value());
	EXPECT_TRUE(plans.front().page_replacement->retire_page);
	EXPECT_EQ(plans.front().page_replacement->relocated_properties,
			  (std::vector<EntityPropertyKey>{firstKey, secondKey}));
	EXPECT_TRUE(plans[1].page_replacement.has_value() == false);

	const EntityPageBatchResult published =
			test::publishPropertyRoots(*entities_, *properties_, *metadata_, replacements, options());
	ASSERT_EQ(published.properties.size(), 2U);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(current, firstKey), firstReplacement);
	EXPECT_EQ(properties_->readEntity(current, secondKey), secondReplacement);
	EXPECT_FALSE(metadata_->findPageLocationOptional(current, firstLocation.page_id).has_value());
	EXPECT_NE(metadata_->findEntityPropertyLocation(current, firstKey).page_id, firstLocation.page_id);
	EXPECT_NE(metadata_->findEntityPropertyLocation(current, secondKey).page_id, firstLocation.page_id);
	current.release();

	EXPECT_EQ(properties_->readEntity(oldSnapshot, firstKey), first);
	EXPECT_EQ(properties_->readEntity(oldSnapshot, secondKey), second);
	EXPECT_TRUE(metadata_->findPageLocationOptional(oldSnapshot, firstLocation.page_id).has_value());
	oldSnapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, NewPoolPreparationRejectsMalformedRelocationSources) {
	const PropertyCollectionRoot initial = root(1001U, 3U, "small source root");
	publishNodeAndRoot(initial);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyLocation initialLocation = metadata_->findEntityPropertyLocation(
			snapshot, {EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id});
	snapshot.release();

	PropertyCollectionRoot blocker = root(1002U, 5U, "");
	const size_t blockerBase = EntityPropertyCollectionRootCodec::encode(blocker).size();
	blocker.entries.front().value = graph::PropertyValue(std::string(2500U - blockerBase, 'b'));
	static_cast<void>(appendPropertyPayload(
			initialLocation.page_id, {EntityPropertyOwnerKind::EPOK_NODE, blocker.owner_entity_id},
			EntityPropertyCollectionRootCodec::encode(blocker)));

	PropertyCollectionRoot replacement = root(1001U, 3U, "");
	const size_t replacementBase = EntityPropertyCollectionRootCodec::encode(replacement).size();
	replacement.entries.front().value = graph::PropertyValue(std::string(2000U - replacementBase, 'r'));
	snapshot = metadata_->openSnapshot();
	const EntityPropertyRootMutationPlan baseline =
			properties_->planEntityRootReplacement(snapshot, replacement, options());
	ASSERT_EQ(baseline.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	ASSERT_TRUE(baseline.page_replacement.has_value());
	ASSERT_FALSE(baseline.page_replacement->retire_page);
	ASSERT_FALSE(baseline.replacement_payload.empty());
	const std::array<std::vector<size_t>, 1U> pageOrdinals{std::vector<size_t>{0U}};
	const LogSequenceNumber checkpointLsn = nextCheckpointLsn();
	const NativeGraphPropertyPageBatchMaterialization baselineMaterialization = newPoolMaterialization(
			baseline, checkpointLsn, SegmentId(99U), PhysicalSlotId(48U), PageId(999U));

	const std::array<std::function<void(EntityPropertyRootMutationPlan &)>, 6U> corruptSources{
			[](auto &value) {
				value.page_replacement->retire_page = true;
				value.page_replacement->upserted_properties.push_back(
						{value.key, value.page_replacement->expected_location.page_id, 0U, 1U});
			},
			[](auto &value) { value.page_replacement->retire_page = true; },
			[](auto &value) {
				value.page_replacement->retire_page = true;
				value.replacement_payload.clear();
				value.replacement_free_bytes = 1U;
			},
			[](auto &value) { value.replacement_payload.pop_back(); },
			[](auto &value) { value.replacement_free_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES; },
			[](auto &value) {
				value.page_replacement->relocated_properties = {
						{EntityPropertyOwnerKind::EPOK_NODE, value.key.entity_id + 1U}};
			},
	};
	for (size_t index = 0U; index < corruptSources.size(); ++index) {
		SCOPED_TRACE(::testing::Message() << "corrupt relocation source " << index);
		std::array plans{baseline};
		auto materialization = baselineMaterialization;
		corruptSources[index](plans.front());
		expectFormatError(
				[&] {
				properties_->preparePlannedEntityRootPageBatch(plans, pageOrdinals, materialization, checkpointLsn);
				},
				FormatError::FERR_INVALID_STATE);
	}
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, NewPoolPreparationRejectsChangedMetadataAndMalformedPlans) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PropertyCollectionRoot firstRoot = root(1001U, 3U);
	const EntityPropertyRootMutationPlan baseline =
			properties_->planEntityRootReplacement(snapshot, firstRoot, options());
	ASSERT_EQ(baseline.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const std::array baselinePlans{baseline};
	const std::array<std::vector<size_t>, 1U> baselinePageOrdinals{std::vector<size_t>{0U}};
	const NativeGraphPropertyPageBatchMaterialization baselineMaterialization = newPoolMaterialization(baseline);

	const std::array<EntityPropertyRootMutationPlan, 0U> noPlans{};
	const std::array<std::vector<size_t>, 0U> noPageOrdinals{};
	auto materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(noPlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(baselinePlans, noPageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<std::vector<size_t>, 1U> duplicatePageOrdinals{std::vector<size_t>{0U, 0U}};
	materialization = baselineMaterialization;
	materialization.pages.front().upserted_properties.resize(
			2U, materialization.pages.front().upserted_properties.front());
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(baselinePlans, duplicatePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(
						baselinePlans, baselinePageOrdinals, materialization, {});
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<std::function<void(NativeGraphPropertyPageBatchMaterialization &)>, 10U> corruptMetadata{
			[](auto &value) { value.kind = static_cast<NativeGraphPropertyPageBatchKind>(0U); },
			[](auto &value) { value.pool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL; },
			[](auto &value) { ++value.pool.owner_id; },
			[](auto &value) { value.pool.generation = Generation(2U); },
			[](auto &value) { value.pool.state = SegmentState::SGS_BUILDING; },
			[](auto &value) { value.pool.committed_lsn = LogSequenceNumber(3U); },
			[](auto &value) { value.pages.front().location.segment_id = SegmentId(99U); },
			[](auto &value) { value.pages.front().location.generation = Generation(2U); },
			[](auto &value) { value.pages.front().location.page_index = 1U; },
			[](auto &value) { value.pages.front().upserted_properties.clear(); },
	};
	for (const auto &corrupt: corruptMetadata) {
		materialization = baselineMaterialization;
		corrupt(materialization);
		expectFormatError(
				[&] {
					properties_->preparePlannedEntityRootPageBatch(
							baselinePlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
				},
				FormatError::FERR_INVALID_STATE);
	}

	const std::array<std::vector<size_t>, 1U> invalidPageOrdinal{std::vector<size_t>{1U}};
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(baselinePlans, invalidPageOrdinal, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);

	const size_t maximumBytes =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
	const std::array<std::function<void(EntityPropertyRootMutationPlan &)>, 10U> corruptPlans{
			[](auto &value) { value.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE; },
			[](auto &value) { value.key.entity_id = 0U; },
			[](auto &value) { value.page_replacement.emplace(); },
			[](auto &value) { value.replacement_payload.push_back(std::byte{1U}); },
			[](auto &value) { value.replacement_free_bytes = 1U; },
			[](auto &value) { value.encoded_root.clear(); },
			[maximumBytes](auto &value) { value.encoded_root.resize(maximumBytes + 1U); },
			[](auto &value) { ++value.key.entity_id; },
			[](auto &) {},
			[](auto &) {},
	};
	for (size_t index = 0U; index < corruptPlans.size(); ++index) {
		std::array plans{baseline};
		materialization = baselineMaterialization;
		if (index == 8U) {
			materialization.pages.front().upserted_properties.front().page_id = PageId(999U);
		} else if (index == 9U) {
			materialization.pages.front().upserted_properties.front().slot_id = 1U;
		} else {
			corruptPlans[index](plans.front());
			if (index == 7U) {
				materialization.pages.front().upserted_properties.front().key = baseline.key;
			}
		}
		expectFormatError(
				[&] {
					properties_->preparePlannedEntityRootPageBatch(plans, baselinePageOrdinals, materialization,
																LogSequenceNumber(2U));
				},
				FormatError::FERR_INVALID_STATE);
	}
	std::array generationPlans{baseline};
	materialization = baselineMaterialization;
	materialization.pages.front().upserted_properties.front().slot_generation = 1U;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(
						generationPlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);

	std::array malformedRecordPlans{baseline};
	malformedRecordPlans.front().encoded_root =
			EntityPropertyCollectionRootCodec::encode(root(1002U, 5U, "foreign owner"));
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(
						malformedRecordPlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	PropertyCollectionRoot emptyRoot = firstRoot;
	emptyRoot.entries.clear();
	malformedRecordPlans.front() = baseline;
	malformedRecordPlans.front().encoded_root = EntityPropertyCollectionRootCodec::encode(emptyRoot);
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(
						malformedRecordPlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	malformedRecordPlans.front() = baseline;
	malformedRecordPlans.front().encoded_root =
			EntityPropertyLargeValueReferenceCodec::encode(largeValueRecord(1001U));
	materialization = baselineMaterialization;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootPageBatch(
						malformedRecordPlans, baselinePageOrdinals, materialization,
															LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, ExistingPagePreparationRejectsChangedPlansAndPayloads) {
	const PropertyCollectionRoot original = root(1001U, 3U, "original root");
	publishNodeAndRoot(original);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PropertyCollectionRoot replacement = root(1001U, 3U, "replacement root");
	const EntityPropertyRootMutationPlan baseline =
			properties_->planEntityRootReplacement(snapshot, replacement, options());
	ASSERT_EQ(baseline.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(baseline.page_replacement.has_value());
	const NativeGraphPropertyPageReplacementMaterialization baselineMaterialization =
			replacementMaterialization(baseline);

	EntityPropertyRootMutationPlan plan = baseline;
	auto materialization = baselineMaterialization;
	plan.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE;
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);
	plan = baseline;
	plan.page_replacement.reset();
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);
	plan = baseline;
	expectFormatError([&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, {}); },
					  FormatError::FERR_INVALID_STATE);

	const std::array<std::function<void(NativeGraphPropertyPageReplacementRequest &)>, 10U> corruptRequests{
			[](auto &value) { value.expected_pool.segment_id = SegmentId(99U); },
			[](auto &value) { value.expected_pool.physical_slot = PhysicalSlotId(99U); },
			[](auto &value) { value.expected_pool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL; },
			[](auto &value) { ++value.expected_pool.owner_id; },
			[](auto &value) { value.expected_pool.generation = Generation(2U); },
			[](auto &value) { value.expected_pool.committed_lsn = LogSequenceNumber(99U); },
			[](auto &value) { ++value.expected_location.page_index; },
			[](auto &value) { ++value.expected_page_owner_id; },
			[](auto &value) { value.expected_owner_lsn = LogSequenceNumber(99U); },
			[](auto &value) { value.retire_page = true; },
	};
	for (size_t index = 0U; index < corruptRequests.size(); ++index) {
		SCOPED_TRACE(::testing::Message() << "corrupt request " << index);
		plan = baseline;
		materialization = baselineMaterialization;
		corruptRequests[index](*plan.page_replacement);
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}

	{
		SCOPED_TRACE("short replacement payload");
		plan = baseline;
		materialization = baselineMaterialization;
		plan.replacement_payload.pop_back();
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	{
		SCOPED_TRACE("invalid replacement free bytes");
		plan = baseline;
		materialization = baselineMaterialization;
		plan.replacement_free_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	{
		SCOPED_TRACE("replacement free bytes disagree with payload");
		plan = baseline;
		materialization = baselineMaterialization;
		ASSERT_GT(plan.replacement_free_bytes, 0U);
		--plan.replacement_free_bytes;
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	{
		SCOPED_TRACE("existing plan encoded root");
		plan = baseline;
		materialization = baselineMaterialization;
		plan.encoded_root.push_back(std::byte{1U});
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}

	plan = baseline;
	materialization = baselineMaterialization;
	const PageOwnerHeader foreignOwner{PageId(999U),
									 options().page_owner_id,
									 PageOwnerKind::POK_PROPERTY,
									 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 baselineMaterialization.pool.generation,
									 LogSequenceNumber(4U)};
	SlottedPage foreignPage = SlottedPage::create(foreignOwner);
	static_cast<void>(foreignPage.insert(EntityPropertyCollectionRootCodec::encode(replacement)));
	plan.replacement_payload = foreignPage.serialize();
	plan.replacement_free_bytes = foreignPage.stats().contiguous_free_bytes;
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);

	const std::array<std::function<void(EntityPropertyLocation &)>, 4U> corruptLocations{
			[](auto &value) { value.key.entity_id = 0U; },
			[](auto &value) { value.page_id = PageId(999U); },
			[](auto &value) { value.slot_id = std::numeric_limits<uint16_t>::max(); },
			[](auto &value) { value.slot_generation = 0U; },
	};
	for (size_t index = 0U; index < corruptLocations.size(); ++index) {
		SCOPED_TRACE(::testing::Message() << "corrupt upsert location " << index);
		plan = baseline;
		materialization = baselineMaterialization;
		corruptLocations[index](plan.page_replacement->upserted_properties.front());
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	plan = baseline;
	materialization = baselineMaterialization;
	plan.page_replacement->upserted_properties.push_back(plan.page_replacement->upserted_properties.front());
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);
	plan = baseline;
	materialization = baselineMaterialization;
	EntityPropertyLocation invalidAdditionalLocation = plan.page_replacement->upserted_properties.front();
	invalidAdditionalLocation.key.entity_id = 0U;
	plan.page_replacement->upserted_properties.push_back(invalidAdditionalLocation);
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);

	const EntityPropertyKey invalidMutationKey{EntityPropertyOwnerKind::EPOK_NODE, 0U};
	const std::array<std::function<void(NativeGraphPropertyPageReplacementRequest &)>, 4U>
			corruptSecondaryMutations{
					[invalidMutationKey](auto &value) { value.erased_properties.push_back(invalidMutationKey); },
					[](auto &value) { value.erased_properties.push_back(value.upserted_properties.front().key); },
					[invalidMutationKey](auto &value) { value.relocated_properties.push_back(invalidMutationKey); },
					[](auto &value) { value.relocated_properties.push_back(value.upserted_properties.front().key); },
			};
	for (size_t index = 0U; index < corruptSecondaryMutations.size(); ++index) {
		SCOPED_TRACE(::testing::Message() << "corrupt secondary mutation " << index);
		plan = baseline;
		materialization = baselineMaterialization;
		corruptSecondaryMutations[index](*plan.page_replacement);
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	plan = baseline;
	materialization = baselineMaterialization;
	SlottedPage changed = SlottedPage::open(plan.replacement_payload);
	const EntityPropertyLocation &storedLocation = plan.page_replacement->upserted_properties.front();
	changed.overwrite({storedLocation.page_id, storedLocation.slot_id, storedLocation.slot_generation},
					  EntityPropertyCollectionRootCodec::encode(root(1002U, 5U, "wrong owner")));
	plan.replacement_payload = changed.serialize();
	plan.replacement_free_bytes = changed.stats().contiguous_free_bytes;
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);

	plan = baseline;
	materialization = baselineMaterialization;
	plan.large_values.push_back({.reference = fakeLargeValueRef()});
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, MaterializerRejectsAmbiguousLargeValueReachability) {
	const PropertyCollectionRoot original = root(1001U, 3U, "original root");
	publishNodeAndRoot(original);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PropertyCollectionRoot replacement = root(1001U, 3U, "replacement root");
	const EntityPropertyRootMutationPlan baseline =
			properties_->planEntityRootReplacement(snapshot, replacement, options());
	ASSERT_TRUE(baseline.page_replacement.has_value());
	const NativeGraphPropertyPageReplacementMaterialization baselineMaterialization =
			replacementMaterialization(baseline);
	const PageOwnerHeader owner{baseline.page_replacement->expected_location.page_id,
								 options().page_owner_id,
								 PageOwnerKind::POK_PROPERTY,
								 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								 baselineMaterialization.pool.generation,
								 LogSequenceNumber(4U)};

	EntityPropertyRootMutationPlan duplicated = baseline;
	SlottedPage duplicatePage = SlottedPage::create(owner);
	const LargeValueRef sharedReference = fakeLargeValueRef();
	const EntityPropertyLargeValueReference firstStored{1001U, graph::Node::typeId, sharedReference};
	const EntityPropertyLargeValueReference secondStored{1002U, graph::Node::typeId, sharedReference};
	const SlottedSlotReference firstSlot =
			duplicatePage.insert(EntityPropertyLargeValueReferenceCodec::encode(firstStored));
	const SlottedSlotReference secondSlot =
			duplicatePage.insert(EntityPropertyLargeValueReferenceCodec::encode(secondStored));
	duplicated.page_replacement->upserted_properties = {
			{{EntityPropertyOwnerKind::EPOK_NODE, 1001U}, owner.page_id, firstSlot.slot_id, firstSlot.generation},
			{{EntityPropertyOwnerKind::EPOK_NODE, 1002U}, owner.page_id, secondSlot.slot_id, secondSlot.generation},
	};
	duplicated.page_replacement->erased_properties.clear();
	duplicated.replacement_payload = duplicatePage.serialize();
	duplicated.replacement_free_bytes = duplicatePage.stats().contiguous_free_bytes;
	duplicated.large_values = {{.reference = sharedReference}};
	auto materialization = baselineMaterialization;
	expectFormatError(
			[&] { properties_->preparePlannedEntityRootReplacement(duplicated, materialization, LogSequenceNumber(4U)); },
			FormatError::FERR_INVALID_STATE);

	EntityPropertyRootMutationPlan eraseWithReference = baseline;
	SlottedPage survivingPage = SlottedPage::create(owner);
	static_cast<void>(survivingPage.insert(EntityPropertyCollectionRootCodec::encode(root(1002U, 5U, "survivor"))));
	eraseWithReference.page_replacement->upserted_properties.clear();
	eraseWithReference.page_replacement->erased_properties = {eraseWithReference.key};
	eraseWithReference.page_replacement->retire_page = false;
	eraseWithReference.replacement_payload = survivingPage.serialize();
	eraseWithReference.replacement_free_bytes = survivingPage.stats().contiguous_free_bytes;
	eraseWithReference.reused_large_values = {sharedReference};
	materialization = baselineMaterialization;
	materialization.retire_page = false;
	expectFormatError(
			[&] {
				properties_->preparePlannedEntityRootReplacement(eraseWithReference, materialization,
															 LogSequenceNumber(4U));
			},
			FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(PropertyCollectionRootStoreValidationTest, RetirementAndLargeValuePreparationRejectInvalidCombinations) {
	const PropertyCollectionRoot original = root(1001U, 3U, "retired root");
	publishNodeAndRoot(original);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	PropertyCollectionRoot empty = original;
	empty.entries.clear();
	const EntityPropertyRootMutationPlan baseline = properties_->planEntityRootReplacement(snapshot, empty, options());
	ASSERT_TRUE(baseline.page_replacement.has_value());
	ASSERT_TRUE(baseline.page_replacement->retire_page);
	const NativeGraphPropertyPageReplacementMaterialization baselineMaterialization =
			replacementMaterialization(baseline);

	const std::array<std::function<void(EntityPropertyRootMutationPlan &)>, 4U> corruptRetirements{
			[](auto &value) { value.page_replacement->erased_properties.clear(); },
			[](auto &value) {
				value.page_replacement->upserted_properties.push_back(
						{value.key, value.page_replacement->expected_location.page_id, 0U, 1U});
			},
			[](auto &value) { value.replacement_payload.push_back(std::byte{1U}); },
			[](auto &value) { value.replacement_free_bytes = 1U; },
	};
	for (size_t index = 0U; index < corruptRetirements.size(); ++index) {
		SCOPED_TRACE(::testing::Message() << "corrupt retirement " << index);
		EntityPropertyRootMutationPlan plan = baseline;
		auto materialization = baselineMaterialization;
		corruptRetirements[index](plan);
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}

	EntityPropertyRootMutationPlan plan = baseline;
	auto materialization = baselineMaterialization;
	plan.large_values.push_back({.reference = fakeLargeValueRef()});
	{
		SCOPED_TRACE("retirement with new LargeValue");
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}

	plan = baseline;
	materialization = baselineMaterialization;
	plan.reused_large_values.push_back(fakeLargeValueRef());
	{
		SCOPED_TRACE("retirement with reused LargeValue");
		expectFormatError(
				[&] { properties_->preparePlannedEntityRootReplacement(plan, materialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}

	NativeGraphLargeValueMutation mutation{.reference = fakeLargeValueRef()};
	LargeValueMaterializationResult largeMaterialization;
	{
		SCOPED_TRACE("LargeValue preparation without store");
		expectFormatError(
				[&] { properties_->preparePlannedLargeValue(mutation, largeMaterialization, LogSequenceNumber(4U)); },
				FormatError::FERR_INVALID_STATE);
	}
	snapshot.release();
}
