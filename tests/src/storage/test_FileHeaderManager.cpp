/**
 * @file test_FileHeaderManager.cpp
 * @author Nexepic
 * @date 2026/1/9
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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace graph::storage::test {

	namespace fs = std::filesystem;

	class FileHeaderManagerTest : public ::testing::Test {
	protected:
		const std::string testFilePath = "test_db.mx";

		void SetUp() override {
			// Ensure a clean state before each test
			if (fs::exists(testFilePath)) {
				fs::remove(testFilePath);
			}
		}

		void TearDown() override {
			// Clean up after tests
			if (fs::exists(testFilePath)) {
				fs::remove(testFilePath);
			}
		}

		// Helper to create an empty file of a specific size
		static void createDummyFile(const std::string &path, const size_t size) {
			std::ofstream ofs(path, std::ios::binary | std::ios::out);
			std::vector<char> padding(size, 0);
			ofs.write(padding.data(), size);
			ofs.close();
		}

		// Helper to flip a bit in the file to simulate corruption
		static void corruptFileAt(const std::string &path, const std::streamoff offset) {
			std::fstream fs_stream(path, std::ios::binary | std::ios::in | std::ios::out);
			fs_stream.seekg(offset);
			char b;
			fs_stream.read(&b, 1);
			b ^= 0xFF; // Flip bits
			fs_stream.seekp(offset);
			fs_stream.write(&b, 1);
			fs_stream.close();
		}
	};

	/**
	 * @test Constructor should throw if the file stream is invalid or null.
	 */
	TEST_F(FileHeaderManagerTest, ConstructorThrowsOnInvalidFile) {
		FileHeader header;
		// Test Null
		EXPECT_THROW(FileHeaderManager(nullptr, header), std::runtime_error);

		// Test Not Open
		auto badStream = std::make_shared<std::fstream>();
		EXPECT_THROW(FileHeaderManager(badStream, header), std::runtime_error);
	}

	/**
	 * @test Verify successful initialization and subsequent validation.
	 */
	TEST_F(FileHeaderManagerTest, InitializeAndValidateSuccess) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);

		ASSERT_NO_THROW(manager.initializeFileHeader());
		// This should now pass because both use file-wide CRC scan
		EXPECT_NO_THROW(manager.validateAndReadHeader());
		EXPECT_EQ(std::memcmp(header.magic, FILE_HEADER_MAGIC_STRING, 8), 0);
	}

	/**
	 * @test Verify that a file smaller than FILE_HEADER_SIZE is rejected.
	 */
	TEST_F(FileHeaderManagerTest, DetectsTruncatedFileAsIntegrityError) {
		createDummyFile(testFilePath, 10); // Only 10 bytes, too small
		auto file = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header;
		FileHeaderManager manager(file, header);

		// Should throw because the file size < sizeof(FileHeader)
		EXPECT_THROW(manager.validateAndReadHeader(), std::runtime_error);
	}

	/**
	 * @test Verify that an invalid Magic Number triggers a format error.
	 */
	TEST_F(FileHeaderManagerTest, DetectsInvalidMagicNumber) {
		// Create a file large enough but with junk magic data
		createDummyFile(testFilePath, FILE_HEADER_SIZE);
		auto file = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header;
		FileHeaderManager manager(file, header);

		// Should throw "Invalid file format"
		EXPECT_THROW(manager.validateAndReadHeader(), std::runtime_error);
	}

	/**
	 * @test Verify that validateAndReadHeader only checks magic and version (no file-level CRC).
	 * Data corruption is now detected via segment-level CRC during reads, not on open.
	 */
	TEST_F(FileHeaderManagerTest, DetectsDataCorruptionViaCrc) {
		{
			auto file = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out |
																			 std::ios::trunc);
			FileHeader header;
			FileHeaderManager manager(file, header);
			manager.initializeFileHeader();
		} // File closed

		// Corrupt the file on disk (after the header)
		std::fstream fs_corrupt(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		fs_corrupt.seekp(FILE_HEADER_SIZE + 10);
		fs_corrupt.write("corrupt", 7);
		fs_corrupt.close();

		auto file2 = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header2;
		FileHeaderManager manager2(file2, header2);

		// validateAndReadHeader no longer checks file-level CRC; it only validates magic + version.
		// Data corruption is detected lazily via segment CRC during reads.
		EXPECT_NO_THROW(manager2.validateAndReadHeader());
	}

	/**
	 * @test Verify that Statistics (Max IDs) are correctly synchronized between struct and manager.
	 */
	TEST_F(FileHeaderManagerTest, FlushesAndReadsMetadataCorrectly) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);

		manager.initializeFileHeader();

		// Modify header through struct and sync to private manager state
		header.max_node_id = 999;
		manager.extractFileHeaderInfo();

		EXPECT_NO_THROW(manager.flushFileHeader());
		file->close();

		auto file2 = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader headerRead;
		FileHeaderManager manager2(file2, headerRead);

		FileHeader diskHeader = manager2.readFileHeader();
		EXPECT_EQ(diskHeader.max_node_id, 999);
	}

	/**
	 * @test Verify that the stream "Fail Bit" triggers a runtime error during write.
	 */
	TEST_F(FileHeaderManagerTest, HandlesStreamIoErrors) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Close the file underneath the manager.
		// clear() cannot recover a closed file handle.
		file->close();

		EXPECT_THROW(manager.flushFileHeader(), std::runtime_error);
	}

} // namespace graph::storage::test
