#include <gtest/gtest.h>

#include "storage/LogicalGraphTransactionOverlayTestFixture.hpp"

namespace graph::storage::transaction_overlay_test {
namespace {
LogicalGraphMutationEntityReference nodeReference(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}

TEST(LogicalGraphPropertyPatchTest, PreservesOtherFieldsAndReadsPreviousStagedValues) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	{
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
		EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())));
	}
	{
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(9)}}, {"name", "missing"}}};
		EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())));
	}
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto node = snapshot->readNodeShaped(1U, {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 1U);
	EXPECT_EQ(node.properties.front().key, "rank");
	EXPECT_EQ(node.properties.front().value, PropertyValue(9));
	EXPECT_EQ(transaction.readSnapshot().readNodeShaped(1U, {.all_properties = true}).properties.front().value, PropertyValue("Ada"));
}

TEST(LogicalGraphPropertyPatchTest, NormalizationSeparatesChangedFieldsWithoutStagingOrRetainingEqualPayloads) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto fence = *snapshot->mutationFence();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"name", PropertyValue("Ada")}, {"rank", PropertyValue(7)}}, {"missing"}}};
	const auto normalized = normalizeLogicalGraphPropertyPatchBatch(*snapshot, {}, patches, 4U, fence);
	ASSERT_EQ(normalized.persisted_patches.size(), 1U);
	EXPECT_EQ(normalized.persisted_patches.front().sets,
		(std::vector<LogicalGraphWriteProperty>{{"rank", PropertyValue(7)}}));
	EXPECT_TRUE(normalized.persisted_patches.front().removals.empty());
	ASSERT_EQ(normalized.batch.property_replacements.size(), 1U);
	EXPECT_EQ(normalized.batch.property_replacements.front().properties.size(), 2U);
	EXPECT_EQ(normalized.entry_work, 4U);
	EXPECT_EQ(snapshot->mutationFence(), fence);
	EXPECT_EQ(snapshot->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
}

TEST(LogicalGraphPropertyPatchTest, LegacyTransactionsRejectRawStagingExplicitlyAndOverlayUsesFullStateFallback) {
	InMemoryLogicalTransaction transaction;
	EXPECT_FALSE(transaction.supportsPropertyPatchStaging());
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_THROW(static_cast<void>(transaction.stageMutationBatchWithPropertyPatches({}, patches, 2U)), std::logic_error);
	EXPECT_EQ(transaction.state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 2U, *snapshot->mutationFence())));
	EXPECT_EQ(snapshot->readNodeShaped(1U, {.all_properties = true}).properties.size(), 2U);
}

TEST(LogicalGraphPropertyPatchTest, UnchangedValuesAndMissingRemovalsDoNotStageAReplacement) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->mutationFence().has_value());
	const uint64_t initialRevision = snapshot->mutationFence()->logical_revision;
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"name", PropertyValue("Ada")}}, {"missing"}}};
	EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(
		*snapshot, patches, 64U, *snapshot->mutationFence())));
	auto after = overlay.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(after->mutationFence().has_value());
	EXPECT_EQ(after->mutationFence()->logical_revision, initialRevision);
	EXPECT_EQ(after->readNodeShaped(1U, {.all_properties = true}).properties,
		(std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
}

TEST(LogicalGraphPropertyPatchTest, InvalidFinalRequestRejectsTheWholePatchList) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}},
		{nodeReference(2U), {{"rank", PropertyValue(8)}}, {"rank"}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
	patches[1].removals.clear();
	patches[1].sets.front().value = PropertyValue{};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
	patches[1] = patches[0];
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
}

TEST(LogicalGraphPropertyPatchTest, StaleSnapshotAndEntryBoundsDoNotPublishPartialChanges) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto stale = overlay.openLogicalGraphReadSnapshot();
	const auto planningFence = *stale->mutationFence();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*stale, patches, 1U, planningFence)), std::runtime_error);
	EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*stale, patches, 64U, planningFence)));
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*stale, patches, 64U, planningFence)), LogicalGraphMutationConflict);
	auto current = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNodeShaped(1U, {.all_properties = true}).properties.size(), 2U);
}

TEST(LogicalGraphPropertyPatchTest, CancellationAndReadOnlyTransactionsRejectBeforeStaging) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	LogicalGraphMutationReadControl control;
	control.checkpoint = [] { throw std::runtime_error("cancelled"); };
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence(), control)), std::runtime_error);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
	InMemoryLogicalTransaction readOnly(true);
	LogicalGraphTransactionOverlay readOnlyOverlay(readOnly);
	auto readOnlySnapshot = readOnlyOverlay.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(readOnlyOverlay.executeLogicalGraphPropertyPatches(*readOnlySnapshot, patches, 64U, *readOnlySnapshot->mutationFence())), std::logic_error);
}
TEST(LogicalGraphPropertyPatchTest, AppliesNodeAndEdgePatchesInOneStageAndRejectsMissingTargets) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}},
		{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 999U}, {{"weight", PropertyValue(2)}}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())), std::out_of_range);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
	patches[1].entity.value = 10U;
	EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence())));
	auto current = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNodeShaped(1U, {.all_properties = true}).properties.size(), 2U);
	EXPECT_EQ(current->readEdgeShaped(10U, {.all_properties = true}).properties.front().value, PropertyValue(2));
}

