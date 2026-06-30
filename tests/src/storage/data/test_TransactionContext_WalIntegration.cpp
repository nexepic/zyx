/**
 * @file test_TransactionContext_WalIntegration.cpp
 * @brief Tests transaction bookkeeping paths that feed durable WAL writes.
 */

#include <gtest/gtest.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/data/TransactionContext.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace {

template<typename Entity>
Entity makeEntity(int64_t id);

template<>
graph::Node makeEntity<graph::Node>(int64_t id) {
	return graph::Node(id, 11);
}

template<>
graph::Edge makeEntity<graph::Edge>(int64_t id) {
	return graph::Edge(id, 101, 202, 22);
}

template<>
graph::Property makeEntity<graph::Property>(int64_t id) {
	graph::Property property(id, 101, graph::toUnderlying(graph::EntityType::Node));
	property.setProperties({{"name", graph::PropertyValue("neo")}});
	return property;
}

template<>
graph::Blob makeEntity<graph::Blob>(int64_t id) {
	return graph::Blob(id, "payload-" + std::to_string(id));
}

template<>
graph::Index makeEntity<graph::Index>(int64_t id) {
	return graph::Index(id, graph::Index::NodeType::LEAF, 3);
}

template<>
graph::State makeEntity<graph::State>(int64_t id) {
	return graph::State(id, "state-" + std::to_string(id), "value");
}

template<typename Entity>
std::vector<uint8_t> serializeFixedEntity(const Entity &entity) {
	std::vector<uint8_t> bytes(Entity::getTotalSize());
	graph::utils::FixedSizeSerializer::serializeInto(
			reinterpret_cast<char *>(bytes.data()),
			entity,
			bytes.size());
	return bytes;
}

template<typename Entity>
void expectDuplicateAddsAreRecordedOnceForWal() {
	graph::storage::TransactionContext ctx;
	ctx.setActive(7);
	const auto entity = makeEntity<Entity>(123);

	ctx.recordAdd<Entity>(entity);
	ctx.recordAdd<Entity>(entity);
	ctx.recordAdds<Entity>({entity, entity});

	const auto entityType = static_cast<uint8_t>(Entity::typeId);
	ASSERT_EQ(ctx.getOps().size(), 4U);
	ASSERT_EQ(ctx.undoLog().size(), 4U);
	ASSERT_LT(entityType, ctx.pendingWalAddsByType().size());
	ASSERT_EQ(ctx.pendingWalAddsByType()[entityType].size(), 1U);
	EXPECT_EQ(ctx.pendingWalAddsByType()[entityType][0], entity.getId());
}

template<typename Entity>
void expectInactiveAndInvalidAddsAreIgnoredForWalStaging() {
	graph::storage::TransactionContext inactiveCtx;
	inactiveCtx.recordAdd<Entity>(makeEntity<Entity>(700));
	inactiveCtx.recordAdds<Entity>({makeEntity<Entity>(701)});
	EXPECT_TRUE(inactiveCtx.getOps().empty());

	graph::storage::TransactionContext activeCtx;
	activeCtx.setActive(16);
	activeCtx.recordAdds<Entity>({});
	auto invalid = makeEntity<Entity>(0);
	activeCtx.recordAdd<Entity>(invalid);
	EXPECT_TRUE(activeCtx.pendingWalAddsByType()[static_cast<uint8_t>(Entity::typeId)].empty());
}

template<typename Entity>
void expectInactiveMutationRecordersAreNoops() {
	graph::storage::TransactionContext ctx;
	const auto before = makeEntity<Entity>(801);
	auto after = before;

	ctx.recordAdd<Entity>(before);
	ctx.recordAdds<Entity>({before});
	ctx.recordUpdate<Entity>(after, before);
	ctx.recordUpdates<Entity>({after}, {before});
	ctx.recordDelete<Entity>(before.getId(), [](int64_t id) { return makeEntity<Entity>(id); });

	EXPECT_FALSE(ctx.isActive());
	EXPECT_TRUE(ctx.getOps().empty());
	EXPECT_TRUE(ctx.undoLog().empty());
	EXPECT_FALSE(ctx.hasPendingWalRecords());
}

