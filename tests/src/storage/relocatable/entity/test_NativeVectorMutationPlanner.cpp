/**
 * @file test_NativeVectorMutationPlanner.cpp
 * @brief Semantic Vector delta planning tests for native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

using namespace graph::storage;
using graph::Node;
using graph::PropertyValue;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7A7B7C7D7E7F8091ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	IndexDefinition vectorDefinition() {
		IndexDefinition result;
		result.index_id = 71U;
		result.name = "embedding-index";
		result.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		result.kind = IndexDefinitionKind::IDK_VECTOR;
		result.label_token_id = 7U;
		result.property_names = {"embedding"};
		result.vector_dimensions = 2U;
		result.vector_metric = "l2";
		return result;
	}

	IndexDefinition propertyDefinition() {
		return {72U,
				"score-index",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_PROPERTY,
				7U,
				{"score"},
				0U,
				""};
	}

	PropertyCollectionRoot root(uint64_t nodeId, int64_t first, int64_t second) {
		return {nodeId, Node::typeId,
				{{17U, PropertyValue(std::vector<PropertyValue>{PropertyValue(first), PropertyValue(second)})}}};
	}

	NativeVectorMutationPlanner plannerFor(const IndexDefinition &definition) {
		return NativeVectorMutationPlanner(
				{definition},
				[](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
					return name == "embedding" ? std::optional<uint32_t>(17U) : std::nullopt;
				},
				{8U, 4U});
	}

	NativeVectorMutationPlanner planner() { return plannerFor(vectorDefinition()); }

} // namespace

class NativeVectorMutationPlannerTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("native_vector_delta_planner_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
										  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	}

	void TearDown() override {
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(NativeVectorMutationPlannerTest, EmitsOrderedEraseThenInsertForChangedIndexedValue) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::array<uint64_t, 1U> labels = {7U};
	const PropertyCollectionRoot oldRoot = root(11U, 1, 2);
	const PropertyCollectionRoot newRoot = root(11U, 1, 3);

	const std::vector<NativeVectorMutation> mutations =
			planner().planNode(snapshot, 11U, labels, &oldRoot, labels, &newRoot);

	ASSERT_EQ(mutations.size(), 2U);
	EXPECT_EQ(mutations[0].kind, NativeVectorMutationKind::NVMK_ERASE);
	EXPECT_EQ(mutations[1].kind, NativeVectorMutationKind::NVMK_INSERT);
	for (const NativeVectorMutation &mutation: mutations) {
		EXPECT_EQ(mutation.index_id, 71U);
		EXPECT_EQ(mutation.node_id, 11U);
		EXPECT_EQ(mutation.dimensions, 2U);
		EXPECT_EQ(mutation.metric, VectorDistanceMetric::VDM_L2);
	}
	EXPECT_TRUE(mutations[0].encoded_bfloat16.empty());
	EXPECT_EQ(mutations[1].encoded_bfloat16.size(), 2U * sizeof(uint16_t));
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, IgnoresEquivalentBfloat16AndUnrelatedLabelTransitions) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::array<uint64_t, 1U> indexedLabels = {7U};
	const std::array<uint64_t, 1U> otherLabels = {9U};
	const PropertyCollectionRoot value = root(11U, 1, 2);

	EXPECT_TRUE(planner().planNode(snapshot, 11U, indexedLabels, &value, indexedLabels, &value).empty());
	EXPECT_TRUE(planner().planNode(snapshot, 11U, otherLabels, &value, otherLabels, &value).empty());
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, EmitsMembershipInsertAndEraseWithoutReadingIrrelevantProperties) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::array<uint64_t, 1U> indexedLabels = {7U};
	const std::array<uint64_t, 1U> otherLabels = {9U};
	const PropertyCollectionRoot value = root(11U, 1, 2);

	const std::vector<NativeVectorMutation> inserted =
			planner().planNode(snapshot, 11U, otherLabels, &value, indexedLabels, &value);
	ASSERT_EQ(inserted.size(), 1U);
	EXPECT_EQ(inserted.front().kind, NativeVectorMutationKind::NVMK_INSERT);

	const std::vector<NativeVectorMutation> erased =
			planner().planNode(snapshot, 11U, indexedLabels, &value, otherLabels, &value);
	ASSERT_EQ(erased.size(), 1U);
	EXPECT_EQ(erased.front().kind, NativeVectorMutationKind::NVMK_ERASE);
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, RejectsMissingIndexedPropertyTokenBeforeInferringMembership) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeVectorMutationPlanner missingToken(
			{vectorDefinition()},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; },
			{8U, 4U});
	const std::array<uint64_t, 1U> labels = {7U};
	const PropertyCollectionRoot value = root(11U, 1, 2);

	EXPECT_THROW(static_cast<void>(missingToken.planNode(snapshot, 11U, labels, &value, labels, &value)),
			 FormatException);
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, RejectsZeroNormCosineValuesAtBothSemanticBoundaries) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	IndexDefinition cosine = vectorDefinition();
	cosine.vector_metric = "cosine";
	const std::array<uint64_t, 1U> labels = {7U};
	const PropertyCollectionRoot zero = root(11U, 0, 0);
	const PropertyCollectionRoot nonzero = root(11U, 1, 0);

	EXPECT_THROW(static_cast<void>(plannerFor(cosine).planNode(snapshot, 11U, labels, nullptr, labels, &zero)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(plannerFor(cosine).planNode(snapshot, 11U, labels, &zero, labels, &nonzero)),
				 FormatException);
	snapshot.release();
}

TEST(NativeVectorMutationPlannerContractsTest, RejectsInvalidConstructorDefinitionsAndBounds) {
	const NativeVectorPropertyKeyTokenResolver resolver =
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; };
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({vectorDefinition()}, {}, {8U, 4U})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({vectorDefinition()}, resolver, {8U, 0U})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({vectorDefinition()}, resolver, {0U, 4U})),
			FormatException);
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner(
				{vectorDefinition(), vectorDefinition()}, resolver, {4U, 4U})), FormatException);
	IndexDefinition zeroId = vectorDefinition();
	zeroId.index_id = 0U;
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({zeroId}, resolver, {4U, 4U})), FormatException);
	EXPECT_NO_THROW(static_cast<void>(NativeVectorMutationPlanner(
				{propertyDefinition(), vectorDefinition()}, resolver, {4U, 4U})));

	IndexDefinition missingLabel = vectorDefinition();
	missingLabel.label_token_id = 0U;
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({missingLabel}, resolver, {4U, 4U})), FormatException);
	IndexDefinition twoProperties = vectorDefinition();
	twoProperties.property_names.push_back("other");
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({twoProperties}, resolver, {4U, 4U})), FormatException);
	IndexDefinition noDimensions = vectorDefinition();
	noDimensions.vector_dimensions = 0U;
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({noDimensions}, resolver, {4U, 4U})), FormatException);
	IndexDefinition excessiveDimensions = vectorDefinition();
	excessiveDimensions.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS + 1U;
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner({excessiveDimensions}, resolver, {4U, 4U})),
			FormatException);
	EXPECT_THROW(static_cast<void>(NativeVectorMutationPlanner(
				{vectorDefinition(), propertyDefinition()}, resolver, {1U, 4U})), FormatException);
}

TEST_F(NativeVectorMutationPlannerTest, RejectsMalformedInputsAndResolverTokensBeforeVectorPlanning) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::array<uint64_t, 1U> labels = {7U};
	const PropertyCollectionRoot value = root(11U, 1, 2);
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 0U, labels, &value, labels, &value)), FormatException);
	const std::array<uint64_t, 1U> zeroLabel = {0U};
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, zeroLabel, &value, labels, &value)), FormatException);
	const std::array<uint64_t, 2U> unorderedLabels = {9U, 7U};
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, unorderedLabels, &value, labels, &value)), FormatException);
	const std::array<uint64_t, 2U> duplicateLabels = {7U, 7U};
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, labels, &value, duplicateLabels, &value)), FormatException);

	PropertyCollectionRoot wrongOwner = value;
	wrongOwner.owner_entity_id = 12U;
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, labels, &wrongOwner, labels, &value)), FormatException);
	PropertyCollectionRoot wrongType = value;
	wrongType.owner_entity_type = Node::typeId + 1U;
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, labels, &wrongType, labels, &value)), FormatException);
	PropertyCollectionRoot nonCanonical = value;
	nonCanonical.entries.push_back({17U, PropertyValue(int64_t(3))});
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, labels, &nonCanonical, labels, &value)), FormatException);
	PropertyCollectionRoot zeroKeyToken = value;
	zeroKeyToken.entries.front().key_token = 0U;
	EXPECT_THROW(static_cast<void>(planner().planNode(snapshot, 11U, labels, &zeroKeyToken, labels, &value)), FormatException);
	NativeVectorMutationPlanner zeroToken(
			{vectorDefinition()}, [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 0U; },
			{4U, 4U});
	EXPECT_THROW(static_cast<void>(zeroToken.planNode(snapshot, 11U, labels, &value, labels, &value)), FormatException);
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, EnforcesPerNodeMutationBudgetAcrossVectorIndexes) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	IndexDefinition second = vectorDefinition();
	second.index_id = 73U;
	second.name = "embedding-index-second";
	NativeVectorMutationPlanner bounded(
			{vectorDefinition(), second},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; }, {4U, 1U});
	const std::array<uint64_t, 1U> labels = {7U};
	const PropertyCollectionRoot value = root(11U, 1, 2);
	EXPECT_THROW(static_cast<void>(bounded.planNode(snapshot, 11U, {}, nullptr, labels, &value)), FormatException);
	snapshot.release();
}

TEST_F(NativeVectorMutationPlannerTest, SkipsAnIndexedLabelWhosePropertyIsAbsentFromAnOtherwiseCanonicalRoot) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::array<uint64_t, 1U> labels = {7U};
	PropertyCollectionRoot otherProperty = root(11U, 1, 2);
	otherProperty.entries.front().key_token = 18U;
	otherProperty.entries.push_back({19U, PropertyValue(int64_t(3))});
	EXPECT_TRUE(planner().planNode(snapshot, 11U, labels, &otherProperty, labels, &otherProperty).empty());
	snapshot.release();
}
