/**
 * @file test_PropertyIndexBuildScanner.cpp
 * @brief Tests typed scalar property scans used by property index builds.
 */

#include "DataManagerTestFixture.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "graph/core/TemporalTypes.hpp"
#include "graph/storage/data/PropertyIndexBuildScanner.hpp"

namespace {

	bool hasStringValue(const std::vector<PropertyEntityOwnerScalarKeyValue> &values,
						int64_t ownerId,
						const std::string &key,
						const std::string &expected) {
		return std::any_of(values.begin(), values.end(), [&](const PropertyEntityOwnerScalarKeyValue &value) {
			return value.ownerId == ownerId && value.key == key && value.type == PropertyType::STRING &&
				   value.stringValue == expected;
		});
	}

	bool hasIntValue(const std::vector<PropertyEntityOwnerScalarKeyValue> &values,
					int64_t ownerId,
					const std::string &key,
					int64_t expected) {
		return std::any_of(values.begin(), values.end(), [&](const PropertyEntityOwnerScalarKeyValue &value) {
			return value.ownerId == ownerId && value.key == key && value.type == PropertyType::INTEGER &&
				   value.intValue == expected;
		});
	}

	bool hasDoubleValue(const std::vector<PropertyEntityOwnerScalarKeyValue> &values,
						int64_t ownerId,
						const std::string &key,
						double expected) {
		return std::any_of(values.begin(), values.end(), [&](const PropertyEntityOwnerScalarKeyValue &value) {
			return value.ownerId == ownerId && value.key == key && value.type == PropertyType::DOUBLE &&
				   value.doubleValue == expected;
		});
	}

	bool hasBoolValue(const std::vector<PropertyEntityOwnerScalarKeyValue> &values,
					  int64_t ownerId,
					  const std::string &key,
					  bool expected) {
		return std::any_of(values.begin(), values.end(), [&](const PropertyEntityOwnerScalarKeyValue &value) {
			return value.ownerId == ownerId && value.key == key && value.type == PropertyType::BOOLEAN &&
				   value.boolValue == expected;
		});
	}

} // namespace

TEST_F(DataManagerTest, PropertyIndexBuildScannerCollectsNodeScalarsAndFiltersOwners) {
	Node user = createTestNode(dataManager, "ScannerUser");
	dataManager->addNode(user);
	dataManager->addNodeProperties(user.getId(), {{"id", PropertyValue("user-1")}});

	Node aged = createTestNode(dataManager, "ScannerUser");
	dataManager->addNode(aged);
	dataManager->addNodeProperties(aged.getId(), {{"age", PropertyValue(int64_t{31})}});

	Node scored = createTestNode(dataManager, "ScannerUser");
	dataManager->addNode(scored);
	dataManager->addNodeProperties(scored.getId(), {{"score", PropertyValue(9.5)}});

	Node active = createTestNode(dataManager, "ScannerUser");
	dataManager->addNode(active);
	dataManager->addNodeProperties(active.getId(), {{"active", PropertyValue(true)}});

	Node temporal = createTestNode(dataManager, "ScannerUser");
	dataManager->addNode(temporal);
	dataManager->addNodeProperties(temporal.getId(), {{"created", PropertyValue(TemporalDate::fromYMD(2026, 6, 11))}});

	Node post = createTestNode(dataManager, "ScannerPost");
	dataManager->addNode(post);
	dataManager->addNodeProperties(post.getId(), {{"id", PropertyValue("post-1")}});
	simulateSave();

	ASSERT_TRUE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(
			EntityType::Node, std::vector<std::string>{"id", "age", "score", "active", "created", "id"});

	EXPECT_TRUE(hasStringValue(values, user.getId(), "id", "user-1"));
	EXPECT_TRUE(hasIntValue(values, aged.getId(), "age", int64_t{31}));
	EXPECT_TRUE(hasDoubleValue(values, scored.getId(), "score", 9.5));
	EXPECT_TRUE(hasBoolValue(values, active.getId(), "active", true));
	EXPECT_TRUE(hasStringValue(values, post.getId(), "id", "post-1"));
	EXPECT_FALSE(std::any_of(values.begin(), values.end(), [](const PropertyEntityOwnerScalarKeyValue &value) {
		return value.key == "created";
	}));

	const std::vector<int64_t> ownerFilter{user.getId()};
	const auto filtered = scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"id"},
			std::span<const int64_t>(ownerFilter.data(), ownerFilter.size()));
	EXPECT_TRUE(hasStringValue(filtered, user.getId(), "id", "user-1"));
	EXPECT_FALSE(hasStringValue(filtered, post.getId(), "id", "post-1"));
}

TEST_F(DataManagerTest, PropertyIndexBuildScannerCollectsEdgeScalars) {
	Node source = createTestNode(dataManager, "ScannerSource");
	Node target = createTestNode(dataManager, "ScannerTarget");
	dataManager->addNode(source);
	dataManager->addNode(target);

	Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "SCANNER_EDGE");
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(),
								   {{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("strong")}});
	simulateSave();

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(EntityType::Edge, std::vector<std::string>{"weight", "kind"});

	EXPECT_TRUE(hasIntValue(values, edge.getId(), "weight", int64_t{7}));
	EXPECT_TRUE(hasStringValue(values, edge.getId(), "kind", "strong"));
}

TEST_F(DataManagerTest, PropertyIndexBuildScannerCollectsDirtyOverlayAndRejectsInvalidRequests) {
	Node dirty = createTestNode(dataManager, "DirtyScannerUser");
	dataManager->addNode(dirty);
	dataManager->addNodeProperties(dirty.getId(), {{"id", PropertyValue("dirty-1")}});

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	EXPECT_TRUE(scanner.canCollect(EntityType::Node));
	EXPECT_TRUE(hasStringValue(scanner.collect(EntityType::Node, std::vector<std::string>{"id"}),
							   dirty.getId(), "id", "dirty-1"));
	EXPECT_TRUE(scanner.collect(EntityType::Blob, std::vector<std::string>{"id"}).empty());
	EXPECT_TRUE(scanner.collect(EntityType::Node, std::vector<std::string>{}).empty());
}
