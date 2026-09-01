/**
 * @file test_RelocatableStorageBackend_LifecycleContracts.cpp
 * @brief Lifecycle and write-surface contracts for the relocatable backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, LifecycleBoundariesRejectOperationsOutsideAnOpenBackend) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(backendOptions());

	EXPECT_THROW(backend.flush(), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.openSnapshot()), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.beginLogicalGraphTransaction(true)), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"unopened"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.listLogicalIndexes(1U)), std::logic_error);
	EXPECT_THROW(backend.pauseMaintenance(), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.verifyIntegrity()), std::logic_error);

	backend.open();
	const RelocatableStorageBackend &constBackend = backend;
	EXPECT_THROW(static_cast<void>(constBackend.maintenanceMetrics()), std::logic_error);
	EXPECT_THROW(static_cast<void>(constBackend.maintenanceFailure()), std::logic_error);
	EXPECT_TRUE(backend.createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>{}).empty());
	EXPECT_TRUE(backend.createLogicalEdges(std::vector<LogicalGraphEdgeCreateRequest>{}).empty());
	EXPECT_NO_THROW(backend.replaceLogicalNodeProperties(std::vector<LogicalGraphPropertyReplaceRequest>{}));
	EXPECT_NO_THROW(backend.replaceLogicalEdgeProperties(std::vector<LogicalGraphPropertyReplaceRequest>{}));
	EXPECT_NO_THROW(backend.deleteLogicalNodes(std::vector<LogicalGraphNodeDeleteRequest>{}));
	EXPECT_NO_THROW(backend.deleteLogicalEdges(std::vector<LogicalGraphEdgeDeleteRequest>{}));
	EXPECT_TRUE(backend.executeLogicalGraphMutationBatch(LogicalGraphMutationBatch{}).created_node_ids.empty());
	EXPECT_THROW(static_cast<void>(backend.reserveLogicalGraphCreationIds(0U, 0U)), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.reserveNativeBulkEdgeSessionIds(0U)), std::invalid_argument);
	EXPECT_NO_THROW(static_cast<void>(backend.currentNativeMetadataGeneration()));
	EXPECT_NO_THROW(static_cast<void>(backend.currentNativeLogicalGraphMutationFence()));
	EXPECT_NO_THROW(static_cast<void>(backend.nextNativeBulkScratchCheckpointLsn()));
	backend.close();

	EXPECT_THROW(backend.flush(), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.openSnapshot()), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.beginLogicalGraphTransaction(true)), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"closed"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.listLogicalIndexes(1U)), std::logic_error);
	EXPECT_THROW(backend.pauseMaintenance(), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.verifyIntegrity()), std::logic_error);
}

TEST_F(RelocatableStorageBackendTest, LifecycleOperationsAreIdempotentAcrossWritableAndReadOnlyHandles) {
	materializeNativeDatabase();

	RelocatableStorageBackend writable(backendOptions());
	EXPECT_NO_THROW(writable.open());
	EXPECT_NO_THROW(writable.open());
	EXPECT_TRUE(writable.isOpen());
	EXPECT_NO_THROW(writable.flush());
	EXPECT_NO_THROW(writable.close());
	EXPECT_FALSE(writable.isOpen());
	EXPECT_NO_THROW(writable.close());

	RelocatableStorageBackendOptions readOnlyOptions = backendOptions();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(std::move(readOnlyOptions));
	EXPECT_NO_THROW(readOnly.open());
	EXPECT_NO_THROW(readOnly.open());
	EXPECT_NO_THROW(readOnly.flush());
	EXPECT_NO_THROW(readOnly.close());
	EXPECT_NO_THROW(readOnly.close());
}

TEST_F(RelocatableStorageBackendTest, EnforcesLifecycleAndReadOnlyNativeWriteGuards) {
	materializeNativeDatabase();

	RelocatableStorageBackend backend(backendOptions());
	EXPECT_FALSE(backend.isOpen());
	EXPECT_NO_THROW(backend.close());
	EXPECT_THROW(backend.flush(), std::logic_error);

	backend.open();
	EXPECT_TRUE(backend.isOpen());
	EXPECT_NO_THROW(backend.open());
	{
		RelocatableReadSnapshot snapshot = backend.openSnapshot();
		EXPECT_THROW(backend.close(), std::logic_error);
		snapshot.release();
	}
	EXPECT_NO_THROW(backend.flush());
	backend.close();
	EXPECT_FALSE(backend.isOpen());
	EXPECT_NO_THROW(backend.close());

	RelocatableStorageBackendOptions readOnlyOptions = backendOptions();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend reader(std::move(readOnlyOptions));
	reader.open();
	EXPECT_NO_THROW(reader.flush());
	EXPECT_THROW(static_cast<void>(reader.beginLogicalGraphTransaction(false)), std::logic_error);
	EXPECT_THROW(static_cast<void>(reader.reserveNativeBulkEdgeSessionIds(1U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(reader.acquireNativePublicationGate()), std::logic_error);
	EXPECT_THROW(static_cast<void>(reader.createNativeBulkRunStore()), std::logic_error);
	RelocatableReadSnapshot current = reader.openSnapshot();
	EXPECT_EQ(reader.currentNativeMetadataGeneration(), current.superblock().generation);
	current.release();
	EXPECT_NO_THROW(static_cast<void>(reader.currentNativeLogicalGraphMutationFence()));
	std::unique_ptr<LogicalGraphTransaction> readTransaction = reader.beginLogicalGraphTransaction(true);
	ASSERT_NE(readTransaction, nullptr);
	EXPECT_NO_THROW(readTransaction->rollback());
	reader.close();
}

TEST(RelocatableStorageBackendFenceTest, RejectsFenceWithoutDatabaseIdentity) {
	Superblock invalid;
	invalid.generation = Generation(1U);
	invalid.checkpoint_lsn = LogSequenceNumber(1U);
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::makeLogicalGraphMutationFence(invalid)),
				 FormatException);
}
