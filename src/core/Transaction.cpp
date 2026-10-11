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
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/StorageBackend.hpp"

#include <stdexcept>

namespace graph {

	Transaction::Transaction(uint64_t txnId, std::unique_ptr<storage::LogicalGraphTransaction> logicalTransaction,
							 std::shared_ptr<std::atomic<size_t>> activeWriteTransactionCount,
							 std::shared_ptr<storage::StorageBackend> backendLifetime) :
		txnId_(txnId), readOnly_(logicalTransaction && logicalTransaction->isReadOnly()),
		backendLifetime_(std::move(backendLifetime)), logicalTransaction_(std::move(logicalTransaction)),
		activeLogicalWriteTransactionCount_(std::move(activeWriteTransactionCount)) {
		if (!logicalTransaction_) {
			throw std::invalid_argument("logical core transaction requires a transaction");
		}
		if (!backendLifetime_) {
			throw std::invalid_argument("logical core transaction requires a backend lifetime anchor");
		}
		if (!readOnly_ && !activeLogicalWriteTransactionCount_) {
			throw std::invalid_argument("logical write transaction requires an active-transaction counter");
		}

		logicalOverlay_ = std::make_unique<storage::LogicalGraphTransactionOverlay>(*logicalTransaction_);
		logicalProjectionCatalog_ = std::make_unique<query::gds::LogicalProjectionCatalog>();
		if (!readOnly_) {
			activeLogicalWriteTransactionCount_->fetch_add(1U, std::memory_order_acq_rel);
			logicalWriteTransactionCounted_ = true;
		}
	}

	void Transaction::finishLogicalTransaction() noexcept {
		if (logicalWriteTransactionCounted_ && activeLogicalWriteTransactionCount_) {
			activeLogicalWriteTransactionCount_->fetch_sub(1U, std::memory_order_acq_rel);
			logicalWriteTransactionCounted_ = false;
		}
		// Commit/rollback releases the transaction snapshot first. The backend may
		// then close normally even if the public Transaction object remains alive.
		backendLifetime_.reset();
	}

	void Transaction::rollbackActiveLogicalTransactionNoexcept() noexcept {
		if (state_ != TxnState::TXN_ACTIVE) {
			return;
		}

		try {
			logicalTransaction_->rollback();
		} catch (...) { // ZYX_COV_EXCL_LINE: destructors and move assignment must remain noexcept on I/O failure.
			// The transaction object owns the failed overlay and is being retired.
		}
		state_ = TxnState::TXN_ROLLED_BACK;
		logicalProjectionCatalog_.reset();
		finishLogicalTransaction();
	}

	Transaction::~Transaction() {
		rollbackActiveLogicalTransactionNoexcept();
	}

	Transaction::Transaction(Transaction &&other) noexcept :
		txnId_(other.txnId_), state_(other.state_), readOnly_(other.readOnly_), backendLifetime_(std::move(other.backendLifetime_)),
		logicalTransaction_(std::move(other.logicalTransaction_)),
		logicalOverlay_(std::move(other.logicalOverlay_)),
		logicalProjectionCatalog_(std::move(other.logicalProjectionCatalog_)),
		activeLogicalWriteTransactionCount_(std::move(other.activeLogicalWriteTransactionCount_)),
		logicalWriteTransactionCounted_(other.logicalWriteTransactionCounted_) {
		other.state_ = TxnState::TXN_ROLLED_BACK; // Prevent other's destructor from rolling back
		other.logicalWriteTransactionCounted_ = false;
	}

	Transaction &Transaction::operator=(Transaction &&other) noexcept {
		if (this != &other) {
			rollbackActiveLogicalTransactionNoexcept();

			txnId_ = other.txnId_;
			state_ = other.state_;
			readOnly_ = other.readOnly_;
			backendLifetime_ = std::move(other.backendLifetime_);
			logicalTransaction_ = std::move(other.logicalTransaction_);
			logicalOverlay_ = std::move(other.logicalOverlay_);
			logicalProjectionCatalog_ = std::move(other.logicalProjectionCatalog_);
			activeLogicalWriteTransactionCount_ = std::move(other.activeLogicalWriteTransactionCount_);
			logicalWriteTransactionCounted_ = other.logicalWriteTransactionCounted_;

			other.state_ = TxnState::TXN_ROLLED_BACK;
			other.logicalWriteTransactionCounted_ = false;
		}
		return *this;
	}

	void Transaction::commit() {
		if (state_ != TxnState::TXN_ACTIVE) {
			throw std::runtime_error("Cannot commit: transaction is not active");
		}
		try {
			static_cast<void>(logicalTransaction_->commit());
			state_ = TxnState::TXN_COMMITTED;
			logicalProjectionCatalog_.reset();
			finishLogicalTransaction();
		} catch (...) {
			if (logicalTransaction_->state() != storage::LogicalGraphTransactionState::LGTTS_ACTIVE) {
				state_ = TxnState::TXN_ROLLED_BACK;
				logicalProjectionCatalog_.reset();
				finishLogicalTransaction();
			}
			throw;
		}
	}

	void Transaction::rollback() {
		if (state_ != TxnState::TXN_ACTIVE) {
			throw std::runtime_error("Cannot rollback: transaction is not active");
		}
		try {
			logicalTransaction_->rollback();
			state_ = TxnState::TXN_ROLLED_BACK;
			logicalProjectionCatalog_.reset();
			finishLogicalTransaction();
		} catch (...) {
			if (logicalTransaction_->state() != storage::LogicalGraphTransactionState::LGTTS_ACTIVE) {
				state_ = TxnState::TXN_ROLLED_BACK;
				logicalProjectionCatalog_.reset();
				finishLogicalTransaction();
			}
			throw;
		}
	}

	query::QueryResult Transaction::execute(query::QueryEngine &engine, const std::string &query) {
		if (state_ != TxnState::TXN_ACTIVE) {
			throw std::runtime_error("Cannot execute: transaction is not active");
		}
		const auto transactionBackend = std::dynamic_pointer_cast<storage::LogicalGraphReadBackend>(backendLifetime_);
		if (!transactionBackend || !engine.usesLogicalReadBackend(*transactionBackend)) {
			throw std::invalid_argument("transaction and query engine belong to different logical backends");
		}
		query::QueryContext context;
		if (readOnly_) {
			context.execMode = query::ExecMode::EM_READ_ONLY;
		}
		if (!logicalOverlay_) {
			throw std::logic_error("logical transaction has no execution overlay");
		}
		return engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan(query), context, *logicalOverlay_, *logicalOverlay_, logicalProjectionCatalog_.get());
	}

} // namespace graph
