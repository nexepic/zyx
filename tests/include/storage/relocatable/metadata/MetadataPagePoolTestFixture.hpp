/**
 * @file MetadataPagePoolTestFixture.hpp
 * @brief Shared temporary-file fixture for relocatable metadata page-pool tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <filesystem>
#include <fstream>
#include <memory>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"

namespace graph::storage::test_support {

	class RelocatableMetadataPagePoolTest : public ::testing::Test {
	protected:
		std::filesystem::path testFilePath;
		std::shared_ptr<std::fstream> file;
		std::shared_ptr<StorageIO> io;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			testFilePath =
					std::filesystem::temp_directory_path() / ("relocatable_catalog_pool_" + to_string(uuid) + ".dat");
			openFile(std::ios::trunc);
		}

		void TearDown() override {
			io.reset();
			if (file && file->is_open()) {
				file->close();
			}
			file.reset();
			std::error_code error;
			std::filesystem::remove(testFilePath, error);
		}

		void reopen() {
			io.reset();
			file->close();
			file.reset();
			openFile({});
		}

		void openFile(std::ios::openmode extraMode) {
			file = std::make_shared<std::fstream>(testFilePath,
														  std::ios::in | std::ios::out | std::ios::binary | extraMode);
			ASSERT_TRUE(file->is_open());
			io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		}
	};

} // namespace graph::storage::test_support
