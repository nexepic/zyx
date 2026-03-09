/**
 * @file Database.hpp
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

#pragma once

#include <string>

#include "graph/config/SystemConfigManager.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph {

	class Database {
	public:
		explicit Database(const std::string &dbPath, storage::OpenMode mode = storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
						  size_t cacheSize = 10000);
		~Database();

		void open();
		[[nodiscard]] bool openIfExists();
		void close() const;

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool isOpen() const;

		Transaction beginTransaction();

		// Query engine access
		std::shared_ptr<query::QueryEngine> getQueryEngine() { return queryEngine; }

		// Access to storage for advanced operations
		[[nodiscard]] std::shared_ptr<storage::FileStorage> getStorage() const { return storage; }

	private:
		std::shared_ptr<storage::FileStorage> storage;
		std::shared_ptr<query::QueryEngine> queryEngine;
		std::string dbPath;
		std::shared_ptr<config::SystemConfigManager> configManager_;
	};

} // namespace graph
