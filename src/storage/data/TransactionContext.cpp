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
#include <array>
#include <span>
#include <stdexcept>
#include <utility>
#include "graph/core/Edge.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/wal/WALManager.hpp"

namespace graph::storage {
	namespace {
		template<typename EntityType>
		std::vector<uint8_t> serializeEntityBytes(const EntityType &entity) {
			std::vector<uint8_t> bytes(EntityType::getTotalSize());
			utils::FixedSizeSerializer::serializeInto(reinterpret_cast<char *>(bytes.data()), entity, bytes.size());
			return bytes;
		}

		template<typename EntityType>
		void writeEntityChangesBatch(wal::WALManager &walManager,
									 uint64_t txnId,
									 uint8_t changeType,
									 const std::vector<EntityType> &entities) {
			constexpr size_t kWalBatchSize = 4096;
			constexpr size_t kEntitySize = EntityType::getTotalSize();
			std::vector<uint8_t> serialized;
			std::vector<wal::WALEntityChangeView> views;
			const uint8_t entityType = static_cast<uint8_t>(EntityType::typeId);

			for (size_t offset = 0; offset < entities.size(); offset += kWalBatchSize) {
				const size_t batchSize = (std::min)(kWalBatchSize, entities.size() - offset);
				serialized.resize(batchSize * kEntitySize);
				views.clear();
				views.reserve(batchSize);

				for (size_t i = 0; i < batchSize; ++i) {
					const auto &entity = entities[offset + i];
					auto *dest = serialized.data() + i * kEntitySize;
					utils::FixedSizeSerializer::serializeInto(reinterpret_cast<char *>(dest), entity, kEntitySize);
					views.push_back({entityType, changeType, entity.getId(), dest, static_cast<uint32_t>(kEntitySize)});
				}

				walManager.writeEntityChangeViews(txnId, views);
			}
		}

	} // namespace

	uint64_t TransactionContext::makeEntityKey(uint8_t entityType, int64_t entityId) {
		return (static_cast<uint64_t>(entityType) << 56U) | (static_cast<uint64_t>(entityId) & 0x00FFFFFFFFFFFFFFULL);
	}

	bool TransactionContext::rememberAddedEntity(uint8_t entityType, int64_t entityId) {
		if (entityId <= 0) {
			return false;
		}
		return addedEntityKeys_.insert(makeEntityKey(entityType, entityId)).second;
	}

	void TransactionContext::forgetAddedEntity(uint8_t entityType, int64_t entityId) {
		if (entityId > 0) {
			addedEntityKeys_.erase(makeEntityKey(entityType, entityId));
		}
	}

	bool TransactionContext::wasEntityAddedInActiveTransaction(uint8_t entityType, int64_t entityId) const {
		return entityId > 0 && addedEntityKeys_.contains(makeEntityKey(entityType, entityId));
	}

	void TransactionContext::reserveAddedEntityRecords(uint8_t entityType, size_t additionalCapacity) {
		if (entityType >= pendingWalAddsByType_.size() || additionalCapacity == 0) {
			return;
		}
		auto &bucket = pendingWalAddsByType_[entityType];
		bucket.reserve(bucket.size() + additionalCapacity);
	}

	void TransactionContext::stageWalAdd(uint8_t entityType, int64_t entityId) {
		if (entityId <= 0 || entityType >= pendingWalAddsByType_.size()) {
			return;
		}
		pendingWalAddsByType_[entityType].push_back(entityId);
	}

	void TransactionContext::markWalAddCanceled(uint8_t entityType) {
		if (entityType < pendingWalAddCanceledByType_.size()) {
			pendingWalAddCanceledByType_[entityType] = true;
		}
	}

	void TransactionContext::stageWalChange(uint8_t entityType, EntityChangeType changeType, int64_t entityId) {
		if (entityId <= 0) {
			return;
		}
		pendingWalChanges_[makeEntityKey(entityType, entityId)] = PendingWalChange{entityType, changeType, entityId, {}};
	}

	void TransactionContext::stageWalChange(uint8_t entityType,
											EntityChangeType changeType,
											int64_t entityId,
											std::vector<uint8_t> serializedData) {
		if (entityId <= 0) {
			return;
		}
		pendingWalChanges_[makeEntityKey(entityType, entityId)] =
				PendingWalChange{entityType, changeType, entityId, std::move(serializedData)};
	}

	void TransactionContext::eraseStagedWalChange(uint8_t entityType, int64_t entityId) {
		if (entityId > 0) {
			pendingWalChanges_.erase(makeEntityKey(entityType, entityId));
		}
	}

	void TransactionContext::setActive(uint64_t txnId) {
		transactionActive_ = true;
		activeTxnId_ = txnId;
		txnOps_.clear();
		addedEntityKeys_.clear();
		for (auto &bucket: pendingWalAddsByType_) {
			bucket.clear();
		}
		pendingWalAddCanceledByType_.fill(false);
		pendingWalChanges_.clear();
	}

