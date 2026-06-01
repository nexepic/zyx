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
#include <algorithm>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph::storage {
	namespace {
		template<typename EntityType>
		std::vector<uint8_t> serializeEntityBytes(const EntityType &entity) {
			std::vector<uint8_t> bytes(EntityType::getTotalSize());
			utils::FixedSizeSerializer::serializeInto(reinterpret_cast<char *>(bytes.data()), entity, bytes.size());
			return bytes;
		}
	} // namespace

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
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_ADDED), entity.getId(),
										   serializeEntityBytes(entity));
		}
	}

	template<typename EntityType>
	void TransactionContext::recordAdds(const std::vector<EntityType> &entities) {
		if (!transactionActive_ || entities.empty()) return;

		constexpr size_t kWalBatchSize = 4096;
		txnOps_.reserve(txnOps_.size() + entities.size());

		std::vector<wal::WALEntityChange> walChanges;
		if (walManager_) {
			walChanges.reserve((std::min)(entities.size(), kWalBatchSize));
		}

		auto flushWalChanges = [&]() {
			if (walManager_ && !walChanges.empty()) {
				walManager_->writeEntityChanges(activeTxnId_, walChanges);
				walChanges.clear();
			}
		};

		for (const auto &entity : entities) {
			recordOp({Transaction::TxnOperation::OP_ADD,
					  static_cast<uint8_t>(EntityType::typeId), entity.getId()});
			undoLog_.record({static_cast<uint8_t>(EntityType::typeId), entity.getId(),
							 wal::UndoChangeType::UNDO_ADDED, {}});
			if (walManager_) {
				walChanges.push_back({static_cast<uint8_t>(EntityType::typeId),
									  static_cast<uint8_t>(EntityChangeType::CHANGE_ADDED),
									  entity.getId(),
									  serializeEntityBytes(entity)});
				if (walChanges.size() >= kWalBatchSize) {
					flushWalChanges();
				}
			}
		}
		flushWalChanges();
	}

	template<typename EntityType>
	void TransactionContext::recordUpdate(const EntityType &newEntity, const EntityType &oldEntity) {
		if (!transactionActive_) return;

		recordOp({Transaction::TxnOperation::OP_UPDATE,
				  static_cast<uint8_t>(EntityType::typeId), newEntity.getId()});
		undoLog_.record({static_cast<uint8_t>(EntityType::typeId), newEntity.getId(),
						 wal::UndoChangeType::UNDO_MODIFIED, serializeEntityBytes(oldEntity)});
		if (walManager_) {
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_MODIFIED), newEntity.getId(),
										   serializeEntityBytes(newEntity));
		}
	}

	template<typename EntityType>
	void TransactionContext::recordDelete(int64_t id, std::function<EntityType(int64_t)> getOld) {
		if (!transactionActive_) return;

		recordOp({Transaction::TxnOperation::OP_DELETE,
				  static_cast<uint8_t>(EntityType::typeId), id});
		EntityType oldEntity = getOld(id);
		auto buf = serializeEntityBytes(oldEntity);
		undoLog_.record({static_cast<uint8_t>(EntityType::typeId), id,
						 wal::UndoChangeType::UNDO_DELETED, buf});
		if (walManager_) {
			walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::typeId),
										   static_cast<uint8_t>(EntityChangeType::CHANGE_DELETED), id,
										   buf);
		}
	}

	// Explicit instantiations
	template void TransactionContext::recordAdd<Node>(const Node &);
	template void TransactionContext::recordAdd<Edge>(const Edge &);
	template void TransactionContext::recordAdds<Node>(const std::vector<Node> &);
	template void TransactionContext::recordAdds<Edge>(const std::vector<Edge> &);

	template void TransactionContext::recordUpdate<Node>(const Node &, const Node &);
	template void TransactionContext::recordUpdate<Edge>(const Edge &, const Edge &);

	template void TransactionContext::recordDelete<Node>(int64_t, std::function<Node(int64_t)>);
	template void TransactionContext::recordDelete<Edge>(int64_t, std::function<Edge(int64_t)>);

} // namespace graph::storage
