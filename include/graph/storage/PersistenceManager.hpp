/**
 * @file PersistenceManager.hpp
 * @author Nexepic
 * @date 2025/12/1
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

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include "DirtyEntityRegistry.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"

namespace graph::storage {

	struct CommittedSnapshot;

	// Snapshot structure to hold all data being flushed
	struct FlushSnapshot {
		DirtyEntityRegistry<Node>::DirtyMap nodes;
		DirtyEntityRegistry<Edge>::DirtyMap edges;
		DirtyEntityRegistry<Property>::DirtyMap properties;
		DirtyEntityRegistry<Blob>::DirtyMap blobs;
		DirtyEntityRegistry<Index>::DirtyMap indexes;
		DirtyEntityRegistry<State>::DirtyMap states;

		[[nodiscard]] bool isEmpty() const {
			return nodes.empty() && edges.empty() && properties.empty() && blobs.empty() && indexes.empty() &&
				   states.empty();
		}
	};

	class PersistenceManager {
	public:
		PersistenceManager();
		~PersistenceManager() = default;

		// --- Core CRUD ---
		template<typename EntityType>
		void upsert(const DirtyEntityInfo<EntityType> &info);

		template<typename EntityType>
		void upsertBatch(const std::vector<EntityType> &entities, EntityChangeType changeType);

		template<typename EntityType>
		void remove(int64_t id);

		template<typename EntityType>
		std::optional<DirtyEntityInfo<EntityType>> getDirtyInfo(int64_t id) const;

		template<typename EntityType>
		std::vector<DirtyEntityInfo<EntityType>> getAllDirtyInfos() const;

		template<typename EntityType>
		bool isDirty(int64_t id) const;

		// --- Transaction/Flush ---
		FlushSnapshot createSnapshot() const;
		void commitSnapshot() const;
		[[nodiscard]] bool hasUnsavedChanges() const;
		void clearAll() const;

		/**
		 * @brief Captures current dirty state as a CommittedSnapshot for read isolation.
		 * Copies (not moves) data from all registries so the flush path is unaffected.
		 */
		[[nodiscard]] std::shared_ptr<CommittedSnapshot> captureCommittedSnapshot() const;

		// --- Auto Flush ---
		void setMaxDirtyEntities(size_t max) { maxDirtyEntities_ = max; }
		void setAutoFlushCallback(std::function<void()> callback) { autoFlushCallback_ = std::move(callback); }
		void checkAndTriggerAutoFlush() const;

		// --- Transaction State ---
		void setTransactionActive(bool active) { transactionActive_.store(active, std::memory_order_release); }
		[[nodiscard]] bool isTransactionActive() const { return transactionActive_.load(std::memory_order_acquire); }

	private:
		std::unique_ptr<DirtyEntityRegistry<Node>> nodeRegistry_;
		std::unique_ptr<DirtyEntityRegistry<Edge>> edgeRegistry_;
		std::unique_ptr<DirtyEntityRegistry<Property>> propertyRegistry_;
		std::unique_ptr<DirtyEntityRegistry<Blob>> blobRegistry_;
		std::unique_ptr<DirtyEntityRegistry<Index>> indexRegistry_;
		std::unique_ptr<DirtyEntityRegistry<State>> stateRegistry_;

		size_t maxDirtyEntities_ = 10000;
		std::function<void()> autoFlushCallback_;
		std::atomic<bool> transactionActive_{false};
	};
} // namespace graph::storage
