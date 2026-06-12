/**
 * @file TransactionContext.hpp
 * @date 2026/3/31
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
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "graph/core/Transaction.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/storage/wal/UndoLog.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {
	struct CommittedSnapshot;

	namespace wal {
		class WALManager;
		struct WALEntityChangeView;
	}

	/**
	 * @brief Manages transaction state, op journaling, and WAL integration.
	 *
	 * Extracted from DataManager to isolate transaction bookkeeping
	 * into a focused, independently testable unit.
	 */
	class TransactionContext {
	public:
		struct PendingWalChange {
			uint8_t entityType = 0;
			EntityChangeType changeType = EntityChangeType::CHANGE_MODIFIED;
			int64_t entityId = 0;
			std::vector<uint8_t> serializedData;
		};

		using PendingWalChangeMap = std::unordered_map<uint64_t, PendingWalChange>;

		void setActive(uint64_t txnId);
		void clear();
		void setRollbackBase(std::shared_ptr<CommittedSnapshot> snapshot);
		[[nodiscard]] const std::shared_ptr<CommittedSnapshot> &rollbackBase() const { return rollbackBase_; }

		[[nodiscard]] bool isActive() const { return transactionActive_; }
		[[nodiscard]] uint64_t activeTxnId() const { return activeTxnId_; }
		[[nodiscard]] const std::vector<Transaction::TxnOperation> &getOps() const { return txnOps_; }

		void recordOp(Transaction::TxnOperation op);

		void setWALManager(wal::WALManager *wal) { walManager_ = wal; }
		[[nodiscard]] wal::WALManager *getWALManager() const { return walManager_; }

		// readOnly_ removed: was a plain bool shared across concurrent readers,
		// causing a data race. Read-only guard is now thread_local in DataManager.

		[[nodiscard]] wal::UndoLog &undoLog() { return undoLog_; }
		[[nodiscard]] const wal::UndoLog &undoLog() const { return undoLog_; }

		// --- Template transaction recording methods ---
		// Encapsulate recordOp + undoLog.record + optional WAL write for each mutation type.

		template<typename EntityType>
		void recordAdd(const EntityType &entity);

		template<typename EntityType>
		void recordAdds(const std::vector<EntityType> &entities);

		template<typename EntityType>
		void recordUpdate(const EntityType &newEntity, const EntityType &oldEntity);

		template<typename EntityType>
		void recordUpdates(const std::vector<EntityType> &newEntities, const std::vector<EntityType> &oldEntities);

		template<typename EntityType>
		void recordDelete(int64_t id, std::function<EntityType(int64_t)> getOld);

		template<typename EntityType>
		void flushWalEntities(EntityChangeType changeType, const std::vector<EntityType> &entities) const;

		void flushWalChangeViews(std::span<const wal::WALEntityChangeView> changes) const;
		void flushSerializedWalChange(const PendingWalChange &change) const;
		[[nodiscard]] const PendingWalChangeMap &pendingWalChanges() const { return pendingWalChanges_; }
		[[nodiscard]] bool wasEntityAddedInActiveTransaction(uint8_t entityType, int64_t entityId) const;

	private:
		[[nodiscard]] static uint64_t makeEntityKey(uint8_t entityType, int64_t entityId);
		void rememberAddedEntity(uint8_t entityType, int64_t entityId);
		void forgetAddedEntity(uint8_t entityType, int64_t entityId);
		void stageWalChange(uint8_t entityType, EntityChangeType changeType, int64_t entityId);
		void stageWalChange(uint8_t entityType, EntityChangeType changeType, int64_t entityId,
							std::vector<uint8_t> serializedData);
		void eraseStagedWalChange(uint8_t entityType, int64_t entityId);

		bool transactionActive_ = false;
		uint64_t activeTxnId_ = 0;
		std::vector<Transaction::TxnOperation> txnOps_;
		std::unordered_set<uint64_t> addedEntityKeys_;
		PendingWalChangeMap pendingWalChanges_;
		wal::WALManager *walManager_ = nullptr;
		wal::UndoLog undoLog_;
		std::shared_ptr<CommittedSnapshot> rollbackBase_;
	};

} // namespace graph::storage
