#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/PropertyEntitySegmentScanner.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;

namespace {

struct ScannerTestState {
	std::vector<char> readBuffer;
	size_t visitedSegments = 0;
};

struct SegmentWorkItem {
	size_t segmentIndex = 0;
};

std::shared_ptr<DataManager> makeNoPreadDataManager() {
	static FileHeader header{};
	IDAllocators allocators{};
	auto noPreadIO = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto segmentTracker = std::make_shared<SegmentTracker>(noPreadIO, header);
	return std::make_shared<DataManager>(nullptr, 16, header, allocators, segmentTracker, noPreadIO);
}

class PropertyEntitySegmentScannerTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() /
				 ("test_property_entity_segment_scanner_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		labelId = dm->getOrCreateTokenId("User");
	}

	void TearDown() override {
		dm.reset();
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove_all(dbPath, ec);
	}

	int64_t addUserWithProperty(const std::string &key, const PropertyValue &value) {
		Node node(0, labelId);
		dm->addNode(node);
		dm->addNodeProperties(node.getId(), {{key, value}});
		return node.getId();
	}

	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t labelId = 0;
};

} // namespace

TEST_F(PropertyEntitySegmentScannerTest, EmptyPropertyIndexIsSuccessfulNoOp) {
	size_t mergedSegments = 0;
	const bool scanned = detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.empty",
			[](const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				mergedSegments += state.visitedSegments;
			});

	EXPECT_TRUE(scanned);
	EXPECT_EQ(mergedSegments, 0U);

	const std::vector<SegmentWorkItem> emptyWork;
	EXPECT_FALSE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.empty_work",
			emptyWork,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &) {},
			[](size_t, ScannerTestState &) {})));
}

TEST_F(PropertyEntitySegmentScannerTest, RejectsNonPositionalReadStorage) {
	auto noPreadDm = makeNoPreadDataManager();
	size_t mergedSegments = 0;
	EXPECT_FALSE((detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*noPreadDm,
			nullptr,
			"test.property_segments.no_pread_all",
			[](const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				mergedSegments += state.visitedSegments;
			})));
	EXPECT_EQ(mergedSegments, 0U);

	const std::vector<SegmentWorkItem> work{{0}};
	EXPECT_FALSE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*noPreadDm,
			nullptr,
			"test.property_segments.no_pread_work",
			work,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &) {},
			[](size_t, ScannerTestState &) {})));
}

TEST_F(PropertyEntitySegmentScannerTest, ScansAllAndTargetedPropertySegments) {
	addUserWithProperty("id", PropertyValue("u0"));
	addUserWithProperty("score", PropertyValue(int64_t{7}));
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	size_t allVisited = 0;
	const bool allScanned = detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.all",
			[](const SegmentHeader &header, const char *, ScannerTestState &state) {
				if (header.used != 0 && header.data_type == Property::typeId) {
					++state.visitedSegments;
				}
			},
			[&](size_t, ScannerTestState &state) {
				allVisited += state.visitedSegments;
			});
	EXPECT_TRUE(allScanned);
	EXPECT_GT(allVisited, 0U);

	const auto &segments = dm->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segments.empty());
	std::vector<SegmentWorkItem> work;
	work.reserve(segments.size());
	for (size_t index = 0; index < segments.size(); ++index) {
		work.push_back({index});
	}

	size_t targetedVisited = 0;
	const bool targetedScanned = detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.targeted",
			work,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &header, const char *, ScannerTestState &state) {
				if (header.used != 0 && header.data_type == Property::typeId) {
					++state.visitedSegments;
				}
			},
			[&](size_t, ScannerTestState &state) {
				targetedVisited += state.visitedSegments;
			});

	EXPECT_TRUE(targetedScanned);
	EXPECT_GT(targetedVisited, 0U);
}

