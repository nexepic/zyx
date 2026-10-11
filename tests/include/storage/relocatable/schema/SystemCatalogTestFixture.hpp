#pragma once
#include <algorithm>
#include <stdexcept>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace graph::storage;
using graph::Node;
using graph::PropertyValue;

	inline Superblock systemCatalogFixtureInitialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x5A5958434154414CULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	inline NativeGraphWritePlannerOptions systemCatalogFixturePlannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 821U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 822U};
		return options;
	}

class SystemCatalogTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> slotted_pages_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("system_catalog_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, systemCatalogFixtureInitialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		labels_.reset();
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
		properties_.reset();
		labels_.reset();
		slotted_pages_.reset();
		entities_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
	}
};
