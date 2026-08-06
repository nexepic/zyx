/**
 * @file test_FileStorage_OpenModes.cpp
 * @brief Branch coverage tests for FileStorage open modes, flush concurrency,
 *        and compaction paths.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <thread>
#include <gtest/gtest.h>
#include "graph/storage/FileStorage.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;

class FileStorageOpenModeTest : public ::testing::Test {
protected:
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_openmode_" + to_string(uuid) + ".dat");
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	[[nodiscard]] std::shared_ptr<FileStorage> openLegacyStorage(
			OpenMode mode = OpenMode::OPEN_CREATE_NEW_FILE) const {
		auto storage = std::make_shared<FileStorage>(testFilePath.string(), 16U, mode);
		storage->open();
		return storage;
	}
};

TEST_F(FileStorageOpenModeTest, OpenExistingFile_FileDoesNotExist_Throws) {
	auto storage = std::make_shared<FileStorage>(testFilePath.string(), 16, OpenMode::OPEN_EXISTING_FILE);
	EXPECT_THROW(storage->open(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, CreateNewFile_FileAlreadyExists_Throws) {
	{
		auto storage = openLegacyStorage();
		storage->close();
	}
	ASSERT_TRUE(fs::exists(testFilePath));

	auto storage = std::make_shared<FileStorage>(testFilePath.string(), 16, OpenMode::OPEN_CREATE_NEW_FILE);
	EXPECT_THROW(storage->open(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, OpenAlreadyOpen_ReturnsEarly) {
	auto storage = openLegacyStorage();
	EXPECT_TRUE(storage->isOpen());
	EXPECT_NO_THROW(storage->open());
	EXPECT_TRUE(storage->isOpen());
	storage->close();
}

TEST_F(FileStorageOpenModeTest, SaveOnClosedStorage_Throws) {
	auto storage = openLegacyStorage();
	storage->close();
	EXPECT_THROW(storage->save(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, SaveNoUnsavedChanges_ReturnsEarly) {
	auto storage = openLegacyStorage();
	EXPECT_NO_THROW(storage->save());
	storage->close();
}

TEST_F(FileStorageOpenModeTest, FlushConcurrency_SecondFlushReturnsEarly) {
	auto storage = openLegacyStorage();
	auto dm = storage->getDataManager();

	Node n(0, 0);
	dm->addNode(n);

	std::thread t1([&] { storage->flush(); });
	std::thread t2([&] { storage->flush(); });
	t1.join();
	t2.join();

	storage->close();
}

TEST_F(FileStorageOpenModeTest, FlushWithDeleteAndCompaction) {
	auto storage = openLegacyStorage();
	auto dm = storage->getDataManager();

	for (int i = 0; i < 10; ++i) {
		Node n(0, 0);
		dm->addNode(n);
	}
	storage->flush();

	Node n1 = dm->getNode(1);
	dm->deleteNode(n1);
	storage->flush();

	dm->clearCache();
	Node loaded = dm->getNode(1);
	EXPECT_FALSE(loaded.isActive());

	storage->close();
}

TEST_F(FileStorageOpenModeTest, CompactionDisabledSkipsCompaction) {
	auto storage = openLegacyStorage();
	auto dm = storage->getDataManager();

	storage->setCompactionEnabled(false);

	Node n(0, 0);
	dm->addNode(n);
	storage->flush();

	dm->deleteNode(n);
	storage->flush();

	storage->close();
}
