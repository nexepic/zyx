/**
 * @file Transaction.cpp
 * @author Nexepic
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include "graph/core/Transaction.hpp"
#include "graph/core/TransactionManager.hpp"

namespace graph {

	Transaction::Transaction(uint64_t txnId, TransactionManager &mgr, std::shared_ptr<storage::FileStorage> storage) :
		txnId_(txnId), manager_(&mgr), storage_(std::move(storage)) {}

	Transaction::~Transaction() {
		if (state_ == TxnState::TXN_ACTIVE && manager_) {
			try {
				manager_->rollbackTransaction(*this);
			} catch (...) {
				// Suppress exceptions in destructor
			}
		}
	}

	Transaction::Transaction(Transaction &&other) noexcept :
		txnId_(other.txnId_), state_(other.state_), manager_(other.manager_), storage_(std::move(other.storage_)),
		operations_(std::move(other.operations_)) {
		other.state_ = TxnState::TXN_ROLLED_BACK; // Prevent other's destructor from rolling back
		other.manager_ = nullptr;
	}

	Transaction &Transaction::operator=(Transaction &&other) noexcept {
		if (this != &other) {
			// If this transaction is still active, roll it back
			if (state_ == TxnState::TXN_ACTIVE && manager_) {
				try {
					manager_->rollbackTransaction(*this);
				} catch (...) {
				}
			}

			txnId_ = other.txnId_;
			state_ = other.state_;
			manager_ = other.manager_;
			storage_ = std::move(other.storage_);
			operations_ = std::move(other.operations_);

			other.state_ = TxnState::TXN_ROLLED_BACK;
			other.manager_ = nullptr;
		}
		return *this;
	}

	void Transaction::commit() {
		if (state_ != TxnState::TXN_ACTIVE) {
			throw std::runtime_error("Cannot commit: transaction is not active");
		}
		manager_->commitTransaction(*this);
	}

	void Transaction::rollback() {
		if (state_ != TxnState::TXN_ACTIVE) {
			throw std::runtime_error("Cannot rollback: transaction is not active");
		}
		manager_->rollbackTransaction(*this);
	}

	void Transaction::recordOperation(TxnOperation op) { operations_.push_back(op); }

} // namespace graph