template<typename Entity>
void expectRecordUpdateBranches() {
	graph::storage::TransactionContext ctx;
	ctx.setActive(8);
	const auto before = makeEntity<Entity>(201);
	auto after = before;

	ctx.recordUpdate<Entity>(after, before);
	ASSERT_EQ(ctx.pendingWalChanges().size(), 1U);

	ctx.recordUpdates<Entity>({}, {});
	EXPECT_EQ(ctx.pendingWalChanges().size(), 1U);

	EXPECT_THROW(ctx.recordUpdates<Entity>({after}, {}), std::invalid_argument);

	ctx.recordAdd<Entity>(makeEntity<Entity>(202));
	ctx.recordUpdate<Entity>(makeEntity<Entity>(202), makeEntity<Entity>(202));
	ctx.recordUpdates<Entity>({makeEntity<Entity>(202)}, {makeEntity<Entity>(202)});
	EXPECT_EQ(ctx.pendingWalAddsByType()[static_cast<uint8_t>(Entity::typeId)].size(), 1U);

	const auto zeroId = makeEntity<Entity>(0);
	ctx.recordUpdate<Entity>(zeroId, zeroId);
	ctx.recordUpdates<Entity>({zeroId}, {zeroId});
	EXPECT_FALSE(ctx.pendingWalChanges().contains(
			(static_cast<uint64_t>(Entity::typeId) << 56U)));
}

template<typename Entity>
void expectRecordDeleteBranches() {
	graph::storage::TransactionContext addedCtx;
	addedCtx.setActive(9);
	const auto added = makeEntity<Entity>(301);
	addedCtx.recordAdd<Entity>(added);
	addedCtx.recordDelete<Entity>(added.getId(), [](int64_t id) { return makeEntity<Entity>(id); });
	EXPECT_TRUE(addedCtx.hasCanceledPendingWalAdds(static_cast<uint8_t>(Entity::typeId)));
	ASSERT_EQ(addedCtx.pendingWalAddsByType()[static_cast<uint8_t>(Entity::typeId)].size(), 1U);
	EXPECT_EQ(addedCtx.pendingWalAddsByType()[static_cast<uint8_t>(Entity::typeId)].front(), added.getId());

	graph::storage::TransactionContext existingCtx;
	existingCtx.setActive(10);
	const auto existing = makeEntity<Entity>(302);
	existingCtx.recordDelete<Entity>(existing.getId(), [](int64_t id) { return makeEntity<Entity>(id); });
	ASSERT_EQ(existingCtx.pendingWalChanges().size(), 1U);
	EXPECT_EQ(existingCtx.pendingWalChanges().begin()->second.entityId, existing.getId());

	existingCtx.recordDelete<Entity>(0, [](int64_t id) { return makeEntity<Entity>(id); });
	EXPECT_EQ(existingCtx.pendingWalChanges().size(), 1U);
}

template<typename Entity>
void exerciseWalEntityFlushGuards(graph::storage::wal::WALManager &wal) {
	const auto entity = makeEntity<Entity>(401);

	graph::storage::TransactionContext inactiveCtx;
	inactiveCtx.flushWalEntities<Entity>(graph::storage::EntityChangeType::CHANGE_ADDED, {entity});

	graph::storage::TransactionContext noWalCtx;
	noWalCtx.setActive(11);
	noWalCtx.flushWalEntities<Entity>(graph::storage::EntityChangeType::CHANGE_ADDED, {entity});

	graph::storage::TransactionContext walCtx;
	walCtx.setActive(12);
	walCtx.setWALManager(&wal);
	walCtx.flushWalEntities<Entity>(graph::storage::EntityChangeType::CHANGE_ADDED, {});
	walCtx.flushWalEntities<Entity>(graph::storage::EntityChangeType::CHANGE_ADDED, {entity});
}

class TransactionContextWalIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() /
				 ("zyx_txn_context_wal_" + boost::uuids::to_string(uuid) + ".db");
		removeArtifacts();
	}

	void TearDown() override { removeArtifacts(); }

	void removeArtifacts() const {
		std::error_code ignored;
		std::filesystem::remove(dbPath, ignored);
		std::filesystem::remove(dbPath.string() + "-wal", ignored);
	}

	std::filesystem::path dbPath;
};

} // namespace

TEST_F(TransactionContextWalIntegrationTest, DuplicateBulkAddsOnlyStageOneWalAddPerEntity) {
	expectDuplicateAddsAreRecordedOnceForWal<graph::Node>();
	expectDuplicateAddsAreRecordedOnceForWal<graph::Edge>();
	expectDuplicateAddsAreRecordedOnceForWal<graph::Property>();
	expectDuplicateAddsAreRecordedOnceForWal<graph::Blob>();
	expectDuplicateAddsAreRecordedOnceForWal<graph::Index>();
	expectDuplicateAddsAreRecordedOnceForWal<graph::State>();
}

TEST_F(TransactionContextWalIntegrationTest, InactiveAndInvalidAddsDoNotStageWalAdds) {
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::Node>();
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::Edge>();
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::Property>();
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::Blob>();
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::Index>();
	expectInactiveAndInvalidAddsAreIgnoredForWalStaging<graph::State>();
}

TEST_F(TransactionContextWalIntegrationTest, InactiveMutationRecordersAreNoops) {
	expectInactiveMutationRecordersAreNoops<graph::Node>();
	expectInactiveMutationRecordersAreNoops<graph::Edge>();
	expectInactiveMutationRecordersAreNoops<graph::Property>();
	expectInactiveMutationRecordersAreNoops<graph::Blob>();
	expectInactiveMutationRecordersAreNoops<graph::Index>();
	expectInactiveMutationRecordersAreNoops<graph::State>();
}

TEST_F(TransactionContextWalIntegrationTest, RecordUpdatesHandleEmptyMismatchedAndAddedEntities) {
	expectRecordUpdateBranches<graph::Node>();
	expectRecordUpdateBranches<graph::Edge>();
	expectRecordUpdateBranches<graph::Property>();
	expectRecordUpdateBranches<graph::Blob>();
	expectRecordUpdateBranches<graph::Index>();
	expectRecordUpdateBranches<graph::State>();
}

TEST_F(TransactionContextWalIntegrationTest, RecordDeletesHandleAddedAndExistingEntities) {
	expectRecordDeleteBranches<graph::Node>();
	expectRecordDeleteBranches<graph::Edge>();
	expectRecordDeleteBranches<graph::Property>();
	expectRecordDeleteBranches<graph::Blob>();
	expectRecordDeleteBranches<graph::Index>();
	expectRecordDeleteBranches<graph::State>();
}

TEST_F(TransactionContextWalIntegrationTest, PendingWalHelpersHandleInvalidEntityIdsAndTypes) {
	graph::storage::TransactionContext ctx;
	ctx.setActive(13);
	EXPECT_FALSE(ctx.wasEntityAddedInActiveTransaction(graph::Node::typeId, 0));
	EXPECT_FALSE(ctx.wasEntityAddedInActiveTransaction(graph::Node::typeId, -1));
	EXPECT_FALSE(ctx.hasCanceledPendingWalAdds(static_cast<uint8_t>(graph::getMaxEntityType() + 1)));

	graph::Node invalidId(0, 1);
	ctx.recordAdd(invalidId);
	EXPECT_TRUE(ctx.pendingWalAddsByType()[graph::Node::typeId].empty());

	ctx.recordAdd(makeEntity<graph::Node>(601));
	EXPECT_TRUE(ctx.hasPendingWalRecords());
	ctx.clear();
	EXPECT_FALSE(ctx.isActive());
	EXPECT_FALSE(ctx.hasPendingWalRecords());
	EXPECT_TRUE(ctx.getOps().empty());
	EXPECT_TRUE(ctx.undoLog().empty());
}

