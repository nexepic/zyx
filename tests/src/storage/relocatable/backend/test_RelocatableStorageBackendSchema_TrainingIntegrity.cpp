/**
 * @file test_RelocatableStorageBackendSchema_TrainingIntegrity.cpp
 * @brief Fail-closed persisted semantic-state contracts for Vector training.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

TEST_F(RelocatableStorageBackendSchemaTest, VectorTrainingRejectsCorruptSemanticMappingsWithoutPublishingDerivedState) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	const auto createUntrainedIndex = [&] {
		RelocatableStorageBackend writer(options());
		writer.open();
		static_cast<void>(writer.createLogicalNodes(
				{{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
		const LogicalGraphIndexDefinition index =
				writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
		writer.close();
		return index;
	};
	const auto assertRejectedWithoutDerivedPublication = [&](const LogicalGraphIndexDefinition &index) {
		const VectorManifest before = readVectorManifest(index.index_id);
		RelocatableStorageBackend corrupted(options());
		corrupted.open();
		EXPECT_THROW(static_cast<void>(corrupted.trainLogicalVector({index.name, 16U, 16U})), FormatException);
		corrupted.close();
		EXPECT_EQ(readVectorManifest(index.index_id), before);
	};
	const auto resetDatabase = [&] {
		std::error_code error;
		std::filesystem::remove(data_path_, error);
		ASSERT_FALSE(error);
		initializeBlankDatabase();
	};

	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(1U);
		// A zero Vector ID cannot be resolved through the reverse mapping or page
		// directory, so the trainer must reject it before derived publication.
		overwriteBTreeLeafEntry(index.index_id, key, {key, 0U});
		assertRejectedWithoutDerivedPublication(index);
	}

	resetDatabase();
	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const VectorManifest original = readVectorManifest(index.index_id);
		// The page codec permits a complete codebook identity without a PQ
		// mapping identity; training must reject this partial derived state.
		VectorManifest partial = original;
		partial.codebook_id = 17U;
		partial.codebook_generation = Generation(1U);
		overwriteVectorManifest(partial);
		RelocatableStorageBackend backend(options());
		backend.open();
		EXPECT_THROW(static_cast<void>(backend.trainLogicalVector({index.name, 16U, 16U})), FormatException);
		backend.close();
		overwriteVectorManifest(original);
	}

	resetDatabase();
	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const VectorManifest original = readVectorManifest(index.index_id);
		// Exercise the mapping-side short circuit independently while retaining
		// its paired root fields so the durable manifest remains syntactically valid.
		VectorManifest partial = original;
		partial.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD1);
		partial.pq_mapping_root_page_id = PageId(1U);
		partial.pq_mapping_root_generation = Generation(1U);
		overwriteVectorManifest(partial);
		RelocatableStorageBackend backend(options());
		backend.open();
		EXPECT_THROW(static_cast<void>(backend.trainLogicalVector({index.name, 16U, 16U})), FormatException);
		backend.close();
		overwriteVectorManifest(original);
	}

	resetDatabase();
	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const VectorManifest manifest = readVectorManifest(index.index_id);
		const std::vector<std::byte> key = VectorSemanticCodec::vectorMappingKey(1U);
		// Keep the canonical key but break the inverse Node binding. This models a
		// committed metadata corruption that a normal graph writer cannot create.
		overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, key, {key, 2U});
		assertRejectedWithoutDerivedPublication(index);
	}

	resetDatabase();
	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const std::vector<std::byte> key = VectorSemanticCodec::nodeMappingKey(1U);
		const std::vector<std::byte> malformedKey{std::byte{0U}};
		overwriteBTreeLeafEntry(index.index_id, key, {malformedKey, 1U});
		assertRejectedWithoutDerivedPublication(index);
	}

	resetDatabase();
	{
		const LogicalGraphIndexDefinition index = createUntrainedIndex();
		const VectorManifest manifest = readVectorManifest(index.index_id);
		// An extra inverse entry makes both individual mappings well-formed but
		// violates the durable forward/reverse cardinality contract.
		const std::vector<std::byte> extraKey = VectorSemanticCodec::vectorMappingKey(2U);
		appendBTreeLeafEntry(manifest.vector_node_mapping_index_id, {extraKey, 2U});
		assertRejectedWithoutDerivedPublication(index);
	}
}

TEST_F(RelocatableStorageBackendSchemaTest, VectorTrainingRejectsStaleRawRecordWithoutPublishingDerivedState) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes(
			{{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const VectorManifest before = readVectorManifest(index.index_id);
	std::vector<std::byte> staleRecord = readVectorRecord(1U);
	LittleEndianCodec::writeU64(staleRecord, 0U, 2U);
	overwriteVectorRecord(1U, staleRecord);

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	EXPECT_THROW(static_cast<void>(corrupted.trainLogicalVector({index.name, 16U, 16U})), FormatException);
	corrupted.close();
	EXPECT_EQ(readVectorManifest(index.index_id), before);
}

TEST_F(RelocatableStorageBackendSchemaTest, BackendOpenRejectsAnExhaustedCheckpointBeforeVectorTraining) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes(
			{{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const VectorManifest before = readVectorManifest(index.index_id);
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		SuperblockFile superblocks(io);
		Superblock exhausted = superblocks.recover();
		ASSERT_NE(exhausted.generation.value(), std::numeric_limits<uint64_t>::max());
		exhausted.generation = Generation(exhausted.generation.value() + 1U);
		exhausted.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max());
		superblocks.publish(exhausted);
		io.reset();
		data->close();
	}

	RelocatableStorageBackend exhaustedBackend(options());
	EXPECT_THROW(exhaustedBackend.open(), FormatException);
	EXPECT_EQ(readVectorManifest(index.index_id), before);
}

TEST_F(RelocatableStorageBackendSchemaTest, VectorRetrainingRejectsStaleDerivedIdentitiesBeforePublication) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	const auto createTrainedIndex = [&]() -> std::pair<LogicalGraphIndexDefinition, std::vector<uint64_t>> {
		RelocatableStorageBackend writer(options());
		writer.open();
		const std::vector<uint64_t> nodes = writer.createLogicalNodes(
				{{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}},
				 { {"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
		const LogicalGraphIndexDefinition index =
				writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
		EXPECT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
		writer.close();
		return std::pair{index, nodes};
	};
	const auto assertRetrainingRejectedWithoutReplacement = [&](const LogicalGraphIndexDefinition &index,
																		 const VectorManifest &corrupt) {
		overwriteVectorManifest(corrupt);
		RelocatableStorageBackend backend(options());
		backend.open();
		EXPECT_THROW(static_cast<void>(backend.trainLogicalVector({index.name, 16U, 16U})), FormatException);
		backend.close();
		EXPECT_EQ(readVectorManifest(index.index_id), corrupt);
	};
	const auto resetDatabase = [&] {
		std::error_code error;
		std::filesystem::remove(data_path_, error);
		ASSERT_FALSE(error);
		initializeBlankDatabase();
	};

	{
		const auto [index, ignoredNodes] = createTrainedIndex();
		static_cast<void>(ignoredNodes);
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.pq_mapping_root_generation.value(), std::numeric_limits<uint64_t>::max());
		corrupt.pq_mapping_root_generation = Generation(corrupt.pq_mapping_root_generation.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, ignoredNodes] = createTrainedIndex();
		static_cast<void>(ignoredNodes);
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.pq_mapping_root_page_id.value(), std::numeric_limits<uint64_t>::max());
		corrupt.pq_mapping_root_page_id = PageId(corrupt.pq_mapping_root_page_id.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, ignoredNodes] = createTrainedIndex();
		static_cast<void>(ignoredNodes);
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.node_mapping_root_generation.value(), std::numeric_limits<uint64_t>::max());
		corrupt.node_mapping_root_generation = Generation(corrupt.node_mapping_root_generation.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, ignoredNodes] = createTrainedIndex();
		static_cast<void>(ignoredNodes);
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.node_mapping_root_page_id.value(), std::numeric_limits<uint64_t>::max());
		ASSERT_NE(corrupt.vector_node_mapping_root_page_id.value(), std::numeric_limits<uint64_t>::max());
		// The page codec accepts a syntactically valid identity; training must
		// still bind both semantic trees to their current metadata roots.
		corrupt.node_mapping_root_page_id = PageId(corrupt.node_mapping_root_page_id.value() + 1U);
		corrupt.vector_node_mapping_root_page_id = PageId(corrupt.vector_node_mapping_root_page_id.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, nodes] = createTrainedIndex();
		RelocatableStorageBackend writer(options());
		writer.open();
		writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(0.5, 0.5)}}}});
		writer.close();
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.delta_mapping_index_id, 0U);
		ASSERT_NE(corrupt.delta_mapping_root_page_id.value(), std::numeric_limits<uint64_t>::max());
		corrupt.delta_mapping_root_page_id = PageId(corrupt.delta_mapping_root_page_id.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, nodes] = createTrainedIndex();
		RelocatableStorageBackend writer(options());
		writer.open();
		writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(0.5, 0.5)}}}});
		writer.close();
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.delta_mapping_index_id, 0U);
		ASSERT_NE(corrupt.adjacency_generation.value(), 0U);
		corrupt.adjacency_generation = Generation(std::numeric_limits<uint64_t>::max());
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}

	resetDatabase();
	{
		const auto [index, nodes] = createTrainedIndex();
		RelocatableStorageBackend writer(options());
		writer.open();
		writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(0.5, 0.5)}}}});
		writer.close();
		VectorManifest corrupt = readVectorManifest(index.index_id);
		ASSERT_NE(corrupt.delta_mapping_index_id, 0U);
		ASSERT_NE(corrupt.delta_mapping_root_generation.value(), std::numeric_limits<uint64_t>::max());
		corrupt.delta_mapping_root_generation = Generation(corrupt.delta_mapping_root_generation.value() + 1U);
		assertRetrainingRejectedWithoutReplacement(index, corrupt);
	}
}

TEST_F(RelocatableStorageBackendSchemaTest, ConstraintCreationRejectsUnknownPublicEnumValuesBeforeSchemaPublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintCreateRequest valid{"person_constraint",
													 LogicalGraphConstraintEntityKind::LGCEK_NODE,
													 LogicalGraphConstraintKind::LGCK_NOT_NULL,
													 "Person",
													 {"name"},
													 LogicalGraphConstraintPropertyType::LGCPT_NONE};
	const auto rejectsWithoutPublication = [&](LogicalGraphConstraintCreateRequest request) {
		EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint(request)), std::invalid_argument);
		EXPECT_TRUE(backend.listLogicalConstraints(8U).empty());
	};

	LogicalGraphConstraintCreateRequest invalidEntity = valid;
	invalidEntity.entity_kind = static_cast<LogicalGraphConstraintEntityKind>(99U);
	rejectsWithoutPublication(std::move(invalidEntity));
	LogicalGraphConstraintCreateRequest invalidKind = valid;
	invalidKind.kind = static_cast<LogicalGraphConstraintKind>(99U);
	rejectsWithoutPublication(std::move(invalidKind));
	LogicalGraphConstraintCreateRequest invalidPropertyType = valid;
	invalidPropertyType.property_type = static_cast<LogicalGraphConstraintPropertyType>(99U);
	rejectsWithoutPublication(std::move(invalidPropertyType));

	backend.close();
}