TEST_F(PropertyEntitySegmentScannerTest, IgnoresNonPropertySegmentsInInjectedIndex) {
	Node node(0, labelId);
	dm->addNode(node);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	auto segmentIndexManager = dm->getSegmentIndexManager();
	const auto &nodeSegments = segmentIndexManager->getNodeSegmentIndex();
	ASSERT_FALSE(nodeSegments.empty());
	segmentIndexManager->setSegmentIndex(Property::typeId, {nodeSegments.front()});

	size_t allVisited = 0;
	EXPECT_TRUE((detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.non_property_all",
			[](const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				allVisited += state.visitedSegments;
			})));
	EXPECT_EQ(allVisited, 0U);

	const std::vector<SegmentWorkItem> work{{0}};
	size_t targetedVisited = 0;
	EXPECT_TRUE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.non_property_work",
			work,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				targetedVisited += state.visitedSegments;
			})));
	EXPECT_EQ(targetedVisited, 0U);
}

TEST_F(PropertyEntitySegmentScannerTest, ToleratesShortSequentialCoalescedReads) {
	addUserWithProperty("id", PropertyValue("u0"));
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	const uint64_t missingOffset =
			static_cast<uint64_t>(fs::file_size(dbPath)) + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE);
	auto segmentIndexManager = dm->getSegmentIndexManager();
	segmentIndexManager->setSegmentIndex(
			Property::typeId,
			{{1, 1, missingOffset}, {2, 2, missingOffset + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE)}});

	size_t allVisited = 0;
	EXPECT_TRUE((detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.short_all",
			[](const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				allVisited += state.visitedSegments;
			})));
	EXPECT_EQ(allVisited, 0U);

	const std::vector<SegmentWorkItem> work{{0}, {1}};
	size_t targetedVisited = 0;
	EXPECT_TRUE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.short_work",
			work,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				targetedVisited += state.visitedSegments;
			})));
	EXPECT_EQ(targetedVisited, 0U);
}

TEST_F(PropertyEntitySegmentScannerTest, IgnoresStaleTargetedSegmentReferences) {
	addUserWithProperty("id", PropertyValue("u0"));
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	const auto &segments = dm->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segments.empty());

	const std::vector<SegmentWorkItem> staleWork{{segments.size() + 3}};
	size_t targetedVisited = 0;
	EXPECT_TRUE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			nullptr,
			"test.property_segments.stale_work",
			staleWork,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				targetedVisited += state.visitedSegments;
			})));
	EXPECT_EQ(targetedVisited, 0U);
}

TEST_F(PropertyEntitySegmentScannerTest, ReportsShortParallelCoalescedReads) {
	addUserWithProperty("id", PropertyValue("u0"));
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	const uint64_t missingOffset =
			static_cast<uint64_t>(fs::file_size(dbPath)) + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE);
	std::vector<SegmentIndexManager::SegmentIndex> fakeSegments;
	for (size_t i = 0; i < detail::kPropertyScannerMinParallelReadSegments; ++i) {
		const auto id = static_cast<int64_t>(i + 1);
		fakeSegments.push_back(
				{id, id, missingOffset + static_cast<uint64_t>(i) * static_cast<uint64_t>(TOTAL_SEGMENT_SIZE)});
	}
	const size_t fakeSegmentCount = fakeSegments.size();
	dm->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(fakeSegments));

	graph::concurrent::ThreadPool pool(2);
	EXPECT_FALSE((detail::scanAllPropertyEntitySegments<ScannerTestState>(
			*dm,
			&pool,
			"test.property_segments.short_parallel_all",
			[](const SegmentHeader &, const char *, ScannerTestState &) {},
			[](size_t, ScannerTestState &) {})));

	std::vector<SegmentWorkItem> work;
	work.reserve(fakeSegmentCount);
	for (size_t i = 0; i < fakeSegmentCount; ++i) {
		work.push_back({i});
	}

	size_t targetedVisited = 0;
	EXPECT_TRUE((detail::scanPropertyEntitySegmentWork<ScannerTestState>(
			*dm,
			&pool,
			"test.property_segments.short_parallel_work",
			work,
			[](size_t, size_t, const SegmentWorkItem &, const SegmentHeader &, const char *, ScannerTestState &state) {
				++state.visitedSegments;
			},
			[&](size_t, ScannerTestState &state) {
				targetedVisited += state.visitedSegments;
			})));
	EXPECT_EQ(targetedVisited, 0U);
}
