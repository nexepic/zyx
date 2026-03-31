/**
 * @file TransactionContext.cpp
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

#include "graph/storage/data/TransactionContext.hpp"

namespace graph::storage {

	void TransactionContext::setActive(uint64_t txnId) {
		transactionActive_ = true;
		activeTxnId_ = txnId;
		txnOps_.clear();
	}

	void TransactionContext::clear() {
		transactionActive_ = false;
		activeTxnId_ = 0;
		txnOps_.clear();
	}

	void TransactionContext::recordOp(Transaction::TxnOperation op) {
		txnOps_.push_back(op);
	}

} // namespace graph::storage