TEST(LogicalGraphPropertyPatchTest, SourceAdmissionFailureCannotStageTheFirstPatch) {
	InMemoryLogicalTransaction transaction;
	auto &base = dynamic_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	LogicalGraphMutationReadControl control;
	size_t admitted = 0U;
	control.property_admission = [&](size_t bytes) {
		admitted = bytes;
		throw std::runtime_error("source byte bound");
	};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence(), control)), std::logic_error);
	EXPECT_EQ(admitted, 0U);
	base.shaped_property_admission = true;
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence(), control)), std::runtime_error);
	EXPECT_GT(admitted, 0U);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
}

TEST(LogicalGraphPropertyPatchTest, MergesUnsortedSetsAndRemovalsIntoCanonicalFieldOrder) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	{
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::vector<LogicalGraphPropertyPatchRequest> patches{
			{nodeReference(1U), {{"z", PropertyValue(1)}, {"b", PropertyValue(2)}, {"a", PropertyValue(3)}}, {}}};
		static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence()));
	}
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"z", PropertyValue(7)}, {"c", PropertyValue(8)}}, {"name", "missing", "a"}}};
	static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 64U, *snapshot->mutationFence()));
	const auto node = overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 3U);
	EXPECT_EQ(node.properties[0].key, "b");
	EXPECT_EQ(node.properties[0].value, PropertyValue(2));
	EXPECT_EQ(node.properties[1].key, "c");
	EXPECT_EQ(node.properties[1].value, PropertyValue(8));
	EXPECT_EQ(node.properties[2].key, "z");
	EXPECT_EQ(node.properties[2].value, PropertyValue(7));
}

TEST(LogicalGraphPropertyPatchTest, RemovesTheWholeCollectionThenAddsToAnEmptyOwnerAtExactBounds) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	{
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {}, {"name"}}};
		static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 2U, *snapshot->mutationFence()));
	}
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	EXPECT_TRUE(snapshot->readNodeShaped(1U, {.all_properties = true}).properties.empty());
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 1U, *snapshot->mutationFence()));
	const auto node = overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 1U);
	EXPECT_EQ(node.properties.front().key, "rank");
	EXPECT_EQ(node.properties.front().value, PropertyValue(7));
}

TEST(LogicalGraphPropertyPatchTest, CreatedOrdinalsAndPersistedIdsRemainSeparateInOneAtomicBatch) {
	InMemoryLogicalTransaction transaction;
	transaction.setRejectCreatedPropertyOverrides(true);
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const LogicalGraphMutationEntityReference first{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference second{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	const LogicalGraphMutationEntityReference edge{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U};
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"Created"}, {{"name", PropertyValue("Grace")}}}, {{"Target"}, {}}};
	companion.edge_creates = {{first, second, "LINK", {{"old", PropertyValue(1)}}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}},
		{first, {{"rank", PropertyValue(6)}}, {}},
		{second, {{"rank", PropertyValue(8)}}, {}},
		{edge, {{"weight", PropertyValue(9)}}, {"old"}}};
	const auto result = overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 64U, *snapshot->mutationFence());
	ASSERT_EQ(result.created_node_ids.size(), 2U);
	ASSERT_EQ(result.created_edge_ids.size(), 1U);
	auto current = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNodeShaped(1U, {.all_properties = true}).properties.size(), 2U);
	EXPECT_EQ(current->readNodeShaped(result.created_node_ids[0], {.all_properties = true}).properties.front().value, PropertyValue("Grace"));
	EXPECT_EQ(current->readNodeShaped(result.created_node_ids[1], {.all_properties = true}).properties.front().value, PropertyValue(8));
	const auto relationship = current->readEdgeShaped(result.created_edge_ids[0], {.all_properties = true});
	ASSERT_EQ(relationship.properties.size(), 1U);
	EXPECT_EQ(relationship.properties.front().key, "weight");
	EXPECT_EQ(relationship.properties.front().value, PropertyValue(9));
}

TEST(LogicalGraphPropertyPatchTest, InvalidCreatedOrdinalRejectsPersistedPatchesBeforePublication) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto originalFence = snapshot->mutationFence();
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"Created"}, {}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}},
		{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U}, {}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeShaped(1U, {.all_properties = true}).properties.size(), 1U);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->mutationFence(), originalFence);
}

