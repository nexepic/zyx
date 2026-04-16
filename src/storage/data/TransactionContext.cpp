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
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/wal/WALManager.hpp"

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
		undoLog_.clear();
	}

	void TransactionContext::recordOp(Transaction::TxnOperation op) {
		txnOps_.push_back(op);
	}

	template<typename EntityType>
	void TransactionContext::recordAdd(const EntityType &entity) {
		if (!transactionActive_) return;

		recordOp({Transaction::TxnOperation::OP_ADD,
				  static_cast<uint8_t>(EntityType::typeId), entity.getId()});
		undoLog_.record({static_cast<uint8_t>(EntityType::typeId), entity.getId(),
						 wal::UndoChangeType::UNDO_ADDED, {}});
		if (walManager_) {
			auto walBuf = utils::FixedSizeSerializer::serializeToBuffer(entity, EntityType::getTotalSize());
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_ADDED), entity.getId(),
										   {walBuf.begin(), walBuf.end()});
		}
	}

	template<typename EntityType>
	void TransactionContext::recordUpdate(const EntityType &newEntity, const EntityType &oldEntity) {
		if (!transactionActive_) return;

		recordOp({Transaction::TxnOperation::OP_UPDATE,
				  static_cast<uint8_t>(EntityType::typeId), newEntity.getId()});
		auto buf = utils::FixedSizeSerializer::serializeToBuffer(oldEntity, EntityType::getTotalSize());
		undoLog_.record({static_cast<uint8_t>(EntityType::typeId), newEntity.getId(),
						 wal::UndoChangeType::UNDO_MODIFIED, {buf.begin(), buf.end()}});
		if (walManager_) {
			auto walBuf = utils::FixedSizeSerializer::serializeToBuffer(newEntity, EntityType::getTotalSize());
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_MODIFIED), newEntity.getId(),
										   {walBuf.begin(), walBuf.end()});
		}
	}

	template<typename EntityType>
	void TransactionContext::recordDelete(int64_t id, std::function<EntityType(int64_t)> getOld) {
		if (!transactionActive_) return;

		recordOp({Transaction::TxnOperation::OP_DELETE,
				  static_cast<uint8_t>(EntityType::typeId), id});
		EntityType oldEntity = getOld(id);
		auto buf = utils::FixedSizeSerializer::serializeToBuffer(oldEntity, EntityType::getTotalSize());
		undoLog_.record({static_cast<uint8_t>(EntityType::typeId), id,
						 wal::UndoChangeType::UNDO_DELETED, {buf.begin(), buf.end()}});
		if (walManager_) {
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_DELETED), id,
										   {buf.begin(), buf.end()});
		}
	}

	// Explicit instantiations
	template void TransactionContext::recordAdd<Node>(const Node &);
	template void TransactionContext::recordAdd<Edge>(const Edge &);

	template void TransactionContext::recordUpdate<Node>(const Node &, const Node &);
	template void TransactionContext::recordUpdate<Edge>(const Edge &, const Edge &);

	template void TransactionContext::recordDelete<Node>(int64_t, std::function<Node(int64_t)>);
	template void TransactionContext::recordDelete<Edge>(int64_t, std::function<Edge(int64_t)>);

} // namespace graph::storage
