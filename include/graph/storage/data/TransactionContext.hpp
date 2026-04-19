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
#include <vector>
#include "graph/core/Transaction.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/storage/wal/UndoLog.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {

	namespace wal {
		class WALManager;
	}

	/**
	 * @brief Manages transaction state, op journaling, and WAL integration.
	 *
	 * Extracted from DataManager to isolate transaction bookkeeping
	 * into a focused, independently testable unit.
	 */
	class TransactionContext {
	public:
		void setActive(uint64_t txnId);
		void clear();

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
		void recordUpdate(const EntityType &newEntity, const EntityType &oldEntity);

		template<typename EntityType>
		void recordDelete(int64_t id, std::function<EntityType(int64_t)> getOld);

	private:
		bool transactionActive_ = false;
		uint64_t activeTxnId_ = 0;
		std::vector<Transaction::TxnOperation> txnOps_;
		wal::WALManager *walManager_ = nullptr;
		wal::UndoLog undoLog_;
	};

} // namespace graph::storage
