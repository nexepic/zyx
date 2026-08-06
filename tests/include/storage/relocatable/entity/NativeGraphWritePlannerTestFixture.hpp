/**
 * @file NativeGraphWritePlannerTestFixture.hpp
 * @brief Shared fixture for native graph writer integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <filesystem>
#include <fstream>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage::test {

	inline Superblock nativeGraphInitialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7A7B7C7D7E7F8081ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	class NativeGraphWritePlannerTestFixture : public ::testing::Test {
	protected:
		std::filesystem::path path_;
		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<StorageIO> io_;
		std::unique_ptr<RelocatableMetadataStore> metadata_;
		std::unique_ptr<EntityPageStore> entities_;
		std::unique_ptr<SlottedPagePool> slotted_pages_;
		std::unique_ptr<BTreePageStore> btree_pages_;
		std::unique_ptr<EntityLabelSetStore> labels_;
		std::unique_ptr<PropertyCollectionRootStore> properties_;
		std::unique_ptr<VectorPageStore> vector_pages_;
		std::unique_ptr<VectorManifestStore> vector_manifests_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			path_ = std::filesystem::temp_directory_path() / ("native_graph_write_planner_" + to_string(uuid));
			file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(file_->is_open());
			io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			metadata_ = RelocatableMetadataStore::initializeNew(io_, nativeGraphInitialSuperblock());
			entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
			slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
			btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
			labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
			properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
			vector_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
			vector_manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
		}

		void TearDown() override {
			vector_manifests_.reset();
			vector_pages_.reset();
			properties_.reset();
			labels_.reset();
			btree_pages_.reset();
			slotted_pages_.reset();
			entities_.reset();
			metadata_.reset();
			io_.reset();
			if (file_ && file_->is_open()) {
				file_->close();
			}
			file_.reset();
			std::error_code error;
			std::filesystem::remove(path_, error);
		}

		void reopenNativeStores() {
			vector_manifests_.reset();
			vector_pages_.reset();
			properties_.reset();
			labels_.reset();
			btree_pages_.reset();
			slotted_pages_.reset();
			entities_.reset();
			metadata_.reset();
			metadata_ = RelocatableMetadataStore::open(io_);
			entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
			slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
			btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
			labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
			properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
			vector_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
			vector_manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
		}
	};

} // namespace graph::storage::test
