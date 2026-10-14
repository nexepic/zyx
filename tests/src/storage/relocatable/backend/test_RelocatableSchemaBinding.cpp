#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "query/execution/BoundPropertyBindings.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, SchemaBindingIdentityDistinguishesIndependentDatabaseFiles) {
	materializeNativeDatabase();
	const std::filesystem::path otherPath = data_path_.string() + ".binding-other";
	struct Cleanup {
		const std::filesystem::path &path;
		~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
	} cleanup{otherPath};
	{
		zyx::Database created(otherPath.string());
		created.open();
		created.close();
	}
	RelocatableStorageBackend backend(backendOptions());
	auto otherOptions = backendOptions();
	otherOptions.data_path = otherPath;
	RelocatableStorageBackend other(std::move(otherOptions));
	backend.open();
	other.open();
	auto first = backend.openLogicalGraphReadSnapshot();
	auto second = other.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(first->bindingSchemaIdentity());
	ASSERT_TRUE(second->bindingSchemaIdentity());
	EXPECT_NE(first->bindingSchemaIdentity()->database_token, second->bindingSchemaIdentity()->database_token);
	EXPECT_NE(first->bindingSchemaIdentity(), second->bindingSchemaIdentity());
	first.reset();
	second.reset();
	other.close();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, SchemaBindingObservationUsesCatalogAndPageDirectoryVersions) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto node = backend.createLogicalNodes({{{"SchemaBinding"}, {{"rank", PropertyValue(1)}}}}).front();
	auto pinned = backend.openLogicalGraphReadSnapshot();
	const auto identity = pinned->bindingSchemaIdentity();
	ASSERT_TRUE(identity);
	ASSERT_TRUE(identity->isValid());
	EXPECT_EQ(identity->view_token, 0U);
	backend.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(2)}}}});
	auto valuesChanged = backend.openLogicalGraphReadSnapshot();
	EXPECT_NE(valuesChanged->mutationFence(), pinned->mutationFence());
	// Shared Page Directory COW may conservatively invalidate bindings even
	// though no schema name changed; equality here is deliberately not promised.
	ASSERT_TRUE(valuesChanged->bindingSchemaIdentity());
	EXPECT_TRUE(valuesChanged->bindingSchemaIdentity()->isValid());
	EXPECT_EQ(valuesChanged->bindingSchemaIdentity()->database_token, identity->database_token);
	backend.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(3)}, {"new_binding_name", PropertyValue(4)}}}});
	auto namesChanged = backend.openLogicalGraphReadSnapshot();
	EXPECT_NE(namesChanged->bindingSchemaIdentity(), identity);
	EXPECT_EQ(pinned->bindingSchemaIdentity(), identity);
	const auto finalIdentity = namesChanged->bindingSchemaIdentity();
	namesChanged.reset();
	valuesChanged.reset();
	pinned.reset();
	backend.close();
	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->bindingSchemaIdentity(), finalIdentity);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, IndependentOverlaySchemaViewsDistinguishEqualRevisionsAndRestoreAfterFailure) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}}}}).front();
	auto first = backend.beginLogicalGraphTransaction(false);
	auto second = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay firstOverlay(*first), secondOverlay(*second);
	auto firstView = firstOverlay.openLogicalGraphReadSnapshot();
	auto firstVectorView = firstOverlay.openLogicalGraphVectorSearchSnapshot();
	auto secondView = secondOverlay.openLogicalGraphReadSnapshot();
	const auto before = firstView->bindingSchemaIdentity();
	ASSERT_TRUE(before);
	ASSERT_TRUE(secondView->bindingSchemaIdentity());
	EXPECT_NE(before->view_token, secondView->bindingSchemaIdentity()->view_token);
	EXPECT_EQ(firstVectorView->bindingSchemaIdentity(), before);
	firstOverlay.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(2)}}}});
	secondOverlay.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(3)}}}});
	const auto after = firstView->bindingSchemaIdentity();
	EXPECT_NE(after, before);
	EXPECT_EQ(after->view_token, before->view_token);
	EXPECT_EQ(after->logical_revision, 1U);
	EXPECT_EQ(after->catalog_tokens, before->catalog_tokens);
	EXPECT_NE(after, secondView->bindingSchemaIdentity());
	EXPECT_EQ(firstVectorView->bindingSchemaIdentity(), after);
	LogicalGraphMutationBatch invalid;
	invalid.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
		LogicalGraphMutationReferenceKind::LGRK_PERSISTED, node}, {{"rank", PropertyValue(4)}}}};
	invalid.node_deletes = {{0U, false, 0U}};
	EXPECT_THROW(static_cast<void>(firstOverlay.executeLogicalGraphMutationBatch(invalid)), std::invalid_argument);
	EXPECT_EQ(firstView->bindingSchemaIdentity(), after);
	first->rollback();
	second->rollback();
	EXPECT_THROW(static_cast<void>(firstView->bindingSchemaIdentity()), std::logic_error);
	EXPECT_THROW(static_cast<void>(firstVectorView->bindingSchemaIdentity()), std::logic_error);
	firstVectorView.reset();
	firstView.reset();
	secondView.reset();
	first.reset();
	second.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, SchemaDefinitionPublicationChangesTheAuthenticatedBindingObservation) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	static_cast<void>(backend.createLogicalNodes({{{"BindingConstraint"}, {{"rank", PropertyValue(1)}}}}));
	auto before = backend.openLogicalGraphReadSnapshot();
	static_cast<void>(backend.createLogicalConstraint({"binding_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
		LogicalGraphConstraintKind::LGCK_UNIQUE, "BindingConstraint", {"rank"}}));
	auto after = backend.openLogicalGraphReadSnapshot();
	EXPECT_NE(before->bindingSchemaIdentity(), after->bindingSchemaIdentity());
	EXPECT_EQ(before->bindingSchemaIdentity()->database_token, after->bindingSchemaIdentity()->database_token);
	after.reset();
	before.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, BoundPropertyDescriptorsRejectNewCatalogViewsButRemainValidOnTheirPinnedSource) {
	using namespace graph::query::execution;
	using namespace graph::query::execution::detail;
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}}}}).front();
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	auto pinned = backend.openLogicalGraphReadSnapshot();
	BoundPropertyBindings bindings(slots, *pinned, requests, 1U, 32U, budget);
	const auto descriptor = bindings.descriptor(0U);
	ASSERT_TRUE(bindings.view(descriptor, *pinned).token);
	backend.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(2)}, {"new_descriptor_name", PropertyValue(3)}}}});
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(bindings.view(descriptor, *fresh)), BoundPropertyBindingConflict);
	EXPECT_NO_THROW(static_cast<void>(bindings.view(descriptor, *pinned)));
	EXPECT_EQ(pinned->readNodeProperty(node, "rank"), PropertyValue(1));
	fresh.reset();
	pinned.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RebindingAnOverlayKeepsStagedUntokenizedFieldsAsNameBasedReads) {
	using namespace graph::query::execution;
	using namespace graph::query::execution::detail;
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	auto source = overlay.openLogicalGraphReadSnapshot();
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "new_staged_field"}}};
	BoundPropertyBindings before(slots, *source, requests, 1U, 32U, budget);
	EXPECT_FALSE(before.view(before.descriptor(0U), *source).token);
	overlay.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(2)}, {"new_staged_field", PropertyValue(9)}}}});
	EXPECT_THROW(before.validateSnapshot(*source), BoundPropertyBindingConflict);
	BoundPropertyBindings after(slots, *source, requests, 1U, 32U, budget);
	const auto view = after.view(after.descriptor(0U), *source);
	EXPECT_FALSE(view.token);
	EXPECT_EQ(source->readNodeProperty(node, view.key), PropertyValue(9));
	transaction->rollback();
	EXPECT_THROW(after.validateSnapshot(*source), std::logic_error);
	source.reset();
	transaction.reset();
	backend.close();
}
