/**
 * @file test_RelocatableStorageBackend.cpp
 * @brief Integration tests for the format-safe relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageBackend.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "zyx/zyx.hpp"
#include "zyx/zyx_driver_abi.h"

using namespace graph;
using namespace graph::storage;
namespace logical = graph::query::logical;
namespace execution = graph::query::execution;

namespace relocatable_storage_backend_test {

	inline Superblock initialSuperblock() {
		Superblock superblock;
		superblock.database_id = DatabaseId(0x0A0B0C0D0E0F1011ULL);
		superblock.generation = Generation(1);
		superblock.checkpoint_lsn = LogSequenceNumber(1);
		return superblock;
	}

	class RelocatableStorageBackendTest : public ::testing::Test {
	protected:
		std::filesystem::path data_path_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			data_path_ = std::filesystem::temp_directory_path() / ("relocatable_backend_" + to_string(uuid) + ".dat");
		}

		void TearDown() override {
			std::error_code error;
			std::filesystem::remove(data_path_, error);
		}

		void materializeNativeDatabase() {
			RelocatableStorageBackendOptions options;
			options.data_path = data_path_;
			options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
			auto backend = RelocatableStorageBackend::initializeNew(std::move(options));
			ASSERT_NE(backend, nullptr);
			ASSERT_NO_THROW(backend->open());
			ASSERT_NO_THROW(backend->close());
		}

		struct NativeGraphFixture {
			uint64_t person_node_id = 0U;
			uint64_t unlabeled_node_id = 0U;
			uint64_t knows_edge_id = 0U;
			uint64_t mentions_edge_id = 0U;
		};

		// Build integration data through the public logical writer. Tests that read
		// the clean format must not inject fixed-record or legacy Property-ID bytes.
		[[nodiscard]] NativeGraphFixture materializeNativeGraphFixture() {
			materializeNativeDatabase();
			zyx::Database database(data_path_.string());
			database.open();
			NativeGraphFixture fixture;
			const int64_t person = database.createNode(std::vector<std::string>{"Person"},
					{{"alpha", int64_t(42)}, {"beta", std::string("value")}});
			const int64_t unlabeled = database.createNode(std::vector<std::string>{}, {});
			if (person <= 0 || unlabeled <= 0) {
				database.close();
				throw std::runtime_error("native graph fixture could not allocate its seed Nodes");
			}
			const int64_t knows = database.createEdge(person, person, "KNOWS", {{"alpha", int64_t(7)}});
			const int64_t mentions = database.createEdge(person, person, "MENTIONS", {});
			if (knows <= 0 || mentions <= 0) {
				database.close();
				throw std::runtime_error("native graph fixture could not allocate its seed Edges");
			}
			fixture.person_node_id = static_cast<uint64_t>(person);
			fixture.unlabeled_node_id = static_cast<uint64_t>(unlabeled);
			fixture.knows_edge_id = static_cast<uint64_t>(knows);
			fixture.mentions_edge_id = static_cast<uint64_t>(mentions);
			database.close();
			return fixture;
		}

		[[nodiscard]] RelocatableStorageBackendOptions backendOptions() const {
			RelocatableStorageBackendOptions options;
			options.data_path = data_path_;
			return options;
		}

		void publishInterruptedGeneration() {
			auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
			ASSERT_TRUE(data->is_open());
			auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto metadata = RelocatableMetadataStore::open(io);
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const LogSequenceNumber nextLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			MetadataPublishOptions options;
			options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
			EXPECT_THROW(static_cast<void>(metadata->allocateLogicalPageId(nextLsn, options)),
						 BootstrapPublishInterrupted);
			metadata.reset();
			io->sync();
			io.reset();
			data->close();
		}

	};

} // namespace relocatable_storage_backend_test
