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
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"

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
			std::error_code ec;
			fs::remove(testFilePath, ec);
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

	/**
	 * @test updateFileHeader copies all fields from the provided header into the internal reference.
	 */
	TEST_F(FileHeaderManagerTest, UpdateFileHeaderCopiesFields) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Create a new header with specific values
		FileHeader newHeader;
		newHeader.max_node_id = 100;
		newHeader.max_edge_id = 200;
		newHeader.max_prop_id = 300;
		newHeader.max_blob_id = 400;
		newHeader.max_index_id = 500;
		newHeader.max_state_id = 600;
		newHeader.data_crc = 0xDEADBEEF;

		manager.updateFileHeader(newHeader);

		// The internal header reference should now match
		EXPECT_EQ(header.max_node_id, 100);
		EXPECT_EQ(header.max_edge_id, 200);
		EXPECT_EQ(header.max_prop_id, 300);
		EXPECT_EQ(header.max_blob_id, 400);
		EXPECT_EQ(header.max_index_id, 500);
		EXPECT_EQ(header.max_state_id, 600);
		EXPECT_EQ(header.data_crc, 0xDEADBEEF);
	}

	/**
	 * @test getMaxIdRefs return mutable references to all six max ID fields.
	 */
	TEST_F(FileHeaderManagerTest, GetMaxIdRefsForAllEntityTypes) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Verify initial values are 0
		EXPECT_EQ(manager.getMaxNodeIdRef(), 0);
		EXPECT_EQ(manager.getMaxEdgeIdRef(), 0);
		EXPECT_EQ(manager.getMaxPropIdRef(), 0);
		EXPECT_EQ(manager.getMaxBlobIdRef(), 0);
		EXPECT_EQ(manager.getMaxIndexIdRef(), 0);
		EXPECT_EQ(manager.getMaxStateIdRef(), 0);

		// Modify via references
		manager.getMaxNodeIdRef() = 10;
		manager.getMaxEdgeIdRef() = 20;
		manager.getMaxPropIdRef() = 30;
		manager.getMaxBlobIdRef() = 40;
		manager.getMaxIndexIdRef() = 50;
		manager.getMaxStateIdRef() = 60;

		// Verify modifications
		EXPECT_EQ(manager.getMaxNodeIdRef(), 10);
		EXPECT_EQ(manager.getMaxEdgeIdRef(), 20);
		EXPECT_EQ(manager.getMaxPropIdRef(), 30);
		EXPECT_EQ(manager.getMaxBlobIdRef(), 40);
		EXPECT_EQ(manager.getMaxIndexIdRef(), 50);
		EXPECT_EQ(manager.getMaxStateIdRef(), 60);
	}

	/**
	 * @test flushFileHeader writes all six max IDs to the file header on disk.
	 */
	TEST_F(FileHeaderManagerTest, FlushFileHeaderWritesAllMaxIds) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Set all max IDs via extractFileHeaderInfo
		header.max_node_id = 11;
		header.max_edge_id = 22;
		header.max_prop_id = 33;
		header.max_blob_id = 44;
		header.max_index_id = 55;
		header.max_state_id = 66;
		manager.extractFileHeaderInfo();

		manager.flushFileHeader();
		file->close();

		// Re-read from disk
		auto file2 = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header2;
		FileHeaderManager manager2(file2, header2);
		FileHeader diskHeader = manager2.readFileHeader();

		EXPECT_EQ(diskHeader.max_node_id, 11);
		EXPECT_EQ(diskHeader.max_edge_id, 22);
		EXPECT_EQ(diskHeader.max_prop_id, 33);
		EXPECT_EQ(diskHeader.max_blob_id, 44);
		EXPECT_EQ(diskHeader.max_index_id, 55);
		EXPECT_EQ(diskHeader.max_state_id, 66);
	}

	/**
	 * @test extractFileHeaderInfo synchronizes all six fields from header to manager.
	 */
	TEST_F(FileHeaderManagerTest, ExtractFileHeaderInfoAllFields) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		header.max_node_id = 111;
		header.max_edge_id = 222;
		header.max_prop_id = 333;
		header.max_blob_id = 444;
		header.max_index_id = 555;
		header.max_state_id = 666;

		manager.extractFileHeaderInfo();

		EXPECT_EQ(manager.getMaxNodeIdRef(), 111);
		EXPECT_EQ(manager.getMaxEdgeIdRef(), 222);
		EXPECT_EQ(manager.getMaxPropIdRef(), 333);
		EXPECT_EQ(manager.getMaxBlobIdRef(), 444);
		EXPECT_EQ(manager.getMaxIndexIdRef(), 555);
		EXPECT_EQ(manager.getMaxStateIdRef(), 666);
	}

	/**
	 * @test updateAggregatedCrc with an empty CRC vector sets data_crc to 0.
	 */
	TEST_F(FileHeaderManagerTest, UpdateAggregatedCrcEmpty) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Set a non-zero CRC first
		header.data_crc = 0x12345678;

		std::vector<uint32_t> emptyCrcs;
		manager.updateAggregatedCrc(emptyCrcs);

		EXPECT_EQ(header.data_crc, 0u);
	}

	/**
	 * @test updateAggregatedCrc with non-empty CRC vector computes a non-zero CRC.
	 */
	TEST_F(FileHeaderManagerTest, UpdateAggregatedCrcNonEmpty) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		std::vector<uint32_t> crcs = {0xAABBCCDD, 0x11223344, 0x55667788};
		manager.updateAggregatedCrc(crcs);

		// Should be non-zero since we gave it actual data
		EXPECT_NE(header.data_crc, 0u);
	}

	/**
	 * @test updateAggregatedCrc produces consistent results for the same input.
	 */
	TEST_F(FileHeaderManagerTest, UpdateAggregatedCrcConsistent) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		std::vector<uint32_t> crcs = {0x1, 0x2, 0x3};
		manager.updateAggregatedCrc(crcs);
		uint32_t firstCrc = header.data_crc;

		manager.updateAggregatedCrc(crcs);
		uint32_t secondCrc = header.data_crc;

		EXPECT_EQ(firstCrc, secondCrc);
	}

	/**
	 * @test Version mismatch in validateAndReadHeader triggers an exception.
	 */
	TEST_F(FileHeaderManagerTest, DetectsVersionMismatch) {
		// Write a valid header but with wrong version
		{
			auto file = std::make_shared<std::fstream>(testFilePath,
													   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
			FileHeader header;
			header.version = 0x00000001; // Wrong version (expected 0x00000003)
			file->write(reinterpret_cast<const char *>(&header), sizeof(FileHeader));
			file->flush();
		}

		auto file = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header;
		FileHeaderManager manager(file, header);

		EXPECT_THROW(manager.validateAndReadHeader(), std::runtime_error);
	}

	/**
	 * @test readFileHeader throws when the file stream cannot read a full header.
	 */
	TEST_F(FileHeaderManagerTest, ReadFileHeaderThrowsOnTruncatedFile) {
		createDummyFile(testFilePath, 10); // Too small to contain a FileHeader
		auto file = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		FileHeader header;
		FileHeaderManager manager(file, header);

		EXPECT_THROW(manager.readFileHeader(), std::runtime_error);
	}

	/**
	 * @test readFileHeader throws when the underlying stream is closed.
	 */
	TEST_F(FileHeaderManagerTest, ReadFileHeaderThrowsOnClosedStream) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		file->close();

		EXPECT_THROW(manager.readFileHeader(), std::runtime_error);
	}

	/**
	 * @test readFileHeader successfully reads back what was written.
	 */
	TEST_F(FileHeaderManagerTest, ReadFileHeaderRoundTrip) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Set some values and flush
		header.max_node_id = 42;
		header.max_edge_id = 84;
		manager.extractFileHeaderInfo();
		manager.flushFileHeader();

		// Read back
		FileHeader readBack = manager.readFileHeader();
		EXPECT_EQ(readBack.max_node_id, 42);
		EXPECT_EQ(readBack.max_edge_id, 84);
		EXPECT_EQ(std::memcmp(readBack.magic, FILE_HEADER_MAGIC_STRING, 8), 0);
	}

	/**
	 * @test getFileHeader returns a reference to the internal header.
	 */
	TEST_F(FileHeaderManagerTest, GetFileHeaderReturnsReference) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		FileHeader &ref = manager.getFileHeader();
		ref.max_node_id = 777;
		EXPECT_EQ(header.max_node_id, 777);
	}

	/**
	 * @test updateMaxIdForSegment with a SegmentTracker that has no chain (head=0) does not modify maxId.
	 */
	TEST_F(FileHeaderManagerTest, UpdateMaxIdForSegmentEmptyChain) {
		// Create a minimal valid database file with header
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Create StorageIO and SegmentTracker with an empty header (all chain heads = 0)
		auto storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto tracker = std::make_shared<SegmentTracker>(storageIO, header);

		// With no segments, updateFileHeaderMaxIds should leave all max IDs at 0
		manager.updateFileHeaderMaxIds(tracker);

		EXPECT_EQ(manager.getMaxNodeIdRef(), 0);
		EXPECT_EQ(manager.getMaxEdgeIdRef(), 0);
		EXPECT_EQ(manager.getMaxPropIdRef(), 0);
		EXPECT_EQ(manager.getMaxBlobIdRef(), 0);
		EXPECT_EQ(manager.getMaxIndexIdRef(), 0);
		EXPECT_EQ(manager.getMaxStateIdRef(), 0);
	}

	/**
	 * @test flushFileHeader correctly syncs manager max IDs back to the header struct.
	 */
	TEST_F(FileHeaderManagerTest, FlushSyncsMaxIdsToHeader) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		// Set max IDs via refs
		manager.getMaxNodeIdRef() = 1;
		manager.getMaxEdgeIdRef() = 2;
		manager.getMaxPropIdRef() = 3;
		manager.getMaxBlobIdRef() = 4;
		manager.getMaxIndexIdRef() = 5;
		manager.getMaxStateIdRef() = 6;

		manager.flushFileHeader();

		// After flush, the header struct should have the values from the manager
		EXPECT_EQ(header.max_node_id, 1);
		EXPECT_EQ(header.max_edge_id, 2);
		EXPECT_EQ(header.max_prop_id, 3);
		EXPECT_EQ(header.max_blob_id, 4);
		EXPECT_EQ(header.max_index_id, 5);
		EXPECT_EQ(header.max_state_id, 6);
	}

	/**
	 * @test initializeFileHeader zeros data_crc and writes a valid header.
	 */
	TEST_F(FileHeaderManagerTest, InitializeFileHeaderZerosCrc) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		header.data_crc = 0xFFFFFFFF; // Set non-zero CRC

		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		EXPECT_EQ(header.data_crc, 0u);
	}

	/**
	 * @test updateAggregatedCrc with a single-element vector produces non-zero CRC.
	 */
	TEST_F(FileHeaderManagerTest, UpdateAggregatedCrcSingleElement) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		std::vector<uint32_t> crcs = {0xDEADBEEF};
		manager.updateAggregatedCrc(crcs);

		EXPECT_NE(header.data_crc, 0u);
	}

	/**
	 * @test updateAggregatedCrc with different inputs produces different CRCs.
	 */
	TEST_F(FileHeaderManagerTest, UpdateAggregatedCrcDifferentInputs) {
		auto file = std::make_shared<std::fstream>(testFilePath,
												   std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		FileHeader header;
		FileHeaderManager manager(file, header);
		manager.initializeFileHeader();

		std::vector<uint32_t> crcs1 = {0x1, 0x2};
		manager.updateAggregatedCrc(crcs1);
		uint32_t crc1 = header.data_crc;

		std::vector<uint32_t> crcs2 = {0x3, 0x4};
		manager.updateAggregatedCrc(crcs2);
		uint32_t crc2 = header.data_crc;

		EXPECT_NE(crc1, crc2);
	}

} // namespace graph::storage::test
