/**
 * @file EntityObserverManager.hpp
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

#include <memory>
#include <mutex>
#include <vector>
#include "graph/storage/constraints/IEntityValidator.hpp"
#include "graph/storage/indexes/IEntityObserver.hpp"

namespace graph::storage {

	/**
	 * @brief Manages entity observer registration and notification dispatch.
	 *
	 * Extracted from DataManager to isolate the pub-sub notification
	 * responsibility into a focused, independently testable unit.
	 */
	class EntityObserverManager {
	public:
		void registerObserver(std::shared_ptr<IEntityObserver> observer);
		void registerValidator(std::shared_ptr<constraints::IEntityValidator> validator);

		void notifyNodeAdded(const Node &node) const;
		void notifyNodesAdded(const std::vector<Node> &nodes) const;
		void notifyNodeUpdated(const Node &oldNode, const Node &newNode) const;
		void notifyNodeDeleted(const Node &node) const;

		void notifyEdgeAdded(const Edge &edge) const;
		void notifyEdgesAdded(const std::vector<Edge> &edges) const;
		void notifyEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) const;
		void notifyEdgeDeleted(const Edge &edge) const;

		void notifyStateUpdated(const State &oldState, const State &newState) const;

		void setSuppressNotifications(bool suppress) const { suppressNotifications_ = suppress; }
		[[nodiscard]] bool isSuppressed() const { return suppressNotifications_; }

		[[nodiscard]] const std::vector<std::shared_ptr<IEntityObserver>> &getObservers() const { return observers_; }
		[[nodiscard]] const std::vector<std::shared_ptr<constraints::IEntityValidator>> &getValidators() const {
			return validators_;
		}

	private:
		std::vector<std::shared_ptr<IEntityObserver>> observers_;
		std::vector<std::shared_ptr<constraints::IEntityValidator>> validators_;
		mutable std::recursive_mutex mutex_;
		mutable bool suppressNotifications_ = false;
	};

} // namespace graph::storage
