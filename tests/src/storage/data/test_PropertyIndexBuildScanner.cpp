/**
 * @file test_PropertyIndexBuildScanner.cpp
 * @brief Tests typed scalar property scans used by property index builds.
 */

#include "DataManagerTestFixture.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "graph/core/TemporalTypes.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"
#include "graph/storage/data/PropertyIndexBuildScanner.hpp"
#include "graph/utils/Serializer.hpp"

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

	template<typename T>
	void appendPod(std::string &out, const T &value) {
		const auto *bytes = reinterpret_cast<const char *>(&value);
		out.append(bytes, bytes + sizeof(T));
	}

	void appendSerializedKey(std::string &out, const std::string &key) {
		appendPod(out, static_cast<uint32_t>(key.size()));
		out.append(key.data(), key.size());
	}

	std::vector<char> payloadBytes(const std::string &payload) {
		return {payload.begin(), payload.end()};
	}

	void addDirtySerializedProperty(
			DataManager *dataManager,
			int64_t propertyId,
			int64_t ownerId,
			const std::string &payload) {
		Property property(propertyId, ownerId, graph::toUnderlying(EntityType::Node));
		property.setSerializedPropertyPayload(payloadBytes(payload));
		dataManager->setEntityDirty<Property>(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, property));
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

TEST_F(DataManagerTest, PropertyIndexBuildScannerReadsDirtyMapBackupsAndFiltersInactiveOwners) {
	constexpr int64_t ownerId = 707;
	Property mapBacked(2001, ownerId, graph::toUnderlying(EntityType::Node));
	mapBacked.setProperties({{"name", PropertyValue("map-backed")},
						  {"age", PropertyValue(int64_t{33})},
						  {"score", PropertyValue(4.5)},
						  {"active", PropertyValue(true)},
						  {"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("skip")})}});
	dataManager->setEntityDirty<Property>(
			graph::storage::DirtyEntityInfo<Property>(graph::storage::EntityChangeType::CHANGE_MODIFIED, mapBacked));

	Property inactive(2002, ownerId + 1, graph::toUnderlying(EntityType::Node));
	inactive.markInactive();
	inactive.setProperties({{"name", PropertyValue("inactive")}});
	dataManager->setEntityDirty<Property>(
			graph::storage::DirtyEntityInfo<Property>(graph::storage::EntityChangeType::CHANGE_MODIFIED, inactive));

	Property wrongOwnerType(2003, ownerId + 2, graph::toUnderlying(EntityType::Edge));
	wrongOwnerType.setProperties({{"name", PropertyValue("edge-owner")}});
	dataManager->setEntityDirty<Property>(
			graph::storage::DirtyEntityInfo<Property>(graph::storage::EntityChangeType::CHANGE_MODIFIED, wrongOwnerType));

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name", "age", "score", "active", "tags", "missing"});

	EXPECT_TRUE(hasStringValue(values, ownerId, "name", "map-backed"));
	EXPECT_TRUE(hasIntValue(values, ownerId, "age", int64_t{33}));
	EXPECT_TRUE(hasDoubleValue(values, ownerId, "score", 4.5));
	EXPECT_TRUE(hasBoolValue(values, ownerId, "active", true));
	EXPECT_FALSE(hasStringValue(values, ownerId + 1, "name", "inactive"));
	EXPECT_FALSE(hasStringValue(values, ownerId + 2, "name", "edge-owner"));
	EXPECT_FALSE(std::any_of(values.begin(), values.end(), [](const PropertyEntityOwnerScalarKeyValue &value) {
		return value.key == "tags" || value.key == "missing";
	}));

	const std::vector<int64_t> ownerFilter{ownerId + 10};
	EXPECT_TRUE(scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name"},
			std::span<const int64_t>(ownerFilter.data(), ownerFilter.size())).empty());
}

TEST_F(DataManagerTest, PropertyIndexBuildScannerCollectsUnsavedScalarTypesAndSkipsNonIndexableValues) {
	Node node = createTestNode(dataManager, "DirtyScalarUser");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(),
								   {{"name", PropertyValue("dirty-name")},
									{"age", PropertyValue(int64_t{42})},
									{"score", PropertyValue(12.5)},
									{"active", PropertyValue(false)},
									{"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("x")})}});

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name", "age", "score", "active", "tags"});

	EXPECT_TRUE(hasStringValue(values, node.getId(), "name", "dirty-name"));
	EXPECT_TRUE(hasIntValue(values, node.getId(), "age", int64_t{42}));
	EXPECT_TRUE(hasDoubleValue(values, node.getId(), "score", 12.5));
	EXPECT_TRUE(hasBoolValue(values, node.getId(), "active", false));
	EXPECT_FALSE(std::any_of(values.begin(), values.end(), [](const PropertyEntityOwnerScalarKeyValue &value) {
		return value.key == "tags";
	}));
}

TEST_F(DataManagerTest, PropertyIndexBuildScannerReadsSerializedDirtyPayloads) {
	const int64_t ownerId = 4242;
	Property property(1001, ownerId, graph::toUnderlying(EntityType::Node));

	std::stringstream payloadStream;
	graph::utils::Serializer::writePOD(payloadStream, static_cast<uint32_t>(12));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "name");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue("serialized-name"));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "age");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue(int64_t{45}));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "score");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue(99.5));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "active");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue(true));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "tags");
	graph::utils::Serializer::serialize<PropertyValue>(
			payloadStream,
			PropertyValue(std::vector<PropertyValue>{PropertyValue("skip-me")}));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue("ignored"));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_null");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue());
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_bool");
	graph::utils::Serializer::serialize<PropertyValue>(payloadStream, PropertyValue(false));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_map");
	graph::utils::Serializer::serialize<PropertyValue>(
			payloadStream,
			PropertyValue(PropertyValue::MapType{{"nested", PropertyValue("value")}}));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_date");
	graph::utils::Serializer::serialize<PropertyValue>(
			payloadStream, PropertyValue(TemporalDate::fromYMD(2026, 6, 12)));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_datetime");
	graph::utils::Serializer::serialize<PropertyValue>(
			payloadStream, PropertyValue(TemporalDateTime::fromComponents(2026, 6, 12, 8, 9, 10, 11)));
	graph::utils::Serializer::serialize<std::string>(payloadStream, "unrequested_duration");
	graph::utils::Serializer::serialize<PropertyValue>(
			payloadStream, PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)));

	const std::string payload = payloadStream.str();
	property.setSerializedPropertyPayload(std::vector<char>(payload.begin(), payload.end()));
	dataManager->setEntityDirty<Property>(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, property));

	Property deleted(1002, ownerId + 1, graph::toUnderlying(EntityType::Node));
	deleted.setProperties({{"name", PropertyValue("deleted")}});
	dataManager->setEntityDirty<Property>(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_DELETED, deleted));

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name", "age", "score", "active", "tags", "", "name"});

	EXPECT_TRUE(hasStringValue(values, ownerId, "name", "serialized-name"));
	EXPECT_TRUE(hasIntValue(values, ownerId, "age", int64_t{45}));
	EXPECT_TRUE(hasDoubleValue(values, ownerId, "score", 99.5));
	EXPECT_TRUE(hasBoolValue(values, ownerId, "active", true));
	EXPECT_FALSE(hasStringValue(values, ownerId + 1, "name", "deleted"));
	EXPECT_FALSE(std::any_of(values.begin(), values.end(), [](const PropertyEntityOwnerScalarKeyValue &value) {
		return value.key == "tags" || value.key == "unrequested";
	}));

	const std::vector<int64_t> missingOwner{ownerId + 10};
	EXPECT_TRUE(scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name"},
			std::span<const int64_t>(missingOwner.data(), missingOwner.size())).empty());
	EXPECT_TRUE(scanner.collect(EntityType::Node, std::vector<std::string>{"", ""}).empty());
}

