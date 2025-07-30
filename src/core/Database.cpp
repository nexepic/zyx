/**
 * @file Database.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Database.hpp"
#include <filesystem>

namespace graph {

	Database::Database(const std::string &dbPath, size_t cacheSize) : dbPath(dbPath) {
		storage = std::make_shared<storage::FileStorage>(dbPath, cacheSize);
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
		queryEngine = std::make_shared<query::QueryEngine>(storage);
		storage->setQueryEngine(queryEngine);
	}

	void Database::close() const {
		if (!isOpen()) {
			return;
		}

		storage->close();
	}

	void Database::save() const {
		if (isOpen()) {
			storage->save();
		}
	}

	void Database::flush() const {
		if (isOpen()) {
			storage->flush();
		}
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
