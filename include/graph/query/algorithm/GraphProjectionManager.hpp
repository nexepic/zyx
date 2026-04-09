/**
 * @file GraphProjectionManager.hpp
 * @author Nexepic
 * @date 2026/4/9
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
#include <string>
#include <unordered_map>
#include "GraphProjection.hpp"

namespace graph::query::algorithm {

	/**
	 * @brief Manages named graph projections.
	 *
	 * Stores projections in memory with thread-safe access.
	 * Projections persist until explicitly dropped or the manager is destroyed.
	 */
	class GraphProjectionManager {
	public:
		void createProjection(const std::string &name, std::shared_ptr<GraphProjection> projection) {
			std::lock_guard lock(mutex_);
			if (projections_.contains(name)) {
				throw std::runtime_error("Graph projection '" + name + "' already exists");
			}
			projections_[name] = std::move(projection);
		}

		[[nodiscard]] std::shared_ptr<GraphProjection> getProjection(const std::string &name) const {
			std::lock_guard lock(mutex_);
			auto it = projections_.find(name);
			if (it == projections_.end()) {
				throw std::runtime_error("Graph projection '" + name + "' not found");
			}
			return it->second;
		}

		bool dropProjection(const std::string &name) {
			std::lock_guard lock(mutex_);
			return projections_.erase(name) > 0;
		}

		[[nodiscard]] bool exists(const std::string &name) const {
			std::lock_guard lock(mutex_);
			return projections_.contains(name);
		}

	private:
		mutable std::mutex mutex_;
		std::unordered_map<std::string, std::shared_ptr<GraphProjection>> projections_;
	};

} // namespace graph::query::algorithm