TEST(LogicalGraphPropertyPatchTest, CreatedPatchRejectsAnOverlappingCompleteReplacement) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto originalFence = snapshot->mutationFence();
	const LogicalGraphMutationEntityReference created{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"Created"}, {{"name", PropertyValue("Grace")}}}};
	companion.property_replacements = {{created, {{"name", PropertyValue("Other")}}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{created, {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->mutationFence(), originalFence);
}

TEST(LogicalGraphPropertyPatchTest, CreatedSourceEntryLimitRejectsBeforeStagingAndAllowsExactBoundary) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto originalFence = snapshot->mutationFence();
	const LogicalGraphMutationEntityReference created{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"Created"}, {{"name", PropertyValue("Grace")}, {"rank", PropertyValue(1)}}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{created, {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 2U, *snapshot->mutationFence())), std::runtime_error);
	EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->mutationFence(), originalFence);
	const auto result = overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 3U, *snapshot->mutationFence());
	ASSERT_EQ(result.created_node_ids.size(), 1U);
	const auto node = overlay.openLogicalGraphReadSnapshot()->readNodeShaped(result.created_node_ids.front(), {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 2U);
	EXPECT_EQ(node.properties.back().value, PropertyValue(7));
}

TEST(LogicalGraphPropertyPatchTest, CreatedReferenceKindsAndDuplicateTargetsRejectTheWholeBatch) {
	for (const auto invalid : {
		LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
		LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U}}) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const auto originalFence = snapshot->mutationFence();
		LogicalGraphMutationBatch companion;
		companion.node_creates = {{{"Created"}, {}}};
		std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}, {invalid, {}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
		EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->mutationFence(), originalFence);
		const LogicalGraphMutationEntityReference created{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
		patches = {{created, {{"rank", PropertyValue(1)}}, {}}, {created, {{"rank", PropertyValue(2)}}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(*snapshot, companion, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
		EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->mutationFence(), originalFence);
	}
}

TEST(LogicalGraphPropertyPatchTest, PublishesCompanionLabelsAndPatchesTogether) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	LogicalGraphMutationBatch companion;
	companion.node_label_replacements = {{nodeReference(1U), {"Updated"}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(
		*snapshot, companion, patches, 64U, *snapshot->mutationFence())));
	auto current = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNode(1U).labels, (std::vector<std::string>{"Updated"}));
	EXPECT_EQ(current->readNodeShaped(1U, {.all_properties = true}).properties.size(), 2U);
}

TEST(LogicalGraphPropertyPatchTest, OverlappingReplacementRejectsCompanionLabelsBeforePublication) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto originalLabels = snapshot->readNode(1U).labels;
	LogicalGraphMutationBatch companion;
	companion.node_label_replacements = {{nodeReference(1U), {"Updated"}}};
	companion.property_replacements = {{nodeReference(1U), {{"name", PropertyValue("Grace")}}}};
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{nodeReference(1U), {{"rank", PropertyValue(7)}}, {}}};
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(
		*snapshot, companion, patches, 64U, *snapshot->mutationFence())), std::invalid_argument);
	auto current = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNode(1U).labels, originalLabels);
	EXPECT_EQ(current->readNodeShaped(1U, {.all_properties = true}).properties.front().value, PropertyValue("Ada"));
}

TEST(LogicalGraphPropertyPatchTest, DeletedTargetsRejectAllPatchesBeforeSourceAdmission) {
	for (bool edge : {false, true}) {
		SCOPED_TRACE(edge);
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const auto fence = *snapshot->mutationFence();
		LogicalGraphMutationBatch companion;
		companion.node_label_replacements = {{nodeReference(1U), {"Changed"}}};
		if (edge) companion.edge_deletes = {{10U}};
		else companion.node_deletes = {{1U, true, 64U}};
		const LogicalGraphMutationEntityReference target{
			edge ? LogicalGraphMutationEntityKind::LGMK_EDGE : LogicalGraphMutationEntityKind::LGMK_NODE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge ? 10U : 1U};
		const std::vector<LogicalGraphPropertyPatchRequest> patches{{target, {{"rank", PropertyValue(7)}}, {}}};
		size_t admissions = 0U;
		LogicalGraphMutationReadControl control;
		control.property_admission = [&](size_t) { ++admissions; throw std::logic_error("Unexpected source admission"); };
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(
			*snapshot, companion, patches, 64U, fence, control)), std::invalid_argument);
		EXPECT_EQ(admissions, 0U);
		auto current = overlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(current->mutationFence(), snapshot->mutationFence());
		EXPECT_EQ(current->readNode(1U).labels, (std::vector<std::string>{"Base"}));
		EXPECT_EQ(current->readEdge(10U).edge_id, 10U);
	}
}

} // namespace
} // namespace graph::storage::transaction_overlay_test
