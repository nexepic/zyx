/**
 * @file WALRecovery.hpp
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

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>
#include "WALManager.hpp"
#include "WALRecord.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::storage {
	class StorageWriter;
	class SegmentTracker;
	class StorageIO;
	class DataManager;
	class FileHeaderManager;
} // namespace graph::storage

namespace graph::storage::wal {

	/**
	 * WAL crash recovery: replays committed transactions from WAL into the DB file.
	 *
	 * Executes a 4-phase algorithm:
	 *   1. Scan — read WAL records, identify committed transactions
	 *   2. Deduplicate — build last-writer-wins final state per entity
	 *   3. Replay — write entities to DB file via StorageWriter
	 *   4. Finalize — persist segment headers, file header, fsync, checkpoint WAL
	 *
	 * Idempotent: safe to run multiple times on the same WAL (e.g. crash during replay).
	 */
	class WALRecovery {
	public:
		struct RecoveryResult {
			uint32_t txnsRecovered = 0;
			uint32_t entitiesReplayed = 0;
			bool success = false;
		};

		WALRecovery(WALManager &wal,
					std::shared_ptr<StorageWriter> writer,
					IDAllocators allocators,
					std::shared_ptr<SegmentTracker> segmentTracker,
					std::shared_ptr<StorageIO> storageIO,
					std::shared_ptr<DataManager> dataManager,
					std::shared_ptr<FileHeaderManager> fileHeaderManager);

		RecoveryResult recover();

	private:
		struct EntityKey {
			uint8_t entityType;
			int64_t entityId;

			bool operator<(const EntityKey &other) const {
				if (entityType != other.entityType) return entityType < other.entityType;
				return entityId < other.entityId;
			}
		};

		struct EntityState {
			uint8_t changeType;
			std::vector<uint8_t> data;
		};

		std::unordered_set<uint64_t> findCommittedTxns(const std::vector<WALReadRecord> &records);

		std::map<EntityKey, EntityState> buildFinalStateMap(
				const std::vector<WALReadRecord> &records,
				const std::unordered_set<uint64_t> &committedTxns);

		void replayEntity(const EntityKey &key, const EntityState &state);

		template<typename T>
		void replayAddOrModify(int64_t entityId, const std::vector<uint8_t> &data);

		template<typename T>
		void replayDelete(int64_t entityId, const std::vector<uint8_t> &data);

		WALManager &wal_;
		std::shared_ptr<StorageWriter> writer_;
		IDAllocators allocators_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		std::shared_ptr<StorageIO> storageIO_;
		std::shared_ptr<DataManager> dataManager_;
		std::shared_ptr<FileHeaderManager> fileHeaderManager_;
	};

} // namespace graph::storage::wal
