/**
 * @file test_DataManager_PropertyOwnerValueScan.cpp
 * @brief Tests for property-owner value collection used by bulk index builds.
 */

#include "DataManagerTestFixture.hpp"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace {
	bool hasOwnerKeyValue(const std::vector<PropertyEntityOwnerKeyValue> &values,
						 int64_t ownerId,
						 const std::string &key,
						 const PropertyValue &value) {
		return std::any_of(values.begin(), values.end(), [&](const PropertyEntityOwnerKeyValue &entry) {
			return entry.ownerId == ownerId && entry.key == key && entry.value == value;
		});
	}
}

TEST_F(DataManagerTest, BulkCollectPropertyValuesByOwnerTypeReturnsMultipleRequestedKeys) {
	std::vector<Node> nodes;
	nodes.reserve(2);

	Node user;
	user.setLabelId(dataManager->getOrCreateTokenId("User"));
	user.setProperties({{"id", PropertyValue("user-1")},
						{"country", PropertyValue("CN")},
						{"age", PropertyValue(int64_t{31})}});
	nodes.push_back(std::move(user));

	Node post;
	post.setLabelId(dataManager->getOrCreateTokenId("Post"));
	post.setProperties({{"id", PropertyValue("post-1")}, {"score", PropertyValue(9.5)}});
	nodes.push_back(std::move(post));

	dataManager->addNodes(nodes);
	const int64_t userId = nodes[0].getId();
	const int64_t postId = nodes[1].getId();
	simulateSave();

	const auto values = dataManager->bulkCollectPropertyValuesByOwnerType(
			EntityType::Node, std::vector<std::string>{"id", "country", "missing", "id"});

	EXPECT_TRUE(hasOwnerKeyValue(values, userId, "id", PropertyValue("user-1")));
	EXPECT_TRUE(hasOwnerKeyValue(values, userId, "country", PropertyValue("CN")));
	EXPECT_TRUE(hasOwnerKeyValue(values, postId, "id", PropertyValue("post-1")));
	EXPECT_FALSE(hasOwnerKeyValue(values, postId, "country", PropertyValue("CN")));
	EXPECT_EQ(std::count_if(values.begin(), values.end(), [](const PropertyEntityOwnerKeyValue &entry) {
			  return entry.key == "missing";
		  }),
		  0);
}

TEST_F(DataManagerTest, BulkCollectPropertyValuesByOwnerTypeRejectsDirtyOrInvalidRequests) {
	Node node;
	node.setLabelId(dataManager->getOrCreateTokenId("DirtyUser"));
	node.setProperties({{"id", PropertyValue("dirty-1")}});
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"id", PropertyValue("dirty-1")}});

	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(
			EntityType::Node, std::vector<std::string>{"id"}).empty());
	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(
			EntityType::Blob, std::vector<std::string>{"id"}).empty());
	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(
			EntityType::Node, std::vector<std::string>{}).empty());
}
