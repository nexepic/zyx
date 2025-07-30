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

	void Database::open() {
		if (openFlag)
			return;

		storage->open();
		queryEngine = std::make_shared<query::QueryEngine>(storage);
		storage->setQueryEngine(queryEngine);
		openFlag = true;
	}

	void Database::close() {
		if (!openFlag)
			return;

		storage->close();
		openFlag = false;
	}

	void Database::save() const {
		if (openFlag) {
			storage->save();
		}
	}

	void Database::flush() const {
		if (openFlag) {
			storage->flush();
		}
	}

	bool Database::exists() const { return std::filesystem::exists(dbPath); }

	Transaction Database::beginTransaction() {
		if (!openFlag) {
			open();
		}

		storage::FileStorage::beginWrite();
		return Transaction(*this);
	}

} // namespace graph
