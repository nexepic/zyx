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
#include <vector>
#include "graph/core/Transaction.hpp"

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

	private:
		bool transactionActive_ = false;
		uint64_t activeTxnId_ = 0;
		std::vector<Transaction::TxnOperation> txnOps_;
		wal::WALManager *walManager_ = nullptr;
	};

} // namespace graph::storage