	void TransactionContext::clear() {
		transactionActive_ = false;
		activeTxnId_ = 0;
		txnOps_.clear();
		addedEntityKeys_.clear();
		for (auto &bucket: pendingWalAddsByType_) {
			bucket.clear();
		}
		pendingWalAddCanceledByType_.fill(false);
		pendingWalChanges_.clear();
		undoLog_.clear();
		rollbackBase_.reset();
	}

	void TransactionContext::setRollbackBase(std::shared_ptr<CommittedSnapshot> snapshot) {
		rollbackBase_ = std::move(snapshot);
	}

	void TransactionContext::recordOp(Transaction::TxnOperation op) {
		txnOps_.push_back(op);
	}

	template<typename EntityType>
	void TransactionContext::recordAdd(const EntityType &entity) {
		if (!transactionActive_) return;

		const auto entityType = static_cast<uint8_t>(EntityType::typeId);
		recordOp({Transaction::TxnOperation::OP_ADD,
				  entityType, entity.getId()});
		const bool firstAdd = rememberAddedEntity(entityType, entity.getId());
		undoLog_.record({entityType, entity.getId(),
						 wal::UndoChangeType::UNDO_ADDED, {}});
		if (firstAdd) {
			stageWalAdd(entityType, entity.getId());
		}
	}

	template<typename EntityType>
	void TransactionContext::recordAdds(const std::vector<EntityType> &entities) {
		if (!transactionActive_ || entities.empty()) return;

		txnOps_.reserve(txnOps_.size() + entities.size());
		addedEntityKeys_.reserve(addedEntityKeys_.size() + entities.size());
		reserveAddedEntityRecords(static_cast<uint8_t>(EntityType::typeId), entities.size());
		undoLog_.reserve(undoLog_.size() + entities.size());
		const auto entityType = static_cast<uint8_t>(EntityType::typeId);

		for (size_t index = 0; index < entities.size(); ++index) {
			const auto &entity = entities[index];
			recordOp({Transaction::TxnOperation::OP_ADD,
					  entityType, entity.getId()});
			const bool firstAdd = rememberAddedEntity(entityType, entity.getId());
			undoLog_.record({entityType, entity.getId(),
							 wal::UndoChangeType::UNDO_ADDED, {}});
			if (firstAdd) {
				stageWalAdd(entityType, entity.getId());
			}
		}
	}

	template<typename EntityType>
	void TransactionContext::recordUpdate(const EntityType &newEntity, const EntityType &oldEntity) {
		if (!transactionActive_) return;

		const auto entityType = static_cast<uint8_t>(EntityType::typeId);
		const bool addedInTxn = wasEntityAddedInActiveTransaction(entityType, newEntity.getId());
		if (addedInTxn) {
			return;
		}

		recordOp({Transaction::TxnOperation::OP_UPDATE, entityType, newEntity.getId()});
		undoLog_.record({entityType, newEntity.getId(),
						 wal::UndoChangeType::UNDO_MODIFIED, serializeEntityBytes(oldEntity)});
		stageWalChange(entityType, EntityChangeType::CHANGE_MODIFIED, newEntity.getId());
	}

	template<typename EntityType>
	void TransactionContext::recordUpdates(
			const std::vector<EntityType> &newEntities,
			const std::vector<EntityType> &oldEntities) {
		if (!transactionActive_ || newEntities.empty()) return;
		if (newEntities.size() != oldEntities.size()) {
			throw std::invalid_argument("recordUpdates requires matching new/old entity counts");
		}

		txnOps_.reserve(txnOps_.size() + newEntities.size());
		pendingWalChanges_.reserve(pendingWalChanges_.size() + newEntities.size());
		undoLog_.reserve(undoLog_.size() + newEntities.size());
		const auto entityType = static_cast<uint8_t>(EntityType::typeId);

		for (size_t index = 0; index < newEntities.size(); ++index) {
			const auto &newEntity = newEntities[index];
			const auto &oldEntity = oldEntities[index];
			const bool addedInTxn = wasEntityAddedInActiveTransaction(entityType, newEntity.getId());
			if (addedInTxn) {
				continue;
			}

			recordOp({Transaction::TxnOperation::OP_UPDATE, entityType, newEntity.getId()});
			undoLog_.record({entityType, newEntity.getId(),
							 wal::UndoChangeType::UNDO_MODIFIED, serializeEntityBytes(oldEntity)});
			stageWalChange(entityType, EntityChangeType::CHANGE_MODIFIED, newEntity.getId());
		}
	}

	template<typename EntityType>
	void TransactionContext::recordDelete(int64_t id, std::function<EntityType(int64_t)> getOld) {
		if (!transactionActive_) return;

		const auto entityType = static_cast<uint8_t>(EntityType::typeId);
		if (wasEntityAddedInActiveTransaction(entityType, id)) {
			forgetAddedEntity(entityType, id);
			markWalAddCanceled(entityType);
			eraseStagedWalChange(entityType, id);
			return;
		}

		recordOp({Transaction::TxnOperation::OP_DELETE,
				  entityType, id});
		EntityType oldEntity = getOld(id);
		auto buf = serializeEntityBytes(oldEntity);
		undoLog_.record({entityType, id,
						 wal::UndoChangeType::UNDO_DELETED, buf});
		stageWalChange(entityType, EntityChangeType::CHANGE_DELETED, id, std::move(buf));
	}

