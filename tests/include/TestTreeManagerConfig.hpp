/**
 * @file TestTreeManagerConfig.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/IndexTreeManager.hpp"
#include "graph/query/indexes/LabelIndex.hpp"
#include <graph/core/Database.hpp>

namespace graph::storage::test {

	class TestTreeManagerConfig {
	public:
		static void initialize() {
		    if (!isInitialized_) {
		        boost::uuids::uuid uuid = boost::uuids::random_generator()();
		        std::filesystem::path testFilePath =
		                std::filesystem::temp_directory_path() / ("test_db_file_" + to_string(uuid) + ".dat");

		        // Create and initialize Database
		        database = std::make_unique<Database>(testFilePath.string());
		        database->open();

		        treeManager = std::make_shared<query::indexes::IndexTreeManager>(
		                database->getStorage()->getDataManager(), query::indexes::LabelIndex::LABEL_INDEX_TYPE);
		        isInitialized_ = true;
		    }
		}

		static void cleanup() {
		    if (isInitialized_ && database) {
		        database->close();
		        database.reset();
		        treeManager.reset();
		        isInitialized_ = false;
		    }
		}

		static std::shared_ptr<query::indexes::IndexTreeManager> &getTreeManager() { return treeManager; }

		static void setMaxKeysPerNode(uint32_t maxKeys) {
			if (!isInitialized_) {
				initializeOriginalMaxKeys();
			}
			query::indexes::IndexTreeManager::MAX_KEYS_PER_NODE = maxKeys;
		}

		static void resetMaxKeysPerNode() {
			if (!isInitialized_) {
				initializeOriginalMaxKeys();
			}
			query::indexes::IndexTreeManager::MAX_KEYS_PER_NODE = originalMaxKeys_;
		}

	private:
		static inline std::unique_ptr<Database> database;
		static inline std::shared_ptr<query::indexes::IndexTreeManager> treeManager;
		static inline bool isInitialized_ = false;
		static inline uint32_t originalMaxKeys_ = 0;

		static void initializeOriginalMaxKeys() {
			originalMaxKeys_ = query::indexes::IndexTreeManager::MAX_KEYS_PER_NODE;
			isInitialized_ = true;
		}
	};

} // namespace graph::storage::test
