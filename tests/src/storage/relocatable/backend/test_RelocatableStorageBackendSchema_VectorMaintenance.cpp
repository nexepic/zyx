/**
 * @file test_RelocatableStorageBackendSchema_VectorMaintenance.cpp
 * @brief Durable Vector rebuild and search-maintenance tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include <atomic>
#include <chrono>
#include <limits>

#ifndef _WIN32
#include <cerrno>
#endif

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

TEST_F(RelocatableStorageBackendSchemaTest, RecoversDurableVectorRebuildDebtAfterReopen) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackendOptions writerOptions = options();
	writerOptions.vector_delta_rebuild_threshold = 1U;
	RelocatableStorageBackend writer(writerOptions);
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);

	// Maintenance is intentionally disabled for this writer, so the committed
	// overlay remains durable debt that only a later writable open may recover.
	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();
	const VectorManifest beforeReopen = readVectorManifest(index.index_id);
	ASSERT_GT(beforeReopen.delta_entry_count, writerOptions.vector_delta_rebuild_threshold);
	ASSERT_TRUE(beforeReopen.delta_mapping_root_page_id.isValid());

	RelocatableStorageBackendOptions recoveryOptions = writerOptions;
	RelocatableMaintenanceOptions maintenance;
	// The combined recovery cursor itself occupies this one slot. The durable
	// Vector debt must still rebuild in this process without a second reopen.
	maintenance.loop.maximum_outstanding_tasks = 1U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	recoveryOptions.maintenance = maintenance;
	RelocatableStorageBackend recovered(recoveryOptions);
	recovered.open();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = recovered.maintenanceMetrics();
		return metrics.vector_rebuild_recovery_completed_rebuild_attempts >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = recovered.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "durable Vector rebuild failed: " << error.what();
		}
	}
	recovered.pauseMaintenance();

	auto snapshot = recovered.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	ASSERT_EQ(search.hits.size(), 1U);
	EXPECT_EQ(search.hits.front().node_id, nodes.front());
	EXPECT_TRUE(search.stats.used_pq);
	EXPECT_TRUE(search.stats.used_adjacency);
	EXPECT_FALSE(search.stats.used_delta);
	snapshot.reset();
	recovered.close();

	const VectorManifest afterRecovery = readVectorManifest(index.index_id);
	EXPECT_EQ(afterRecovery.delta_entry_count, 0U);
	EXPECT_EQ(afterRecovery.delta_mapping_index_id, 0U);
	EXPECT_FALSE(afterRecovery.delta_mapping_root_page_id.isValid());
	EXPECT_FALSE(afterRecovery.delta_mapping_root_generation.isValid());
	EXPECT_TRUE(afterRecovery.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(afterRecovery.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableStorageBackendSchemaTest, EnqueuesCommittedVectorRebuildBeforeResumingMaintenance) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackendOptions configured = options();
	configured.vector_delta_rebuild_threshold = 1U;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 4U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	backend.pauseMaintenance();
	static_cast<void>(backend.cancelQueuedMaintenance());

	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(backend.trainLogicalVector({index.name, 16U, 16U}).trained);

	// Drop work triggered by setup, then observe only the post-commit queueing
	// for the Vector delta that crosses the configured rebuild threshold.
	static_cast<void>(backend.cancelQueuedMaintenance());
	const RelocatableMaintenanceLoopMetrics before = backend.maintenanceMetrics().loop;
	backend.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	const RelocatableMaintenanceLoopMetrics queued = backend.maintenanceMetrics().loop;
	EXPECT_EQ(queued.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	EXPECT_GE(queued.submitted_tasks, before.submitted_tasks + 2U);
	EXPECT_GE(queued.queued_tasks, 2U);
	EXPECT_GE(queued.last_submitted_task_id, before.last_submitted_task_id + 2U);

	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceLoopMetrics metrics = backend.maintenanceMetrics().loop;
		return metrics.completed_tasks > before.completed_tasks ||
			   metrics.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "committed Vector rebuild maintenance failed: " << error.what();
		}
	}
	backend.pauseMaintenance();

	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	ASSERT_EQ(search.hits.size(), 1U);
	EXPECT_EQ(search.hits.front().node_id, nodes.front());
	EXPECT_FALSE(search.stats.used_delta);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RecoversCommittedVectorDebtWhenTheTransientMaintenanceQueueIsFull) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackendOptions configured = options();
	configured.vector_delta_rebuild_threshold = 1U;
	RelocatableMaintenanceOptions maintenance;
	// The durable recovery cursor consumes the sole queue slot. Post-commit
	// best-effort admissions must not make the graph mutation fail.
	maintenance.loop.maximum_outstanding_tasks = 1U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	backend.pauseMaintenance();
	ASSERT_EQ(backend.maintenanceMetrics().loop.queued_tasks, 1U);

	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(backend.trainLogicalVector({index.name, 16U, 16U}).trained);
	const RelocatableMaintenanceLoopMetrics before = backend.maintenanceMetrics().loop;
	backend.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	const RelocatableMaintenanceLoopMetrics afterCommit = backend.maintenanceMetrics().loop;
	EXPECT_EQ(afterCommit.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	EXPECT_EQ(afterCommit.queued_tasks, 1U);
	EXPECT_EQ(afterCommit.last_submitted_task_id, before.last_submitted_task_id);

	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return metrics.vector_rebuild_recovery_completed_rebuild_attempts >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "full-queue Vector recovery failed: " << error.what();
		}
	}
	backend.pauseMaintenance();

	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	ASSERT_EQ(search.hits.size(), 1U);
	EXPECT_EQ(search.hits.front().node_id, nodes.front());
	EXPECT_FALSE(search.stats.used_delta);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaTombstoneWhoseReplacementForwardMappingIsMissingBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);

	// This produces one semantic erase plus one current insert for the same
	// Node. The delta validator must reject a forged current forward mapping
	// before it can expose an inconsistent candidate.
	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	ASSERT_GT(manifest.delta_entry_count, 0U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(nodes.front());
	overwriteBTreeLeafEntry(index.index_id, forwardKey, {forwardKey, 0U});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an erase overlay entry with semantic reachability or an invalid "
			"replacement mapping");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaTombstoneWhoseForwardMappingPointsBackToTheErasedVectorBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);

	// Replacing a base Vector creates an ERASE tombstone for the old Vector and
	// an INSERT for the replacement. Reintroducing the old forward mapping would
	// make the tombstone semantically reachable and must fail before traversal.
	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	ASSERT_GT(manifest.delta_entry_count, 0U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(nodes.front());
	overwriteBTreeLeafEntry(index.index_id, forwardKey, {forwardKey, 1U});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an erase overlay entry with semantic reachability or an invalid "
			"replacement mapping");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaTombstoneWhoseReplacementReverseMappingIsMissingBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(nodes.front());
	const std::vector<BTreePageEntry> forwardEntries = readIndexEntries(index.index_id);
	const auto forward =
			std::ranges::find_if(forwardEntries, [&](const BTreePageEntry &entry) { return entry.key == forwardKey; });
	ASSERT_NE(forward, forwardEntries.end());
	ASSERT_NE(forward->value_or_child, 0U);

	// Keep the current forward mapping intact but remove its exact reverse key.
	// The erase overlay must not regard an unresolved replacement as safe.
	const std::vector<std::byte> replacementKey = VectorSemanticCodec::vectorMappingKey(forward->value_or_child);
	const std::vector<std::byte> missingKey = VectorSemanticCodec::vectorMappingKey(forward->value_or_child + 100U);
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, replacementKey, {missingKey, nodes.front()});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an erase overlay entry with semantic reachability or an invalid "
			"replacement mapping");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaTombstoneWhoseReplacementRawLocationIsMissingBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(nodes.front());
	const std::vector<BTreePageEntry> forwardEntries = readIndexEntries(index.index_id);
	const auto forward =
			std::ranges::find_if(forwardEntries, [&](const BTreePageEntry &entry) { return entry.key == forwardKey; });
	ASSERT_NE(forward, forwardEntries.end());
	ASSERT_NE(forward->value_or_child, 0U);
	const uint64_t missingVectorId = forward->value_or_child + 100U;
	const std::vector<std::byte> replacementKey = VectorSemanticCodec::vectorMappingKey(forward->value_or_child);
	const std::vector<std::byte> missingKey = VectorSemanticCodec::vectorMappingKey(missingVectorId);

	// Forge a complete semantic pair for an ID that has no Page Directory
	// Vector location. The replacement must be rejected before it can be read.
	overwriteBTreeLeafEntry(index.index_id, forwardKey, {forwardKey, missingVectorId});
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, replacementKey, {missingKey, nodes.front()});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an erase overlay entry with semantic reachability or an invalid "
			"replacement mapping");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaInsertWhoseCurrentForwardMappingNoLongerMatchesBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);

	// Inserting after the base generation creates a pure INSERT overlay, so its
	// current forward mapping must remain exactly bijective with the reverse map.
	const std::vector<uint64_t> inserted =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}});
	ASSERT_EQ(inserted.size(), 1U);
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	ASSERT_EQ(manifest.delta_entry_count, 1U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(inserted.front());
	overwriteBTreeLeafEntry(index.index_id, forwardKey, {forwardKey, 0U});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(definition, manifest, request,
									 "relocatable logical Vector search found non-bijective insert overlay mappings");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   RejectsDeltaInsertWhoseCurrentReverseMappingIsMissingOrMismatchedBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
	const std::vector<uint64_t> inserted =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}});
	ASSERT_EQ(inserted.size(), 1U);
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_EQ(manifest.delta_entry_count, 1U);
	const std::vector<std::byte> forwardKey = VectorSemanticCodec::nodeMappingKey(inserted.front());
	const std::vector<BTreePageEntry> forwardEntries = readIndexEntries(index.index_id);
	const auto forward =
			std::ranges::find_if(forwardEntries, [&](const BTreePageEntry &entry) { return entry.key == forwardKey; });
	ASSERT_NE(forward, forwardEntries.end());
	ASSERT_NE(forward->value_or_child, 0U);

	// Preserve the INSERT's durable Raw record and forward mapping, but remove
	// its exact reverse key. A candidate cannot be trusted without both maps.
	const std::vector<std::byte> reverseKey = VectorSemanticCodec::vectorMappingKey(forward->value_or_child);
	const std::vector<std::byte> missingKey = VectorSemanticCodec::vectorMappingKey(forward->value_or_child + 100U);
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, reverseKey, {missingKey, inserted.front()});

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an insert overlay entry without a semantic Vector record");

	// Recreate the durable reverse key, then retain the key while changing only
	// its Node value. Both absence and disagreement must reject the insert.
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, missingKey, {reverseKey, inserted.front()});
	ASSERT_GT(inserted.front(), 1U);
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, reverseKey, {reverseKey, inserted.front() - 1U});
	expectDirectVectorSearchRejected(
			definition, manifest, request,
			"relocatable logical Vector search found an insert overlay entry without a semantic Vector record");
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   FallsBackToExactForATombstonedPqEntryPointButStillValidatesTheCodebookBinding) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
	const VectorManifest base = readVectorManifest(index.index_id);
	ASSERT_EQ(base.entry_point_vector_id, 1U);

	writer.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	writer.close();
	const VectorManifest overlay = readVectorManifest(index.index_id);
	ASSERT_NE(overlay.delta_mapping_index_id, 0U);
	ASSERT_GT(overlay.delta_entry_count, 0U);

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult fallback = snapshot->searchLogicalVector(request);
	ASSERT_EQ(fallback.hits.size(), 1U);
	EXPECT_EQ(fallback.hits.front().node_id, nodes.front());
	EXPECT_TRUE(fallback.stats.used_delta);
	EXPECT_FALSE(fallback.stats.used_adjacency);
	EXPECT_FALSE(fallback.stats.used_pq);
	snapshot.reset();
	reader.close();

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = overlay.dimensions;
	definition.vector_metric = "l2";
	VectorManifest staleCodebook = overlay;
	ASSERT_LT(staleCodebook.codebook_generation.value(), std::numeric_limits<uint64_t>::max());
	staleCodebook.codebook_generation = Generation(staleCodebook.codebook_generation.value() + 1U);
	expectDirectVectorSearchRejected(definition, staleCodebook, request,
									 "relocatable logical Vector search found an incompatible Codebook manifest");
}

TEST_F(RelocatableStorageBackendSchemaTest, FallsBackToExactForATombstonedRawAdjacencyEntryPoint) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::array<CanonicalVectorRecord, 2U> baseRecords = {bfloatRecord(1U, 0.0F, 0.0F),
															   bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, baseRecords);

	RelocatableStorageBackend updater(options());
	updater.open();
	updater.replaceLogicalNodeProperties({{nodes.front(), {{"embedding", vectorValue(1.0, 0.0)}}}});
	updater.close();
	const VectorManifest overlay = readVectorManifest(index.index_id);
	ASSERT_NE(overlay.delta_mapping_index_id, 0U);
	ASSERT_NE(overlay.entry_point_vector_id, 0U);

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	const LogicalGraphVectorSearchResult fallback = snapshot->searchLogicalVector(request);
	ASSERT_EQ(fallback.hits.size(), 1U);
	EXPECT_EQ(fallback.hits.front().node_id, nodes.front());
	EXPECT_TRUE(fallback.stats.used_delta);
	EXPECT_FALSE(fallback.stats.used_adjacency);
	EXPECT_FALSE(fallback.stats.used_pq);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RawAdjacencySuppressesATombstonedNeighborAndRefinesItsReplacementInsert) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::array<CanonicalVectorRecord, 2U> baseRecords = {bfloatRecord(1U, 0.0F, 0.0F),
															   bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, baseRecords);

	RelocatableStorageBackend updater(options());
	updater.open();
	updater.replaceLogicalNodeProperties({{nodes.back(), {{"embedding", vectorValue(1.0, 1.0)}}}});
	updater.close();
	const VectorManifest overlay = readVectorManifest(index.index_id);
	ASSERT_EQ(overlay.delta_entry_count, 2U);
	ASSERT_EQ(overlay.entry_point_vector_id, 1U);
	ASSERT_FALSE(overlay.pq_mapping_root_page_id.isValid());

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 1.0F};
	request.top_k = 2U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	ASSERT_EQ(search.hits.size(), 2U);
	EXPECT_EQ(search.hits.front().node_id, nodes.back());
	EXPECT_EQ(search.hits.back().node_id, nodes.front());
	EXPECT_TRUE(search.stats.used_adjacency);
	EXPECT_TRUE(search.stats.used_delta);
	EXPECT_FALSE(search.stats.used_pq);
	EXPECT_EQ(search.stats.candidate_discoveries, 1U);
	EXPECT_EQ(search.stats.raw_vector_reads, 2U);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsCommittedPqDeltaInsertsThatExceedTheExactRefinementBudget) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);

	// Both records are committed semantic inserts. A smaller refinement budget
	// must reject the request rather than silently omit one visible Vector.
	const std::vector<uint64_t> inserted =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(1.0, 1.0)}}}});
	ASSERT_EQ(inserted.size(), 2U);
	writer.close();
	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_EQ(manifest.delta_entry_count, inserted.size());

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 4U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsCommittedRawAdjacencyDeltaInsertsThatExceedTheExactReadBudget) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::array<CanonicalVectorRecord, 2U> baseRecords = {bfloatRecord(1U, 0.0F, 0.0F),
															   bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, baseRecords);

	RelocatableStorageBackend updater(options());
	updater.open();
	const std::vector<uint64_t> inserted =
			updater.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}},
										{{"Person"}, {{"embedding", vectorValue(1.0, 1.0)}}}});
	ASSERT_EQ(inserted.size(), 2U);
	updater.close();
	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_EQ(manifest.delta_entry_count, inserted.size());
	ASSERT_FALSE(manifest.pq_mapping_root_page_id.isValid());

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 4U;
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, TrainsCosineIndexWithoutChangingExactSemanticReads) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes({{{"CosineMemory"}, {{"embedding", vectorValue(1.0, 0.0)}}},
												  {{"CosineMemory"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeVectorIndex(
			{"cosine_memory_embedding", "CosineMemory", "embedding", 2U, "cosine"});

	const LogicalGraphVectorTrainingResult trained = backend.trainLogicalVector({"cosine_memory_embedding", 16U, 16U});
	EXPECT_TRUE(trained.trained);
	EXPECT_EQ(trained.vector_count, 2U);
	const VectorManifest manifest = readVectorManifest(index.index_id);
	EXPECT_NE(manifest.codebook_id, 0U);
	EXPECT_TRUE(manifest.pq_mapping_root_page_id.isValid());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	auto exact = backend.openLogicalGraphVectorSearchSnapshot();
	LogicalGraphVectorSearchRequest request;
	request.index_name = "cosine_memory_embedding";
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 16U;
	request.maximum_raw_vector_reads = 16U;
	ASSERT_EQ(exact->searchLogicalVector(request).hits.size(), 1U);
	exact.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, EvaluatesPropertiesAndScalarsOverLogicalVectorProcedureRows) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"Person"}, {{"name", graph::PropertyValue("Ada")}, {"embedding", vectorValue(1.0, -2.0)}}},
			 {{"Person"}, {{"name", graph::PropertyValue("Grace")}, {"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "WITH node AS matched, score "
						   "RETURN matched.name AS name, score AS score, score * 2.0 AS doubled, matched AS projected");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"name", "score", "doubled", "projected"}));
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("name").asPrimitive(), graph::PropertyValue("Ada"));
	EXPECT_EQ(result.getRows()[1].at("name").asPrimitive(), graph::PropertyValue("Grace"));
	for (const graph::query::QueryResult::Row &row: result.getRows()) {
		const double score = std::get<double>(row.at("score").asPrimitive().getVariant());
		const double doubled = std::get<double>(row.at("doubled").asPrimitive().getVariant());
		EXPECT_DOUBLE_EQ(doubled, score * 2.0);
		ASSERT_TRUE(row.at("projected").isLogicalNode());
		EXPECT_EQ(row.at("projected").asLogicalNode().properties.at("name"), row.at("name").asPrimitive());
	}
	EXPECT_EQ(result.getRows()[0].at("projected").asLogicalNode().node_id, nodes[0]);
	EXPECT_EQ(result.getRows()[1].at("projected").asLogicalNode().node_id, nodes[1]);
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, DistinctVectorProcedureProjectionUsesLogicalStructuralRowKeys) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Person"}, {{"name", graph::PropertyValue("duplicate")}, {"embedding", vectorValue(1.0, -2.0)}}},
			 {{"Person"}, {{"name", graph::PropertyValue("duplicate")}, {"embedding", vectorValue(-3.0, 4.0)}}}}));
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "RETURN DISTINCT node.name AS name");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"name"}));
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), graph::PropertyValue("duplicate"));
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, DistinctVectorProcedureProjectionKeepsTypedValuesDistinct) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Person"}, {{"value", graph::PropertyValue("[1, 2]")}, {"embedding", vectorValue(1.0, -2.0)}}},
			 {{"Person"},
			  {{"value", graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{1}),
																				graph::PropertyValue(int64_t{2})})},
			   {"embedding", vectorValue(-3.0, 4.0)}}}}));
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "RETURN DISTINCT node.value AS value");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"value"}));
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), graph::PropertyValue("[1, 2]"));
	EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(),
			  graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{1}),
																	 graph::PropertyValue(int64_t{2})}));
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, VectorProcedureLazilyLoadsNodePropertiesForScalarProjection) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Person"}, {{"name", graph::PropertyValue("Ada")}, {"embedding", vectorValue(1.0, -2.0)}}}}));
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::LogicalReadExecutionOptions readOptions;
	readOptions.bounds.maximum_property_entries = 1U;
	graph::query::QueryEngine engine(backend, backend, readOptions);
	const graph::query::QueryResult scalarResult =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 1, [1.0, -2.0]) YIELD node, score "
						   "RETURN score");
	ASSERT_EQ(scalarResult.getColumns(), (std::vector<std::string>{"score"}));
	ASSERT_EQ(scalarResult.rowCount(), 1U);
	EXPECT_EQ(scalarResult.getRows().front().at("score").asPrimitive().getType(), graph::PropertyType::DOUBLE);

	EXPECT_THROW(static_cast<void>(engine.execute(
						 "CALL db.index.vector.queryNodes('person_embedding', 1, [1.0, -2.0]) YIELD node, score "
						 "RETURN node")),
				 std::runtime_error);
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, VectorProcedureHonorsExistingYieldWherePredicate) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Person"}, {{"name", graph::PropertyValue("Ada")}, {"embedding", vectorValue(1.0, -2.0)}}},
			 {{"Person"}, {{"name", graph::PropertyValue("Grace")}, {"embedding", vectorValue(-3.0, 4.0)}}}}));
	static_cast<void>(backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "WHERE score < 1.0 RETURN node.name AS name, score");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"name", "score"}));
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), graph::PropertyValue("Ada"));
	EXPECT_LT(std::get<double>(result.getRows().front().at("score").asPrimitive().getVariant()), 1.0);

	const graph::query::QueryResult nullFiltered =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "WHERE null RETURN score");
	EXPECT_TRUE(nullFiltered.isEmpty());
	EXPECT_THROW(static_cast<void>(engine.execute(
						 "CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						 "WHERE score RETURN score")),
				 std::runtime_error);

	const graph::query::QueryResult propertyFiltered =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "WHERE node.name = 'Ada' RETURN node");
	ASSERT_EQ(propertyFiltered.rowCount(), 1U);
	ASSERT_TRUE(propertyFiltered.getRows().front().at("node").isLogicalNode());
	EXPECT_EQ(propertyFiltered.getRows().front().at("node").asLogicalNode().properties.at("name"),
			  graph::PropertyValue("Ada"));

	const graph::query::QueryResult paged =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "RETURN node.name AS name SKIP 1 LIMIT 1");
	ASSERT_EQ(paged.getColumns(), (std::vector<std::string>{"name"}));
	ASSERT_EQ(paged.rowCount(), 1U);
	EXPECT_EQ(paged.getRows().front().at("name").asPrimitive(), graph::PropertyValue("Grace"));

	const graph::query::QueryResult sorted =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "RETURN node.name AS name ORDER BY node.name DESC");
	ASSERT_EQ(sorted.getColumns(), (std::vector<std::string>{"name"}));
	ASSERT_EQ(sorted.rowCount(), 2U);
	EXPECT_EQ(sorted.getRows()[0].at("name").asPrimitive(), graph::PropertyValue("Grace"));
	EXPECT_EQ(sorted.getRows()[1].at("name").asPrimitive(), graph::PropertyValue("Ada"));

	const graph::query::QueryResult emptyPage =
			engine.execute("CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score "
						   "RETURN score LIMIT 0");
	EXPECT_TRUE(emptyPage.isEmpty());
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsVectorDdlBeforePublishingAnIncompleteHistoricalIndex) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const graph::PropertyValue vector(
			std::vector<graph::PropertyValue>{graph::PropertyValue(1.0), graph::PropertyValue(2.0)});
	static_cast<void>(
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vector}}}, {{"Person"}, {{"embedding", vector}}}}));
	RelocatableStorageBackendOptions limited = options();
	limited.maximum_online_index_entries = 1U;
	backend.close();

	RelocatableStorageBackend constrained(limited);
	constrained.open();
	EXPECT_THROW(static_cast<void>(constrained.createLogicalNodeVectorIndex(
						 {"person_embedding", "Person", "embedding", 2U, "l2"})),
				 FormatException);
	EXPECT_TRUE(constrained.listLogicalIndexes(8U).empty());
	constrained.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RequiresConsistentCodebookCacheBoundsAndAllowsExplicitDisable) {
	RelocatableStorageBackendOptions missingEntryBound = options();
	missingEntryBound.maximum_vector_codebook_cache_entries = 0U;
	RelocatableStorageBackend invalidEntryBound(missingEntryBound);
	EXPECT_THROW(invalidEntryBound.open(), std::invalid_argument);

	RelocatableStorageBackendOptions missingByteBound = options();
	missingByteBound.maximum_vector_codebook_cache_bytes = 0U;
	RelocatableStorageBackend invalidByteBound(missingByteBound);
	EXPECT_THROW(invalidByteBound.open(), std::invalid_argument);

	RelocatableStorageBackendOptions disabled = options();
	disabled.maximum_vector_codebook_cache_bytes = 0U;
	disabled.maximum_vector_codebook_cache_entries = 0U;
	RelocatableStorageBackend noCache(disabled);
	EXPECT_NO_THROW(noCache.open());
	noCache.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsInconsistentVectorDeltaBoundsBeforeOpening) {
	RelocatableStorageBackendOptions zeroThreshold = options();
	zeroThreshold.vector_delta_rebuild_threshold = 0U;
	RelocatableStorageBackend invalidZeroThreshold(zeroThreshold);
	EXPECT_THROW(invalidZeroThreshold.open(), std::invalid_argument);

	RelocatableStorageBackendOptions inverted = options();
	inverted.vector_delta_rebuild_threshold = 2U;
	inverted.maximum_vector_delta_overlay_entries = 1U;
	RelocatableStorageBackend invalidInverted(inverted);
	EXPECT_THROW(invalidInverted.open(), std::invalid_argument);

	RelocatableStorageBackendOptions exceedsTree = options();
	exceedsTree.vector_delta_rebuild_threshold = 1U;
	exceedsTree.maximum_vector_delta_pages = 1U;
	exceedsTree.maximum_vector_delta_overlay_entries =
			VectorDeltaMapping::maximumEntriesForPageBudget(exceedsTree.maximum_vector_delta_pages) + 1U;
	RelocatableStorageBackend invalidTreeCapacity(exceedsTree);
	EXPECT_THROW(invalidTreeCapacity.open(), std::invalid_argument);
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsEveryNonFiniteWritableResourceBoundBeforeOpening) {
	using Mutator = std::function<void(RelocatableStorageBackendOptions &)>;
	const size_t sizeMax = std::numeric_limits<size_t>::max();
	const size_t uint32MaxPlusOne = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;
	const std::vector<Mutator> invalidBounds{
			[](auto &value) { value.maximum_index_definitions = 0U; },
			[sizeMax](auto &value) { value.maximum_index_definitions = sizeMax; },
			[](auto &value) { value.maximum_constraint_definitions = 0U; },
			[sizeMax](auto &value) { value.maximum_constraint_definitions = sizeMax; },
			[](auto &value) { value.maximum_online_index_entries = 0U; },
			[sizeMax](auto &value) { value.maximum_online_index_entries = sizeMax; },
			[](auto &value) { value.maximum_online_index_scan_nodes = 0U; },
			[sizeMax](auto &value) { value.maximum_online_index_scan_nodes = sizeMax; },
			[](auto &value) { value.maximum_online_index_pages = 0U; },
			[sizeMax](auto &value) { value.maximum_online_index_pages = sizeMax; },
			[](auto &value) { value.online_index_sort_run_bytes = 0U; },
			[sizeMax](auto &value) { value.online_index_sort_run_bytes = sizeMax; },
			[](auto &value) { value.maximum_online_index_sort_runs = 1U; },
			[sizeMax](auto &value) { value.maximum_online_index_sort_runs = sizeMax; },
			[](auto &value) { value.vector_delta_rebuild_threshold = 0U; },
			[sizeMax](auto &value) { value.vector_delta_rebuild_threshold = sizeMax; },
			[uint32MaxPlusOne](auto &value) { value.vector_delta_rebuild_threshold = uint32MaxPlusOne; },
			[](auto &value) { value.maximum_vector_delta_overlay_entries = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_delta_overlay_entries = sizeMax; },
			[uint32MaxPlusOne](auto &value) { value.maximum_vector_delta_overlay_entries = uint32MaxPlusOne; },
			[](auto &value) {
				value.vector_delta_rebuild_threshold = 2U;
				value.maximum_vector_delta_overlay_entries = 1U;
			},
			[](auto &value) { value.maximum_vector_delta_pages = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_delta_pages = sizeMax; },
			[](auto &value) {
				value.maximum_vector_delta_pages = 1U;
				value.maximum_vector_delta_overlay_entries = VectorDeltaMapping::maximumEntriesForPageBudget(1U) + 1U;
			},
			[](auto &value) { value.maximum_vector_training_records = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_training_records = sizeMax; },
			[](auto &value) { value.maximum_vector_training_pages = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_training_pages = sizeMax; },
			[](auto &value) { value.maximum_vector_training_samples = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_training_samples = sizeMax; },
			[](auto &value) { value.vector_training_iterations = 0U; },
			[](auto &value) { value.maximum_adjacency_read_edges = 0U; },
			[sizeMax](auto &value) { value.maximum_adjacency_read_edges = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_transaction_records = 0U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_transaction_records = sizeMax; },
			[](auto &value) { value.bulk_edge_id_lease_block_records = 0U; },
			[sizeMax](auto &value) { value.bulk_edge_id_lease_block_records = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_append_records = 0U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_append_records = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_append_bytes = 0U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_append_bytes = sizeMax; },
			[](auto &value) { value.bulk_edge_sort_run_bytes = 0U; },
			[sizeMax](auto &value) { value.bulk_edge_sort_run_bytes = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_sort_runs = 1U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_sort_runs = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_candidate_pages = 0U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_candidate_pages = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_index_entries = 0U; },
			[sizeMax](auto &value) { value.maximum_bulk_edge_index_entries = sizeMax; },
			[](auto &value) { value.maximum_bulk_edge_transaction_records = 10'000'001U; },
			[](auto &value) { value.maximum_vector_adjacency_exact_records = 0U; },
			[sizeMax](auto &value) { value.maximum_vector_adjacency_exact_records = sizeMax; },
			[](auto &value) { value.maximum_vector_adjacency_degree = 0U; },
			[](auto &value) { value.maximum_vector_adjacency_degree = RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS + 1U; },
			[](auto &value) { value.vector_adjacency_candidate_search_budget = 0U; },
			[sizeMax](auto &value) { value.vector_adjacency_candidate_search_budget = sizeMax; },
			[](auto &value) { value.vector_adjacency_pruning_alpha = std::numeric_limits<float>::quiet_NaN(); },
			[](auto &value) { value.vector_adjacency_pruning_alpha = 0.5F; },
			[](auto &value) { value.maximum_constraint_support_collisions = 0U; },
			[sizeMax](auto &value) { value.maximum_constraint_support_collisions = sizeMax; },
	};

	for (size_t index = 0U; index < invalidBounds.size(); ++index) {
		SCOPED_TRACE(std::to_string(index));
		RelocatableStorageBackendOptions invalid = options();
		invalidBounds[index](invalid);
		RelocatableStorageBackend backend(std::move(invalid));
		EXPECT_THROW(backend.open(), std::invalid_argument);
		EXPECT_FALSE(backend.isOpen());
	}
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsAStaleRawVectorRecordBeforeScoringIt) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	// The Node-to-Vector mapping still resolves VectorId 1, but the fixed-stride
	// raw payload now advertises a different identity. Search must fail closed
	// before it attempts distance scoring.
	std::vector<std::byte> stale = readVectorRecord(1U);
	ASSERT_GE(stale.size(), sizeof(uint64_t));
	LittleEndianCodec::writeU64(stale, 0U, 2U);
	overwriteVectorRecord(1U, stale);

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), FormatException);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsAPersistedZeroNormCosineRawVector) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding_cosine", "Person", "embedding", 2U, "cosine"});
	writer.close();

	// Preserve the record identity and page layout while replacing the two
	// bfloat16 coordinates with zero. DDL/write-time validation cannot protect
	// an already persisted corrupt record, so the read boundary must reject it.
	std::vector<std::byte> zeroNorm = readVectorRecord(1U);
	ASSERT_EQ(zeroNorm.size(), sizeof(uint64_t) + 2U * sizeof(uint16_t));
	LittleEndianCodec::writeU16(zeroNorm, sizeof(uint64_t), 0U);
	LittleEndianCodec::writeU16(zeroNorm, sizeof(uint64_t) + sizeof(uint16_t), 0U);
	overwriteVectorRecord(1U, zeroNorm);

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), FormatException);
	snapshot.reset();
	reader.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsAnOutOfRangePersistedPqCentroidCode) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(writer.trainLogicalVector({index.name, 16U, 16U}).trained);
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	const auto pqPage = std::ranges::find_if(manifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_NE(pqPage, manifest.entries.end());
	std::vector<std::byte> corrupt = readVectorPageRecord(pqPage->page_id, 1U);
	ASSERT_GT(corrupt.size(), sizeof(uint64_t));
	corrupt[sizeof(uint64_t)] = std::byte{0xFF};
	overwriteVectorPageRecord(pqPage->page_id, 1U, corrupt);

	RelocatableStorageBackend reader(options());
	reader.open();
	auto snapshot = reader.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {1.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 2U;
	request.candidate_refinement_budget = 2U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), FormatException);
	snapshot.reset();
	reader.close();
}