TEST_F(TransactionContextWalIntegrationTest, FlushWalEntitiesWritesBatchedEntityRecords) {
	graph::storage::wal::WALManager wal;
	wal.open(dbPath.string());
	wal.writeBegin(12);

	exerciseWalEntityFlushGuards<graph::Node>(wal);
	exerciseWalEntityFlushGuards<graph::Edge>(wal);
	exerciseWalEntityFlushGuards<graph::Property>(wal);
	exerciseWalEntityFlushGuards<graph::Blob>(wal);
	exerciseWalEntityFlushGuards<graph::Index>(wal);
	exerciseWalEntityFlushGuards<graph::State>(wal);

	wal.sync();
	const auto result = wal.readRecords();
	EXPECT_FALSE(result.corrupted);
	ASSERT_EQ(result.records.size(), 7U);
	EXPECT_EQ(result.records[0].header.type, graph::storage::wal::WALRecordType::WAL_TXN_BEGIN);
	for (size_t i = 1; i < result.records.size(); ++i) {
		EXPECT_EQ(result.records[i].header.type, graph::storage::wal::WALRecordType::WAL_ENTITY_WRITE);
		ASSERT_GE(result.records[i].data.size(), sizeof(graph::storage::wal::WALEntityPayload));
		const auto payload = graph::storage::wal::deserializeEntityPayload(result.records[i].data.data());
		EXPECT_EQ(payload.changeType, static_cast<uint8_t>(graph::storage::EntityChangeType::CHANGE_ADDED));
		EXPECT_GT(payload.dataSize, 0U);
	}
	wal.close(graph::storage::wal::WALManager::CloseMode::WCM_REMOVE_FILE);
}

TEST_F(TransactionContextWalIntegrationTest, FlushSerializedWalChangeAndViewsHonorGuards) {
	graph::storage::wal::WALManager wal;
	wal.open(dbPath.string());

	const auto node = makeEntity<graph::Node>(501);
	const auto nodeBytes = serializeFixedEntity(node);
	graph::storage::TransactionContext::PendingWalChange change{
			static_cast<uint8_t>(graph::Node::typeId),
			graph::storage::EntityChangeType::CHANGE_MODIFIED,
			node.getId(),
			nodeBytes};

	graph::storage::TransactionContext inactiveCtx;
	inactiveCtx.flushSerializedWalChange(change);
	graph::storage::wal::WALEntityChangeView view{
			static_cast<uint8_t>(graph::Node::typeId),
			static_cast<uint8_t>(graph::storage::EntityChangeType::CHANGE_MODIFIED),
			node.getId(),
			nodeBytes.data(),
			static_cast<uint32_t>(nodeBytes.size())};
	inactiveCtx.flushWalChangeViews(std::span<const graph::storage::wal::WALEntityChangeView>(&view, 1));

	graph::storage::TransactionContext noWalCtx;
	noWalCtx.setActive(14);
	noWalCtx.flushSerializedWalChange(change);
	noWalCtx.flushWalChangeViews(std::span<const graph::storage::wal::WALEntityChangeView>(&view, 1));

	graph::storage::TransactionContext walCtx;
	walCtx.setActive(15);
	walCtx.setWALManager(&wal);
	walCtx.flushSerializedWalChange({});
	walCtx.flushWalChangeViews({});
	walCtx.flushSerializedWalChange(change);
	walCtx.flushWalChangeViews(std::span<const graph::storage::wal::WALEntityChangeView>(&view, 1));

	wal.sync();
	const auto result = wal.readRecords();
	EXPECT_FALSE(result.corrupted);
	ASSERT_EQ(result.records.size(), 2U);
	for (const auto &record : result.records) {
		EXPECT_EQ(record.header.type, graph::storage::wal::WALRecordType::WAL_ENTITY_WRITE);
		const auto payload = graph::storage::wal::deserializeEntityPayload(record.data.data());
		EXPECT_EQ(payload.entityType, static_cast<uint8_t>(graph::Node::typeId));
		EXPECT_EQ(payload.entityId, node.getId());
		EXPECT_EQ(payload.dataSize, nodeBytes.size());
	}
	wal.close(graph::storage::wal::WALManager::CloseMode::WCM_REMOVE_FILE);
}
