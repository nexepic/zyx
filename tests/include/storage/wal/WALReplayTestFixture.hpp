/**
 * @file WALReplayTestFixture.hpp
 * @date 2026/4/14
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

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/core/State.hpp"
#include "graph/core/TransactionManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"
#include "graph/storage/wal/WALRecovery.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;
using namespace graph::storage::wal;

/**
 * Helper: create a unique temp DB path.
 */
inline fs::path makeTempDbPath() {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	return fs::temp_directory_path() / ("test_walreplay_" + boost::uuids::to_string(uuid) + ".zyx");
}

/**
 * Test-only owner for the legacy physical storage and its WAL transaction
 * machinery.  The public Database API is intentionally relocatable-only, so WAL
 * replay coverage must not obtain legacy storage through it.
 */
class LegacyWALTestDatabase {
public:
	explicit LegacyWALTestDatabase(std::string databasePath) : databasePath_(std::move(databasePath)) {}

	~LegacyWALTestDatabase() {
		try {
			close();
		} catch (...) {
			// Test teardown must not throw while preserving the WAL for inspection.
		}
	}

	LegacyWALTestDatabase(const LegacyWALTestDatabase &) = delete;
	LegacyWALTestDatabase &operator=(const LegacyWALTestDatabase &) = delete;

	void open() {
		if (storage_ && storage_->isOpen()) {
			return;
		}

		storage_ = std::make_shared<FileStorage>(
				databasePath_, 10'000U, OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		storage_->open();
	}

	void close() {
		if (!storage_ || !storage_->isOpen()) {
			return;
		}

		// Match legacy Database close ordering: persist data before removing WAL.
		if (walManager_) {
			storage_->flushOrThrow();
			if (transactionManager_) {
				transactionManager_->markStorageCheckpointed();
			}
			walManager_->close(WALManager::CloseMode::WCM_REMOVE_FILE,
							   WALManager::CloseSyncMode::WSM_FLUSH_ONLY);
		}
		storage_->close();
		transactionManager_.reset();
		walManager_.reset();
	}

	[[nodiscard]] std::shared_ptr<FileStorage> storage() const { return storage_; }

	Transaction beginTransaction() {
		ensureWALAndTransactionManager();
		ensureWALForWrites();
		return transactionManager_->begin();
	}

	Transaction beginReadOnlyTransaction() {
		ensureWALAndTransactionManager();
		return transactionManager_->beginReadOnly();
	}

private:
	void ensureWALAndTransactionManager() {
		if (transactionManager_) {
			return;
		}
		if (!storage_ || !storage_->isOpen()) {
			throw std::logic_error("legacy WAL test database must be open before starting a transaction");
		}

		const std::string walPath = databasePath_ + "-wal";
		if (fs::exists(walPath)) {
			walManager_ = std::make_shared<WALManager>();
			walManager_->open(databasePath_);
			if (walManager_->needsRecovery()) {
				WALRecovery recovery(*walManager_, storage_->getStorageWriter(), storage_->getIDAllocators(),
							 storage_->getSegmentTracker(), storage_->getStorageIO(),
							 storage_->getDataManager(), storage_->getFileHeaderManager());
				recovery.recover();
			}
			storage_->getDataManager()->setWALManager(walManager_.get());
		}

		transactionManager_ = std::make_unique<TransactionManager>(storage_, walManager_);
	}

	void ensureWALForWrites() {
		if (walManager_) {
			return;
		}

		walManager_ = std::make_shared<WALManager>();
		walManager_->open(databasePath_);
		transactionManager_->setWALManager(walManager_);
		storage_->getDataManager()->setWALManager(walManager_.get());
	}

	std::string databasePath_;
	std::shared_ptr<FileStorage> storage_;
	std::shared_ptr<WALManager> walManager_;
	std::unique_ptr<TransactionManager> transactionManager_;
};

class WALReplayTest : public ::testing::Test {
protected:
	void SetUp() override {
		dbPath = makeTempDbPath();
		fs::remove_all(dbPath);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(dbPath, ec);
		std::string walPath = dbPath.string() + "-wal";
		fs::remove(walPath, ec);
	}

	fs::path dbPath;
};
