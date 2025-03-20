/**
 * @file Database
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Database.h"
#include "graph/storage/FileStorage.h"
#include <filesystem>

namespace graph {

	Database::Database(const std::string& dbPath)
	 : storage(std::make_unique<storage::FileStorage>(dbPath)), dbPath(dbPath) {}

	Database::~Database() = default;

	void Database::open() {
		if (!openFlag) {
			storage->load(nodes, edges);
			openFlag = true;
		}
	}

	void Database::close() {
		if (openFlag) {
			// storage->save(nodes, edges);
			openFlag = false;
		}
	}

	bool Database::exists() const {
		return std::filesystem::exists(dbPath);
	}

	bool Database::isOpen() const { // Implement the isOpen method
		return openFlag;
	}

	Transaction Database::beginTransaction() {
		return Transaction(*this);
	}

} // namespace graph