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
#include "graph/core/Database.hpp"
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
};

TEST_F(FileStorageOpenModeTest, OpenExistingFile_FileDoesNotExist_Throws) {
	auto storage = std::make_shared<FileStorage>(testFilePath.string(), 16, OpenMode::OPEN_EXISTING_FILE);
	EXPECT_THROW(storage->open(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, CreateNewFile_FileAlreadyExists_Throws) {
	{
		Database db(testFilePath.string());
		db.open();
		db.close();
	}
	ASSERT_TRUE(fs::exists(testFilePath));

	auto storage = std::make_shared<FileStorage>(testFilePath.string(), 16, OpenMode::OPEN_CREATE_NEW_FILE);
	EXPECT_THROW(storage->open(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, OpenAlreadyOpen_ReturnsEarly) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	EXPECT_TRUE(storage->isOpen());
	EXPECT_NO_THROW(storage->open());
	EXPECT_TRUE(storage->isOpen());
	db.close();
}

TEST_F(FileStorageOpenModeTest, SaveOnClosedStorage_Throws) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	db.close();
	EXPECT_THROW(storage->save(), std::runtime_error);
}

TEST_F(FileStorageOpenModeTest, SaveNoUnsavedChanges_ReturnsEarly) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	EXPECT_NO_THROW(storage->save());
	db.close();
}

TEST_F(FileStorageOpenModeTest, FlushConcurrency_SecondFlushReturnsEarly) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	Node n(0, 0);
	dm->addNode(n);

	std::thread t1([&] { storage->flush(); });
	std::thread t2([&] { storage->flush(); });
	t1.join();
	t2.join();

	db.close();
}

TEST_F(FileStorageOpenModeTest, FlushWithDeleteAndCompaction) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
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

	db.close();
}

TEST_F(FileStorageOpenModeTest, CompactionDisabledSkipsCompaction) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	storage->setCompactionEnabled(false);

	Node n(0, 0);
	dm->addNode(n);
	storage->flush();

	dm->deleteNode(n);
	storage->flush();

	db.close();
}
