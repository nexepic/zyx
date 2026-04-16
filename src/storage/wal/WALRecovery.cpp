/**
 * @file WALRecovery.cpp
 * @date 2026/4/16
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/wal/WALRecovery.hpp"
#include <sstream>
#include <stdexcept>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageWriter.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage::wal {

	WALRecovery::WALRecovery(WALManager &wal,
							 std::shared_ptr<StorageWriter> writer,
							 std::shared_ptr<IDAllocator> idAllocator,
							 std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<StorageIO> storageIO,
							 std::shared_ptr<DataManager> dataManager,
							 std::shared_ptr<FileHeaderManager> fileHeaderManager) :
		wal_(wal),
		writer_(std::move(writer)),
		idAllocator_(std::move(idAllocator)),
		segmentTracker_(std::move(segmentTracker)),
		storageIO_(std::move(storageIO)),
		dataManager_(std::move(dataManager)),
		fileHeaderManager_(std::move(fileHeaderManager)) {}

	// ── Phase 1: Scan ──────────────────────────────────────────────────────

	std::unordered_set<uint64_t> WALRecovery::findCommittedTxns(const std::vector<WALReadRecord> &records) {
		std::unordered_set<uint64_t> committed;
		for (const auto &rec : records) {
			if (rec.header.type == WALRecordType::WAL_TXN_COMMIT) {
				committed.insert(rec.header.txnId);
			}
		}
		return committed;
	}

	// ── Phase 2: Deduplicate ───────────────────────────────────────────────

	std::map<WALRecovery::EntityKey, WALRecovery::EntityState> WALRecovery::buildFinalStateMap(
			const std::vector<WALReadRecord> &records,
			const std::unordered_set<uint64_t> &committedTxns) {
		std::map<EntityKey, EntityState> finalState;

		for (const auto &rec : records) {
			if (rec.header.type != WALRecordType::WAL_ENTITY_WRITE)
				continue;
			if (!committedTxns.contains(rec.header.txnId))
				continue;

			if (rec.data.size() < sizeof(WALEntityPayload))
				continue;

			WALEntityPayload payload = deserializeEntityPayload(rec.data.data());

			EntityKey key{payload.entityType, payload.entityId};

			// Extract serialized entity data (after the WALEntityPayload header)
			std::vector<uint8_t> entityData;
			if (payload.dataSize > 0) {
				size_t payloadHeaderSize = sizeof(WALEntityPayload);
				if (rec.data.size() >= payloadHeaderSize + payload.dataSize) {
					entityData.assign(rec.data.begin() + static_cast<ptrdiff_t>(payloadHeaderSize),
									  rec.data.begin() + static_cast<ptrdiff_t>(payloadHeaderSize + payload.dataSize));
				}
			}

			// Last-writer-wins: overwrite any previous entry for this entity
			finalState[key] = EntityState{payload.changeType, std::move(entityData)};
		}

		return finalState;
	}

	// ── Phase 3: Replay ────────────────────────────────────────────────────

	void WALRecovery::replayEntity(const EntityKey &key, const EntityState &state) {
		auto changeType = static_cast<EntityChangeType>(state.changeType);

		switch (key.entityType) {
			case Node::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<Node>(key.entityId, state.data);
				else
					replayAddOrModify<Node>(key.entityId, state.data);
				break;
			case Edge::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<Edge>(key.entityId, state.data);
				else
					replayAddOrModify<Edge>(key.entityId, state.data);
				break;
			case Property::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<Property>(key.entityId, state.data);
				else
					replayAddOrModify<Property>(key.entityId, state.data);
				break;
			case Blob::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<Blob>(key.entityId, state.data);
				else
					replayAddOrModify<Blob>(key.entityId, state.data);
				break;
			case Index::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<Index>(key.entityId, state.data);
				else
					replayAddOrModify<Index>(key.entityId, state.data);
				break;
			case State::typeId:
				if (changeType == EntityChangeType::CHANGE_DELETED)
					replayDelete<State>(key.entityId, state.data);
				else
					replayAddOrModify<State>(key.entityId, state.data);
				break;
			default:
				break; // Unknown entity type — skip
		}
	}

	template<typename T>
	void WALRecovery::replayAddOrModify(int64_t entityId, const std::vector<uint8_t> &data) {
		if (data.empty()) return;

		// Deserialize entity from WAL payload
		std::string dataStr(data.begin(), data.end());
		std::istringstream iss(dataStr, std::ios::binary);
		T entity = utils::FixedSizeSerializer::deserializeWithFixedSize<T>(iss, T::getTotalSize());

		// Ensure max-ID counter reflects this entity (crash may have lost counter advance)
		idAllocator_->ensureMaxId(T::typeId, entityId);

		// Check if entity already has a segment slot
		uint64_t segmentOffset = dataManager_->findSegmentForEntityId<T>(entityId);

		if (segmentOffset != 0) {
			// Slot exists — update in place
			writer_->updateEntityInPlace(entity, segmentOffset);
		} else {
			// No slot — allocate via saveNewEntities
			std::vector<T> entities{entity};
			writer_->saveNewEntities(entities);
		}
	}

	template<typename T>
	void WALRecovery::replayDelete(int64_t entityId, const std::vector<uint8_t> &data) {
		if (data.empty()) return;

		// Deserialize entity from WAL payload (before-image)
		std::string dataStr(data.begin(), data.end());
		std::istringstream iss(dataStr, std::ios::binary);
		T entity = utils::FixedSizeSerializer::deserializeWithFixedSize<T>(iss, T::getTotalSize());

		// Mark as inactive
		entity.markInactive();

		// Check if entity has a segment slot
		uint64_t segmentOffset = dataManager_->findSegmentForEntityId<T>(entityId);
		if (segmentOffset == 0) return; // Nothing to delete — entity was never persisted

		// Write inactive entity to disk and free ID
		writer_->updateEntityInPlace(entity, segmentOffset);
		idAllocator_->freeId(entityId, T::typeId);
	}

	// ── recover() — orchestrates all 4 phases ──────────────────────────────

	WALRecovery::RecoveryResult WALRecovery::recover() {
		RecoveryResult result;

		// Phase 1: Read WAL records
		WALReadResult walData = wal_.readRecords();

		if (walData.records.empty()) {
			// No records to replay — just checkpoint
			wal_.checkpoint();
			result.success = true;
			return result;
		}

		auto committedTxns = findCommittedTxns(walData.records);

		if (committedTxns.empty()) {
			// No committed transactions — just checkpoint (discard uncommitted)
			wal_.checkpoint();
			result.success = true;
			return result;
		}

		// Phase 2: Build deduplicated final-state map
		auto finalState = buildFinalStateMap(walData.records, committedTxns);

		if (finalState.empty()) {
			// Committed txns had no entity writes — just checkpoint
			wal_.checkpoint();
			result.success = true;
			return result;
		}

		// Phase 3: Replay each entity change
		for (const auto &[key, state] : finalState) {
			replayEntity(key, state);
		}

		// Phase 4: Finalize — persist metadata + fsync + checkpoint
		segmentTracker_->flushDirtySegments();

		auto segmentCrcs = segmentTracker_->collectSegmentCrcs();
		fileHeaderManager_->updateAggregatedCrc(segmentCrcs);
		fileHeaderManager_->flushFileHeader();

		storageIO_->sync();
		wal_.checkpoint();

		result.txnsRecovered = static_cast<uint32_t>(committedTxns.size());
		result.entitiesReplayed = static_cast<uint32_t>(finalState.size());
		result.success = true;
		return result;
	}

	// ── Explicit template instantiations ───────────────────────────────────

	template void WALRecovery::replayAddOrModify<Node>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayAddOrModify<Edge>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayAddOrModify<Property>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayAddOrModify<Blob>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayAddOrModify<Index>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayAddOrModify<State>(int64_t, const std::vector<uint8_t> &);

	template void WALRecovery::replayDelete<Node>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayDelete<Edge>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayDelete<Property>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayDelete<Blob>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayDelete<Index>(int64_t, const std::vector<uint8_t> &);
	template void WALRecovery::replayDelete<State>(int64_t, const std::vector<uint8_t> &);

} // namespace graph::storage::wal
