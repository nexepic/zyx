/**
 * @file Database.cpp
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

#include "graph/core/Database.hpp"
#include <filesystem>

namespace graph {

	Database::Database(const std::string &dbPath, storage::OpenMode mode, size_t cacheSize) : dbPath(dbPath) {
		storage = std::make_shared<storage::FileStorage>(dbPath, cacheSize, mode);
	}

	Database::~Database() { close(); }

	bool Database::isOpen() const {
		// The database is open if the storage is initialized and its file is open.
		return storage && storage->isOpen();
	}

	void Database::open() {
		if (isOpen()) {
			return;
		}

		storage->open();
		configManager_ = std::make_shared<config::SystemConfigManager>(storage->getSystemStateManager(), storage);
		configManager_->loadAndApplyAll();
		storage->getDataManager()->registerObserver(configManager_);
		queryEngine = std::make_shared<query::QueryEngine>(storage);
	}

	bool Database::openIfExists() {
		if (isOpen())
			return true;
		if (!exists())
			return false;

		try {
			open();
			return isOpen(); // Verify it's actually open
		} catch (...) {
			return false;
		}
	}

	void Database::close() const {
		if (!isOpen()) {
			return;
		}

		storage->close();
	}

	bool Database::exists() const { return std::filesystem::exists(dbPath); }

	Transaction Database::beginTransaction() {
		if (!isOpen()) {
			open();
		}

		storage::FileStorage::beginWrite();
		return Transaction(*this);
	}

} // namespace graph
