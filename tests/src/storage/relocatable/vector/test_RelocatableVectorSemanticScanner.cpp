/**
 * @file test_RelocatableVectorSemanticScanner.cpp
 * @brief Logical Vector extraction tests for native relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/RelocatableVectorSemanticScanner.hpp"
#include "graph/vector/core/BFloat16.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x0123456789ABCDEFULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	std::vector<std::byte> raw(std::initializer_list<float> values) {
		std::vector<std::byte> result(values.size() * sizeof(uint16_t));
		size_t offset = 0;
		for (const float value: values) {
			const vector::BFloat16 encoded(value);
			LittleEndianCodec::writeU16(result, offset, encoded.data);
			offset += sizeof(uint16_t);
		}
		return result;
	}

} // namespace

class RelocatableVectorSemanticScannerTest : public ::testing::Test {
protected:
	std::filesystem::path destinationPath;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> destinationFile;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> destinationIo;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		const std::filesystem::path prefix =
				std::filesystem::temp_directory_path() / ("relocatable_vector_semantic_" + to_string(uuid));
		destinationPath = prefix.string() + ".relocatable";
		walPath = destinationPath.string() + ".foreign";
		destinationFile = std::make_shared<std::fstream>(
				destinationPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(destinationFile->is_open());
		ASSERT_TRUE(walFile->is_open());
		destinationIo = std::make_shared<StorageIO>(destinationFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		destinationIo.reset();
		walIo.reset();
		for (const std::shared_ptr<std::fstream> &file: {destinationFile, walFile}) {
			if (file && file->is_open()) {
				file->close();
			}
		}
		std::error_code error;
		std::filesystem::remove(destinationPath, error);
		std::filesystem::remove(walPath, error);
	}

	[[nodiscard]] RelocatableVectorSemanticScanOptions scanOptions() const {
		return {4U, 1U, 16U, 4U, 2U};
	}

	[[nodiscard]] static NativeGraphWritePlannerOptions catalogPlannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(61U);
		options.last_slot = PhysicalSlotId(70U);
		options.entity_pool_owner_id = 402U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
		return options;
	}

	static void publishPropertyTokens(SystemCatalog &catalog, NativeGraphWritePlanner &planner,
								  std::initializer_list<std::string_view> names) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		for (const std::string_view name: names) {
			static_cast<void>(session.ensurePropertyToken(name));
		}
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	void populateNative(EntityPageStore &entities, RelocatableMetadataStore &metadata, SlottedPagePool &pool) const {
		const auto nextLsn = [&metadata] {
			MetadataSnapshotPin snapshot = metadata.openSnapshot();
			const LogSequenceNumber result(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			return result;
		};
		PropertyCollectionRootStore roots(pool, metadata);
		const auto root = [](uint64_t ownerId,
							 const std::vector<PropertyCollectionRootEntry> &entries) {
			PropertyCollectionRoot value;
			value.owner_entity_id = ownerId;
			value.owner_entity_type = Node::typeId;
			value.entries = entries;
			return value;
		};
		const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
		for (uint64_t index = 0U; index < 5U; ++index) {
			NodeEntityRecord node;
			node.inline_label_ids[0] = index == 2U ? 12U : 11U;
			node.inline_label_count = 1U;
			const EntityPageNewPoolRequest request{
				.segment_id = SegmentId(101U + index),
				.pool_owner_id = 11U,
				.generation = Generation(1U),
				.page_id = PageId(101U + index),
				.first_slot = PhysicalSlotId(71U + index),
				.last_slot = PhysicalSlotId(71U + index)};
			ASSERT_TRUE(entities.allocateNode(request, index + 1U, node, nextLsn()).has_value());
		}
		const std::array propertyRoots{
				root(1U, {{1U, PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																	PropertyValue(2.5)})}}),
				root(2U, {{1U, PropertyValue(std::vector<PropertyValue>{PropertyValue(-3.0),
																	PropertyValue(int64_t(4))})}}),
				root(3U, {{1U, PropertyValue(std::vector<PropertyValue>{PropertyValue(9.0),
																	PropertyValue(10.0)})}}),
				root(5U, {{2U, PropertyValue(int64_t(7))}})};
		static_cast<void>(test::publishPropertyRoots(entities, roots, metadata, propertyRoots, propertyOptions));
		BTreePageStore pages(pool, metadata);
		EntityLabelSetStore labels(destinationIo, metadata, pool);
		SystemCatalog catalog(pages, metadata);
		NativeGraphWritePlanner planner(metadata, entities, labels, roots, pages, catalogPlannerOptions());
		publishPropertyTokens(catalog, planner, {"embedding", "other"});
	}
};

TEST_F(RelocatableVectorSemanticScannerTest, ExtractsScopedVectorPropertiesAsCanonicalBFloat16Records) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition definition{7,
							 "member_embedding",
							 IndexDefinitionEntityKind::IDEK_NODE,
							 IndexDefinitionKind::IDK_VECTOR,
							 11,
							 {"embedding"},
							 2,
							 "l2"};
	std::vector<RelocatableVectorSemanticRecord> records;
	const RelocatableVectorSemanticScanResult result = RelocatableVectorSemanticScanner::scan(
			std::span(&definition, 1), entities, labels, roots, catalog, *metadata, scanOptions(),
			[&records](std::span<const RelocatableVectorSemanticRecord> batch) {
				records.insert(records.end(), batch.begin(), batch.end());
			});
	EXPECT_EQ(records,
			  (std::vector<RelocatableVectorSemanticRecord>{{7, 1, raw({1.0F, 2.5F})}, {7, 2, raw({-3.0F, 4.0F})}}));
	EXPECT_EQ(result.vector_count, 2U);
	EXPECT_NE(result.semantic_crc, 0U);
}

TEST_F(RelocatableVectorSemanticScannerTest, RejectsInvalidBoundsAndDimensionMismatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition definition{7,
							 "member_embedding",
							 IndexDefinitionEntityKind::IDEK_NODE,
							 IndexDefinitionKind::IDK_VECTOR,
							 11,
							 {"embedding"},
							 3,
							 "l2"};
	RelocatableVectorSemanticScanOptions invalid = scanOptions();
	invalid.maximum_property_names = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(RelocatableVectorSemanticScanner::scan(
						std::span(&definition, 1), entities, labels, roots, catalog, *metadata, invalid,
						[](std::span<const RelocatableVectorSemanticRecord>) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(RelocatableVectorSemanticScanner::scan(
						std::span(&definition, 1), entities, labels, roots, catalog, *metadata, scanOptions(),
						[](std::span<const RelocatableVectorSemanticRecord>) {}));
			},
		FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorSemanticScannerTest, FiltersNonMatchingDefinitionsAndRejectsSemanticInputMismatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition vectorDefinition{7U,
													 "member_embedding",
													 IndexDefinitionEntityKind::IDEK_NODE,
													 IndexDefinitionKind::IDK_VECTOR,
													 11U,
													 {"embedding"},
													 2U,
													 "l2"};
	const auto consume = [](std::span<const RelocatableVectorSemanticRecord>) {};

	IndexDefinition scalarDefinition;
	scalarDefinition.kind = IndexDefinitionKind::IDK_PROPERTY;
	EXPECT_EQ(RelocatableVectorSemanticScanner::scan(std::span(&scalarDefinition, 1U), entities, labels, roots, catalog, *metadata,
														 scanOptions(), consume),
			  (RelocatableVectorSemanticScanResult{}));

	IndexDefinition unmatchedLabel = vectorDefinition;
	unmatchedLabel.label_token_id = 99U;
	EXPECT_EQ(RelocatableVectorSemanticScanner::scan(std::span(&unmatchedLabel, 1U), entities, labels, roots, catalog, *metadata,
														 scanOptions(), consume),
			  (RelocatableVectorSemanticScanResult{}));

	const RelocatableVectorSemanticBatchConsumer noConsumer;
	expectFormatError(
			[&] {
				static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&vectorDefinition, 1U), entities, labels, roots,
															 catalog, *metadata, scanOptions(), noConsumer));
			},
			FormatError::FERR_INVALID_STATE);

	const std::array definitions = {vectorDefinition, vectorDefinition};
	expectFormatError(
			[&] {
				RelocatableVectorSemanticScanOptions bounded = scanOptions();
				bounded.maximum_definitions = 1U;
				static_cast<void>(RelocatableVectorSemanticScanner::scan(definitions, entities, labels, roots, catalog, *metadata, bounded,
																					 consume));
			},
			FormatError::FERR_INVALID_STATE);

	IndexDefinition missingToken = vectorDefinition;
	missingToken.property_names = {"absent_embedding"};
	expectFormatError(
			[&] {
					static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&missingToken, 1U), entities, labels, roots, catalog,
															  *metadata, scanOptions(), consume));
			},
			FormatError::FERR_INVALID_STATE);

	expectFormatError(
			[&] {
				RelocatableVectorSemanticScanOptions bounded = scanOptions();
				bounded.maximum_vectors_per_index = 1U;
				static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&vectorDefinition, 1U), entities, labels, roots,
															 catalog, *metadata, bounded, consume));
			},
		FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorSemanticScannerTest, FlushesTheFinalPartialBatchAfterSkippingIneligibleNodes) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition definition{7U,
											 "member_embedding",
											 IndexDefinitionEntityKind::IDEK_NODE,
											 IndexDefinitionKind::IDK_VECTOR,
											 11U,
											 {"embedding"},
											 2U,
											 "l2"};
	RelocatableVectorSemanticScanOptions options = scanOptions();
	options.maximum_batch_records = 4U;
	std::vector<RelocatableVectorSemanticRecord> records;
	const RelocatableVectorSemanticScanResult result = RelocatableVectorSemanticScanner::scan(
			std::span(&definition, 1U), entities, labels, roots, catalog, *metadata, options,
			[&records](std::span<const RelocatableVectorSemanticRecord> batch) {
				records.insert(records.end(), batch.begin(), batch.end());
			});
	EXPECT_EQ(records,
			  (std::vector<RelocatableVectorSemanticRecord>{{7U, 1U, raw({1.0F, 2.5F})},
																		 {7U, 2U, raw({-3.0F, 4.0F})}}));
	EXPECT_EQ(result.vector_count, records.size());
}

TEST_F(RelocatableVectorSemanticScannerTest, RejectsEachStoreFromAnotherMetadataDomain) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition definition{7U,
											 "member_embedding",
											 IndexDefinitionEntityKind::IDEK_NODE,
											 IndexDefinitionKind::IDK_VECTOR,
											 11U,
											 {"embedding"},
											 2U,
											 "l2"};
	const auto consume = [](std::span<const RelocatableVectorSemanticRecord>) {};
	const std::filesystem::path foreignPath = destinationPath.string() + ".foreign";
	const std::filesystem::path foreignWalPath = foreignPath.string() + ".foreign";
	{
		auto foreignDataFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		auto foreignWalFile = std::make_shared<std::fstream>(
				foreignWalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignDataFile->is_open());
		ASSERT_TRUE(foreignWalFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignDataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignWalIo = std::make_shared<StorageIO>(foreignWalFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		SlottedPagePool foreignPool(foreignIo, *foreignMetadata);
		EntityPageStore foreignEntities(foreignIo, *foreignMetadata);
		EntityLabelSetStore foreignLabels(foreignIo, *foreignMetadata, foreignPool);
		BTreePageStore foreignPages(foreignPool, *foreignMetadata);
		PropertyCollectionRootStore foreignRoots(foreignPool, *foreignMetadata);
		SystemCatalog foreignCatalog(foreignPages, *foreignMetadata);

		expectFormatError(
				[&] {
					static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&definition, 1U), foreignEntities,
																											  labels, roots, catalog, *metadata, scanOptions(), consume));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&definition, 1U), entities, foreignLabels,
																					 foreignRoots, catalog, *metadata, scanOptions(), consume));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&definition, 1U), entities, labels,
																																		 roots, foreignCatalog, *metadata, scanOptions(), consume));
				},
				FormatError::FERR_INVALID_STATE);
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}

TEST_F(RelocatableVectorSemanticScannerTest, TreatsAPropertyTokenAboveEveryNodeEntryAsAbsent) {
	auto metadata = RelocatableMetadataStore::initializeNew(destinationIo, initialSuperblock());
	SlottedPagePool pool(destinationIo, *metadata);
	EntityPageStore entities(destinationIo, *metadata);
	populateNative(entities, *metadata, pool);
	EntityLabelSetStore labels(destinationIo, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	const IndexDefinition definition{7U,
											 "member_other",
											 IndexDefinitionEntityKind::IDEK_NODE,
											 IndexDefinitionKind::IDK_VECTOR,
											 11U,
											 {"other"},
											 2U,
											 "l2"};
	expectFormatError(
			[&] {
				static_cast<void>(RelocatableVectorSemanticScanner::scan(std::span(&definition, 1U), entities, labels, roots, catalog,
															  *metadata, scanOptions(),
																					  [](std::span<const RelocatableVectorSemanticRecord>) {}));
			},
		FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorSemanticScannerTest, KeepsOwnerKeyedRootsScopedToTheirEntityKindAndId) {
	const auto verifyOwner = [this](uint64_t ownerId, uint32_t ownerType, const std::string &suffix) {
		const std::filesystem::path path = destinationPath.string() + suffix;
		const std::filesystem::path wal = path.string() + ".foreign";
		{
			auto dataFile = std::make_shared<std::fstream>(
					path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			auto walFile = std::make_shared<std::fstream>(
					wal, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(dataFile->is_open());
			ASSERT_TRUE(walFile->is_open());
			auto dataIo = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto metadata = RelocatableMetadataStore::initializeNew(dataIo, initialSuperblock());
			SlottedPagePool pool(dataIo, *metadata);
			EntityPageStore entities(dataIo, *metadata);
			EntityLabelSetStore labels(dataIo, *metadata, pool);
			NodeEntityRecord node;
			node.inline_label_ids[0] = 11U;
			node.inline_label_count = 1U;
			ASSERT_TRUE(entities.allocateNode(
					{.segment_id = SegmentId(100U),
					 .pool_owner_id = 11U,
					 .generation = Generation(1U),
					 .page_id = PageId(100U),
					 .first_slot = PhysicalSlotId(41U),
					 .last_slot = PhysicalSlotId(41U)},
					1U, node, LogSequenceNumber(2U))
					.has_value());
			if (ownerType == Node::typeId && ownerId != 1U) {
				NodeEntityRecord nonMatchingNode = node;
				nonMatchingNode.inline_label_ids[0] = 12U;
				ASSERT_TRUE(entities.allocateNode(
						{.segment_id = SegmentId(102U),
						 .pool_owner_id = 11U,
						 .generation = Generation(1U),
						 .page_id = PageId(102U),
						 .first_slot = PhysicalSlotId(43U),
						 .last_slot = PhysicalSlotId(43U)},
						ownerId, nonMatchingNode, LogSequenceNumber(3U))
						.has_value());
			}
			if (ownerType == Edge::typeId) {
				ASSERT_TRUE(entities.allocateEdge(
						{.segment_id = SegmentId(101U),
						 .pool_owner_id = 11U,
						 .generation = Generation(1U),
						 .page_id = PageId(101U),
						 .first_slot = PhysicalSlotId(42U),
						 .last_slot = PhysicalSlotId(42U)},
							ownerId, EdgeEntityRecord{1U, 1U, 41U}, LogSequenceNumber(3U))
						.has_value());
			}
			PropertyCollectionRootStore roots(pool, *metadata);
			PropertyCollectionRoot root;
			root.owner_entity_id = ownerId;
			root.owner_entity_type = ownerType;
			root.entries = {{1U, PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)})}};
			static_cast<void>(test::publishPropertyRoot(
					entities, roots, *metadata, root, {PhysicalSlotId(40U), PhysicalSlotId(40U), 91U}));
			BTreePageStore pages(pool, *metadata);
			SystemCatalog catalog(pages, *metadata);
			NativeGraphWritePlanner planner(*metadata, entities, labels, roots, pages, catalogPlannerOptions());
			publishPropertyTokens(catalog, planner, {"embedding"});
			const IndexDefinition definition{7U,
													 "member_embedding",
													 IndexDefinitionEntityKind::IDEK_NODE,
													 IndexDefinitionKind::IDK_VECTOR,
													 11U,
													 {"embedding"},
													 2U,
													 "l2"};
		const RelocatableVectorSemanticScanResult result = RelocatableVectorSemanticScanner::scan(
				std::span(&definition, 1U), entities, labels, roots, catalog, *metadata, scanOptions(),
				[](std::span<const RelocatableVectorSemanticRecord>) {});
		EXPECT_EQ(result.vector_count, 0U);
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(wal, error);
	};

	verifyOwner(1U, Edge::typeId, ".wrong-type");
	verifyOwner(2U, Node::typeId, ".wrong-id");
}