	template<typename EntityType>
	void TransactionContext::flushWalEntities(EntityChangeType changeType, const std::vector<EntityType> &entities) const {
		if (!transactionActive_ || !walManager_ || entities.empty()) {
			return;
		}
		writeEntityChangesBatch(*walManager_, activeTxnId_, static_cast<uint8_t>(changeType), entities);
	}

	void TransactionContext::flushWalChangeViews(std::span<const wal::WALEntityChangeView> changes) const {
		if (!transactionActive_ || !walManager_ || changes.empty()) {
			return;
		}
		walManager_->writeEntityChangeViews(activeTxnId_, changes);
	}

	void TransactionContext::flushSerializedWalChange(const PendingWalChange &change) const {
		if (!transactionActive_ || !walManager_ || change.serializedData.empty()) {
			return;
		}
		walManager_->writeEntityChange(activeTxnId_, change.entityType, static_cast<uint8_t>(change.changeType),
									   change.entityId, change.serializedData);
	}

	bool TransactionContext::hasPendingWalRecords() const {
		if (!pendingWalChanges_.empty()) {
			return true;
		}
		for (const auto &bucket: pendingWalAddsByType_) {
			if (!bucket.empty()) {
				return true;
			}
		}
		return false;
	}

	bool TransactionContext::hasCanceledPendingWalAdds(uint8_t entityType) const {
		return entityType < pendingWalAddCanceledByType_.size() && pendingWalAddCanceledByType_[entityType];
	}

	// Explicit instantiations
	template void TransactionContext::recordAdd<Node>(const Node &);
	template void TransactionContext::recordAdd<Edge>(const Edge &);
	template void TransactionContext::recordAdd<Property>(const Property &);
	template void TransactionContext::recordAdd<Blob>(const Blob &);
	template void TransactionContext::recordAdd<Index>(const Index &);
	template void TransactionContext::recordAdd<State>(const State &);
	template void TransactionContext::recordAdds<Node>(const std::vector<Node> &);
	template void TransactionContext::recordAdds<Edge>(const std::vector<Edge> &);
	template void TransactionContext::recordAdds<Property>(const std::vector<Property> &);
	template void TransactionContext::recordAdds<Blob>(const std::vector<Blob> &);
	template void TransactionContext::recordAdds<Index>(const std::vector<Index> &);
	template void TransactionContext::recordAdds<State>(const std::vector<State> &);

	template void TransactionContext::recordUpdate<Node>(const Node &, const Node &);
	template void TransactionContext::recordUpdate<Edge>(const Edge &, const Edge &);
	template void TransactionContext::recordUpdate<Property>(const Property &, const Property &);
	template void TransactionContext::recordUpdate<Blob>(const Blob &, const Blob &);
	template void TransactionContext::recordUpdate<Index>(const Index &, const Index &);
	template void TransactionContext::recordUpdate<State>(const State &, const State &);
	template void TransactionContext::recordUpdates<Node>(const std::vector<Node> &, const std::vector<Node> &);
	template void TransactionContext::recordUpdates<Edge>(const std::vector<Edge> &, const std::vector<Edge> &);
	template void TransactionContext::recordUpdates<Property>(
			const std::vector<Property> &, const std::vector<Property> &);
	template void TransactionContext::recordUpdates<Blob>(const std::vector<Blob> &, const std::vector<Blob> &);
	template void TransactionContext::recordUpdates<Index>(const std::vector<Index> &, const std::vector<Index> &);
	template void TransactionContext::recordUpdates<State>(const std::vector<State> &, const std::vector<State> &);

	template void TransactionContext::recordDelete<Node>(int64_t, std::function<Node(int64_t)>);
	template void TransactionContext::recordDelete<Edge>(int64_t, std::function<Edge(int64_t)>);
	template void TransactionContext::recordDelete<Property>(int64_t, std::function<Property(int64_t)>);
	template void TransactionContext::recordDelete<Blob>(int64_t, std::function<Blob(int64_t)>);
	template void TransactionContext::recordDelete<Index>(int64_t, std::function<Index(int64_t)>);
	template void TransactionContext::recordDelete<State>(int64_t, std::function<State(int64_t)>);

	template void TransactionContext::flushWalEntities<Node>(EntityChangeType, const std::vector<Node> &) const;
	template void TransactionContext::flushWalEntities<Edge>(EntityChangeType, const std::vector<Edge> &) const;
	template void TransactionContext::flushWalEntities<Property>(EntityChangeType, const std::vector<Property> &) const;
	template void TransactionContext::flushWalEntities<Blob>(EntityChangeType, const std::vector<Blob> &) const;
	template void TransactionContext::flushWalEntities<Index>(EntityChangeType, const std::vector<Index> &) const;
	template void TransactionContext::flushWalEntities<State>(EntityChangeType, const std::vector<State> &) const;

} // namespace graph::storage
