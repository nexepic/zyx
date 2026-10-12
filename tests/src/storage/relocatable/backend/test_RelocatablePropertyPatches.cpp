#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, CreatedPropertyPatchesFoldIntoNativeCreationAndSurviveReopen) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const LogicalGraphMutationEntityReference node{
		LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference edge{
		LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U};
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"PatchCreated"}, {{"keep", PropertyValue("original")}, {"remove", PropertyValue(1)}}}};
	companion.edge_creates = {{node, node, "PATCH_LINK", {{"keep", PropertyValue(2)}, {"remove", PropertyValue(3)}}}};
	const auto original = companion;
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{node, {{"added", PropertyValue(4)}}, {"remove"}},
		{edge, {{"keep", PropertyValue(5)}}, {"remove"}}};
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->mutationFence().has_value());
	const auto result = backend.executeLogicalGraphMutationBatchWithPropertyPatches(
		*snapshot, companion, patches, 8U, *snapshot->mutationFence());
	EXPECT_EQ(companion, original);
	ASSERT_EQ(result.created_node_ids.size(), 1U);
	ASSERT_EQ(result.created_edge_ids.size(), 1U);
	snapshot.reset();
	backend.close();
	auto reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenedOptions));
	reopened.open();
	auto current = reopened.openLogicalGraphReadSnapshot();
	const auto createdNode = current->readNodeShaped(result.created_node_ids.front(), {.all_properties = true});
	ASSERT_EQ(createdNode.properties.size(), 2U);
	const auto added = std::ranges::find(createdNode.properties, "added", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(added, createdNode.properties.end());
	EXPECT_EQ(added->value, PropertyValue(4));
	const auto keep = std::ranges::find(createdNode.properties, "keep", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(keep, createdNode.properties.end());
	EXPECT_EQ(keep->value, PropertyValue("original"));
	const auto createdEdge = current->readEdgeShaped(result.created_edge_ids.front(), {.all_properties = true});
	ASSERT_EQ(createdEdge.properties.size(), 1U);
	EXPECT_EQ(createdEdge.properties.front().key, "keep");
	EXPECT_EQ(createdEdge.properties.front().value, PropertyValue(5));
	EXPECT_EQ(createdEdge.source_node_id, result.created_node_ids.front());
	EXPECT_EQ(createdEdge.target_node_id, result.created_node_ids.front());
	current.reset();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, PersistedPropertyPatchAdmissionFailureAllowsRetryAndPreservesOldLargeValueSnapshot) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const PropertyValue payload(std::string(16000U, 'p'));
	const auto ids = backend.createLogicalNodes({{{"PatchPersisted"}, {{"payload", payload}, {"rank", PropertyValue(1)}}}});
	ASSERT_EQ(ids.size(), 1U);
	auto before = backend.openLogicalGraphReadSnapshot();
	const auto fence = *before->mutationFence();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{
		{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids.front()},
		{{"rank", PropertyValue(2)}}, {}}};
	size_t admissions = 0U;
	LogicalGraphMutationReadControl control;
	control.property_admission = [&](size_t bytes) {
		++admissions;
		if (bytes > 1U) throw std::length_error("Patch source exceeds test budget");
	};
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 3U, fence, control)), std::length_error);
	EXPECT_GT(admissions, 0U);
	EXPECT_EQ(backend.openLogicalGraphReadSnapshot()->mutationFence(), before->mutationFence());
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 3U, fence)));
	const auto verify = [&](LogicalGraphReadSnapshot &snapshot, int rank) {
		const auto node = snapshot.readNodeShaped(ids.front(), {.all_properties = true});
		ASSERT_EQ(node.properties.size(), 2U);
		const auto field = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
		ASSERT_NE(field, node.properties.end());
		EXPECT_EQ(field->value, PropertyValue(rank));
		const auto value = std::ranges::find(node.properties, "payload", &LogicalGraphPropertyEntry::key);
		ASSERT_NE(value, node.properties.end());
		EXPECT_EQ(value->value, payload);
	};
	verify(*before, 1);
	auto after = backend.openLogicalGraphReadSnapshot();
	verify(*after, 2);
	after.reset();
	before.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, PropertyPatchUniqueFailureRollsBackAllTargetsAndCompanionLabels) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto ids = backend.createLogicalNodes({
		{{"PatchUnique"}, {{"key", PropertyValue(1)}}},
		{{"PatchUnique"}, {{"key", PropertyValue(2)}}}});
	ASSERT_EQ(ids.size(), 2U);
	static_cast<void>(backend.createLogicalConstraint({"patch_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
		LogicalGraphConstraintKind::LGCK_UNIQUE, "PatchUnique", {"key"}}));
	const auto reference = [&](size_t index) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids[index]};
	};
	LogicalGraphMutationBatch companion;
	companion.node_label_replacements = {{reference(0U), {"PatchUnique", "Touched"}}};
	std::vector<LogicalGraphPropertyPatchRequest> patches{
		{reference(0U), {{"key", PropertyValue(3)}}, {}},
		{reference(1U), {{"key", PropertyValue(3)}}, {}}};
	auto before = backend.openLogicalGraphReadSnapshot();
	const auto fence = *before->mutationFence();
	try {
		static_cast<void>(backend.executeLogicalGraphMutationBatchWithPropertyPatches(*before, companion, patches, 4U, fence));
		FAIL() << "Expected duplicate final constraint key to reject the batch";
	} catch (const std::runtime_error &error) {
		EXPECT_NE(std::string(error.what()).find("patch_unique"), std::string::npos);
	}
	auto unchanged = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(unchanged->mutationFence(), before->mutationFence());
	for (size_t index = 0U; index < ids.size(); ++index) {
		const auto node = unchanged->readNodeShaped(ids[index], {.include_labels = true, .all_properties = true});
		ASSERT_EQ(node.properties.size(), 1U);
		EXPECT_EQ(node.properties.front().value, PropertyValue(static_cast<int>(index + 1U)));
		EXPECT_EQ(node.labels, (std::vector<std::string>{"PatchUnique"}));
	}
	patches[1].sets.front().value = PropertyValue(4);
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatchWithPropertyPatches(
		*before, companion, patches, 4U, fence)));
	auto changed = backend.openLogicalGraphReadSnapshot();
	for (size_t index = 0U; index < ids.size(); ++index) {
		const auto node = changed->readNodeShaped(ids[index], {.include_labels = true, .all_properties = true});
		ASSERT_EQ(node.properties.size(), 1U);
		EXPECT_EQ(node.properties.front().value, PropertyValue(static_cast<int>(index + 3U)));
		EXPECT_EQ(std::ranges::find(node.labels, "Touched") != node.labels.end(), index == 0U);
	}
	changed.reset();
	unchanged.reset();
	before.reset();
	backend.close();
}
