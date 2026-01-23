/**
 * @file VectorIndexManager.hpp
 * @author Nexepic
 * @date 2026/1/22
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

namespace graph::storage {
	class DataManager;
	namespace state {
		class SystemStateManager;
	}
} // namespace graph::storage

namespace graph::vector {
	class DiskANNIndex;
}

namespace graph::query::indexes {

	class VectorIndexManager {
	public:
		VectorIndexManager(std::shared_ptr<storage::DataManager> dm,
						   std::shared_ptr<storage::state::SystemStateManager> sm);
		~VectorIndexManager();

		/**
		 * @brief Retrieves (or loads) a vector index by name.
		 * Thread-safe.
		 */
		std::shared_ptr<vector::DiskANNIndex> getIndex(const std::string &name);

		/**
		 * @brief Creates a new vector index definition.
		 */
		void createIndex(const std::string &name, int dim);

		/**
		 * @brief Checks if an index exists (in memory or disk).
		 */
		bool hasIndex(const std::string &name);

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<storage::state::SystemStateManager> stateManager_;

		// Cache of loaded indexes
		std::unordered_map<std::string, std::shared_ptr<vector::DiskANNIndex>> indexes_;
		std::mutex mutex_;
	};

} // namespace graph::query::indexes