TEST_F(DataManagerTest, PropertyIndexBuildScannerSkipsMalformedSerializedDirtyPayloads) {
	int64_t propertyId = 3000;
	int64_t ownerId = 9000;

	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, std::string{});

	std::string missingKey;
	appendPod(missingKey, uint32_t{1});
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, missingKey);

	std::string truncatedUnrequestedString;
	appendPod(truncatedUnrequestedString, uint32_t{1});
	appendSerializedKey(truncatedUnrequestedString, "unrequested");
	appendPod(truncatedUnrequestedString, PropertyType::STRING);
	appendPod(truncatedUnrequestedString, uint32_t{32});
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, truncatedUnrequestedString);

	std::string missingRequestedType;
	appendPod(missingRequestedType, uint32_t{1});
	appendSerializedKey(missingRequestedType, "name");
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, missingRequestedType);

	std::string truncatedBool;
	appendPod(truncatedBool, uint32_t{1});
	appendSerializedKey(truncatedBool, "active");
	appendPod(truncatedBool, PropertyType::BOOLEAN);
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, truncatedBool);

	std::string truncatedInteger;
	appendPod(truncatedInteger, uint32_t{1});
	appendSerializedKey(truncatedInteger, "age");
	appendPod(truncatedInteger, PropertyType::INTEGER);
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, truncatedInteger);

	std::string truncatedDouble;
	appendPod(truncatedDouble, uint32_t{1});
	appendSerializedKey(truncatedDouble, "score");
	appendPod(truncatedDouble, PropertyType::DOUBLE);
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, truncatedDouble);

	std::string truncatedString;
	appendPod(truncatedString, uint32_t{1});
	appendSerializedKey(truncatedString, "name");
	appendPod(truncatedString, PropertyType::STRING);
	appendPod(truncatedString, uint32_t{8});
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, truncatedString);

	std::string unknownType;
	appendPod(unknownType, uint32_t{1});
	appendSerializedKey(unknownType, "name");
	appendPod(unknownType, static_cast<PropertyType>(255));
	addDirtySerializedProperty(dataManager.get(), propertyId++, ownerId++, unknownType);

	const graph::storage::PropertyIndexBuildScanner scanner(*dataManager);
	const auto values = scanner.collect(
			EntityType::Node,
			std::vector<std::string>{"name", "active", "age", "score"});

	EXPECT_TRUE(values.empty());
}
