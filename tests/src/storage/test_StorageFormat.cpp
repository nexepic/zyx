/**
 * @file test_StorageFormat.cpp
 * @brief Format-boundary tests for the native relocatable database format.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageBackendFactory.hpp"
#include "graph/storage/StorageFormat.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	std::array<std::byte, sizeof(uint32_t)> relocatableMagic() {
		return {std::byte{0x5A}, std::byte{0x59}, std::byte{0x58}, std::byte{0x53}};
	}

	constexpr std::array<std::byte, 8> HISTORICAL_FILE_MAGIC = {std::byte{0x3C}, std::byte{0x5A}, std::byte{0x59},
																									std::byte{0x58}, std::byte{0x2D}, std::byte{0x44},
																									std::byte{0x42}, std::byte{0x3E}};

	constexpr size_t SUPERBLOCK_FORMAT_VERSION_OFFSET = 4U;
	constexpr size_t SUPERBLOCK_RESERVED_HEADER_OFFSET = 12U;
	constexpr size_t SUPERBLOCK_PAYLOAD_CRC_OFFSET = 112U;
	constexpr size_t SUPERBLOCK_HEADER_CRC_OFFSET = 116U;
	constexpr size_t SUPERBLOCK_PAYLOAD_RESERVED_OFFSET = RSS_SUPERBLOCK_HEADER_BYTES + 32U;

	void resealSuperblock(std::array<std::byte, RSS_SUPERBLOCK_BYTES> &bytes) {
		LittleEndianCodec::writeU32(bytes, SUPERBLOCK_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(
				bytes, SUPERBLOCK_PAYLOAD_CRC_OFFSET,
				graph::utils::calculateCrc(bytes.data() + RSS_SUPERBLOCK_HEADER_BYTES,
										bytes.size() - RSS_SUPERBLOCK_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, SUPERBLOCK_HEADER_CRC_OFFSET,
								graph::utils::calculateCrc(bytes.data(), RSS_SUPERBLOCK_HEADER_BYTES));
	}

} // namespace

class StorageFormatTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("storage_format_" + to_string(uuid) + ".dat");
		resetFile();
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(path, error);
	}

	void closeFixtureFile() {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
	}

	void resetFile() {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}
};

TEST_F(StorageFormatTest, ClassifiesEmptyTruncatedAndUnrecognizedFiles) {
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_EMPTY);

	const std::array<std::byte, 3> truncated{};
	io->writeAt(0, truncated.data(), truncated.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_TRUNCATED);

	resetFile();
	const std::array<std::byte, 8> unknown{};
	io->writeAt(0, unknown.data(), unknown.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_UNRECOGNIZED);

	resetFile();
	io->writeAt(0, HISTORICAL_FILE_MAGIC.data(), HISTORICAL_FILE_MAGIC.size());
	io->sync();
	const StorageFormatInfo result = StorageFormatProbe::inspect(*io);
	EXPECT_EQ(result.kind, StorageFormatKind::SFM_UNRECOGNIZED);
	EXPECT_EQ(result.file_size, HISTORICAL_FILE_MAGIC.size());
}

TEST_F(StorageFormatTest, RecognizesCompleteAndIncompleteRelocatableBootstrapFiles) {
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initialSuperblock());
	EXPECT_TRUE(StorageFormatProbe::inspect(*io).isRelocatable());

	// A complete A/B bootstrap remains valid when one crash-damaged peer is
	// recoverable from the other peer.
	std::byte corrupt = std::byte{0};
	io->writeAt(0, &corrupt, sizeof(corrupt));
	io->sync();
	EXPECT_TRUE(StorageFormatProbe::inspect(*io).isRelocatable());

	resetFile();
	const auto encoded = SuperblockCodec::encode(initialSuperblock());
	io->writeAt(0, encoded.data(), encoded.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);

	resetFile();
	std::array<std::byte, 8> earlyMagic{};
	const auto magic = relocatableMagic();
	std::copy(magic.begin(), magic.end(), earlyMagic.begin());
	io->writeAt(0, earlyMagic.data(), earlyMagic.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);

	resetFile();
	static_cast<void>(io->reserveAppendSpace(RSS_FILE_BOOTSTRAP_BYTES));
	io->writeAt(0, magic.data(), magic.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);

	resetFile();
	io->writeAt(RSS_SUPERBLOCK_BYTES, magic.data(), magic.size());
	io->sync();
	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);
}

TEST_F(StorageFormatTest, DescribesEveryKnownAndUnknownFormat) {
	EXPECT_STREQ(StorageFormatProbe::describe(StorageFormatKind::SFM_EMPTY), "empty");
	EXPECT_STREQ(StorageFormatProbe::describe(StorageFormatKind::SFM_RELOCATABLE), "relocatable");
	EXPECT_STREQ(StorageFormatProbe::describe(StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE), "incomplete relocatable");
	EXPECT_STREQ(StorageFormatProbe::describe(StorageFormatKind::SFM_TRUNCATED), "truncated");
	EXPECT_STREQ(StorageFormatProbe::describe(StorageFormatKind::SFM_UNRECOGNIZED), "unrecognized");
	EXPECT_STREQ(StorageFormatProbe::describe(static_cast<StorageFormatKind>(99)), "invalid");
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsHistoricalFileMagic) {
	io->writeAt(0, HISTORICAL_FILE_MAGIC.data(), HISTORICAL_FILE_MAGIC.size());
	io->sync();

	EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE)),
				 std::runtime_error);
}

TEST_F(StorageFormatTest, UnsupportedBootstrapVersionsAreRejectedWithoutChangingTheExistingFile) {
	const std::array<uint16_t, 2U> versions{
		static_cast<uint16_t>(RSS_SUPERBLOCK_VERSION - 1U), static_cast<uint16_t>(RSS_SUPERBLOCK_VERSION + 1U)};
	for (const uint16_t version : versions) {
		resetFile();
		auto unsupported = SuperblockCodec::encode(initialSuperblock());
		LittleEndianCodec::writeU16(unsupported, SUPERBLOCK_FORMAT_VERSION_OFFSET, version);
		resealSuperblock(unsupported);
		std::array<std::byte, RSS_FILE_BOOTSTRAP_BYTES> expected{};
		std::copy(unsupported.begin(), unsupported.end(), expected.begin());
		std::copy(unsupported.begin(), unsupported.end(), expected.begin() + RSS_SUPERBLOCK_BYTES);
		io->writeAt(0U, expected.data(), expected.size());
		io->sync();
		closeFixtureFile();
		EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE)),
			std::runtime_error);
		ASSERT_EQ(std::filesystem::file_size(path), expected.size());
		std::ifstream unchanged(path, std::ios::binary);
		ASSERT_TRUE(unchanged.is_open());
		std::array<std::byte, RSS_FILE_BOOTSTRAP_BYTES> actual{};
		unchanged.read(reinterpret_cast<char *>(actual.data()), static_cast<std::streamsize>(actual.size()));
		ASSERT_EQ(unchanged.gcount(), static_cast<std::streamsize>(actual.size()));
		EXPECT_EQ(actual, expected);
	}
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsMissingAndNonRegularPaths) {
	closeFixtureFile();
	ASSERT_TRUE(std::filesystem::remove(path));

	EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE)),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(
						 StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_READ_ONLY_EXISTING_FILE)),
				 std::runtime_error);

	ASSERT_TRUE(std::filesystem::create_directory(path));
	EXPECT_THROW(
			static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_CREATE_OR_OPEN_FILE)),
			std::runtime_error);
}

TEST_F(StorageFormatTest, DatabaseFactoryCreatesAndOpensANewRelocatableDatabase) {
	closeFixtureFile();
	ASSERT_TRUE(std::filesystem::remove(path));

	const StorageBackendSelection creation =
			StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_CREATE_NEW_FILE);
	ASSERT_NE(creation.backend, nullptr);
	ASSERT_NE(creation.relocatable_storage, nullptr);
	EXPECT_EQ(creation.backend->formatKind(), StorageFormatKind::SFM_RELOCATABLE);
	EXPECT_FALSE(creation.backend->isOpen());
	EXPECT_NO_THROW(creation.backend->open());
	EXPECT_TRUE(creation.backend->isOpen());
	ASSERT_NE(creation.relocatable_storage, nullptr);
	EXPECT_EQ(creation.relocatable_storage->ioStats().buffer_pool.capacity_bytes, 4096U);
	const BufferPoolStats bufferBeforeReset = creation.relocatable_storage->ioStats().buffer_pool;
	creation.relocatable_storage->resetIoStats();
	const RelocatableStorageIOStats afterReset = creation.relocatable_storage->ioStats();
	EXPECT_EQ(afterReset.data, (StorageIOStats{}));
	EXPECT_EQ(afterReset.buffer_pool.capacity_bytes, bufferBeforeReset.capacity_bytes);
	EXPECT_EQ(afterReset.buffer_pool.resident_bytes, bufferBeforeReset.resident_bytes);
	EXPECT_EQ(afterReset.buffer_pool.pinned_bytes, bufferBeforeReset.pinned_bytes);
	EXPECT_EQ(afterReset.buffer_pool.dirty_bytes, bufferBeforeReset.dirty_bytes);
	EXPECT_EQ(afterReset.buffer_pool.hits, 0U);
	EXPECT_EQ(afterReset.buffer_pool.misses, 0U);
	EXPECT_EQ(afterReset.buffer_pool.evictions, 0U);
	EXPECT_EQ(afterReset.buffer_pool.prefetch_requests, 0U);
	EXPECT_EQ(afterReset.buffer_pool.prefetch_hits, 0U);
	EXPECT_EQ(afterReset.buffer_pool.disk_read_bytes, 0U);
	EXPECT_EQ(afterReset.buffer_pool.disk_write_bytes, 0U);
	EXPECT_NO_THROW(creation.backend->close());
}

TEST_F(StorageFormatTest, DatabaseFactoryCreatesNoMetadataWalShmOrMigrationSidecar) {
	closeFixtureFile();
	ASSERT_TRUE(std::filesystem::remove(path));

	const StorageBackendSelection creation =
			StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_CREATE_NEW_FILE);
	ASSERT_NE(creation.backend, nullptr);
	ASSERT_NO_THROW(creation.backend->open());
	ASSERT_NO_THROW(creation.backend->close());

	const std::array sidecars = {std::filesystem::path(path.string() + ".wal"),
								 std::filesystem::path(path.string() + "-wal"),
								 std::filesystem::path(path.string() + ".shm"),
								 std::filesystem::path(path.string() + "-shm"),
								 std::filesystem::path(path.string() + ".migration")};
	for (const std::filesystem::path &sidecar: sidecars) {
		std::error_code error;
		EXPECT_FALSE(std::filesystem::exists(sidecar, error));
		EXPECT_FALSE(error) << "failed to inspect unexpected sidecar " << sidecar;
	}
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsBufferPoolCapacitySmallerThanOneRawPage) {
	closeFixtureFile();
	ASSERT_TRUE(std::filesystem::remove(path));
	EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4095U, OpenMode::OPEN_CREATE_NEW_FILE)),
				 std::invalid_argument);
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(StorageFormatTest, DatabaseFactoryReopensRelocatableDatabasesInWritableAndReadOnlyModes) {
	closeFixtureFile();
	ASSERT_TRUE(std::filesystem::remove(path));
	{
		const StorageBackendSelection creation =
				StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_CREATE_NEW_FILE);
		ASSERT_NE(creation.backend, nullptr);
		ASSERT_NO_THROW(creation.backend->open());
		ASSERT_NO_THROW(creation.backend->close());
	}
	{
		const StorageBackendSelection writable =
				StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE);
		ASSERT_NE(writable.relocatable_storage, nullptr);
		EXPECT_FALSE(writable.backend->isReadOnly());
		EXPECT_NO_THROW(writable.backend->open());
		EXPECT_NO_THROW(writable.backend->close());
	}
	{
		const StorageBackendSelection readOnly =
				StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		ASSERT_NE(readOnly.relocatable_storage, nullptr);
		EXPECT_TRUE(readOnly.backend->isReadOnly());
		EXPECT_NO_THROW(readOnly.backend->open());
		EXPECT_NO_THROW(readOnly.backend->close());
	}
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsUnrecognizedExistingFiles) {
	const std::array<std::byte, 8> unknown{};
	io->writeAt(0, unknown.data(), unknown.size());
	io->sync();

	EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE)),
				 std::runtime_error);
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsChecksumValidButNonCanonicalBootstraps) {
	const auto assertRejected = [this](std::array<std::byte, RSS_SUPERBLOCK_BYTES> invalid) {
		// Both A/B slots contain a recognizable but structurally invalid image.
		// This verifies that format selection cannot treat checksum-valid junk as a
		// recoverable native database.
		static_cast<void>(io->reserveAppendSpace(RSS_FILE_BOOTSTRAP_BYTES));
		io->writeAt(0U, invalid.data(), invalid.size());
		io->writeAt(RSS_SUPERBLOCK_BYTES, invalid.data(), invalid.size());
		io->sync();
		EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);

		closeFixtureFile();
		EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U,
																						 OpenMode::OPEN_EXISTING_FILE)),
					 std::runtime_error);
	};

	auto unsupportedVersion = SuperblockCodec::encode(initialSuperblock());
	LittleEndianCodec::writeU16(unsupportedVersion, SUPERBLOCK_FORMAT_VERSION_OFFSET,
								RSS_SUPERBLOCK_VERSION + 1U);
	resealSuperblock(unsupportedVersion);
	assertRejected(unsupportedVersion);

	resetFile();
	auto reservedHeader = SuperblockCodec::encode(initialSuperblock());
	LittleEndianCodec::writeU32(reservedHeader, SUPERBLOCK_RESERVED_HEADER_OFFSET, 1U);
	resealSuperblock(reservedHeader);
	assertRejected(reservedHeader);

	resetFile();
	auto reservedPayload = SuperblockCodec::encode(initialSuperblock());
	reservedPayload[SUPERBLOCK_PAYLOAD_RESERVED_OFFSET] = std::byte{0x01U};
	resealSuperblock(reservedPayload);
	assertRejected(reservedPayload);
}

TEST_F(StorageFormatTest, DatabaseFactoryRejectsBootstrapsWithNoValidChecksum) {
	auto corrupted = SuperblockCodec::encode(initialSuperblock());
	corrupted[RSS_SUPERBLOCK_HEADER_BYTES] ^= std::byte{0x01U};
	static_cast<void>(io->reserveAppendSpace(RSS_FILE_BOOTSTRAP_BYTES));
	io->writeAt(0U, corrupted.data(), corrupted.size());
	io->writeAt(RSS_SUPERBLOCK_BYTES, corrupted.data(), corrupted.size());
	io->sync();

	EXPECT_EQ(StorageFormatProbe::inspect(*io).kind, StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE);
	closeFixtureFile();
	EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(path.string(), 4096U, OpenMode::OPEN_EXISTING_FILE)),
				 std::runtime_error);
}
