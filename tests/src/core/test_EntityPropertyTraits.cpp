/**
 * @file test_EntityPropertyTraits.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"

using namespace graph::storage;

TEST(EntityPropertyTraitsTest, NodeSupportsProperties) {
	graph::Node node;
	EXPECT_TRUE(EntityPropertyTraits<graph::Node>::supportsProperties);
	EXPECT_TRUE(EntityPropertyTraits<graph::Node>::supportsExternalProperties);
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::addProperty(node, "k", graph::PropertyValue(1)));
	EXPECT_TRUE(EntityPropertyTraits<graph::Node>::hasProperty(node, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::removeProperty(node, "k"));
	EXPECT_FALSE(EntityPropertyTraits<graph::Node>::hasProperty(node, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::clearProperties(node));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::getProperties(node));
}

TEST(EntityPropertyTraitsTest, EdgeSupportsProperties) {
	graph::Edge edge;
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::supportsProperties);
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::supportsExternalProperties);
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::addProperty(edge, "k", graph::PropertyValue(2)));
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::hasProperty(edge, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::removeProperty(edge, "k"));
	EXPECT_FALSE(EntityPropertyTraits<graph::Edge>::hasProperty(edge, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::clearProperties(edge));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::getProperties(edge));
}

TEST(EntityPropertyTraitsTest, PropertyDoesNotSupportProperties) {
	graph::Property prop;
	EXPECT_FALSE(EntityPropertyTraits<graph::Property>::supportsProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Property>::supportsExternalProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Property>::hasPropertyEntity(prop));
	EXPECT_EQ(EntityPropertyTraits<graph::Property>::getPropertyStorageType(prop), graph::PropertyStorageType::NONE);
	EXPECT_EQ(EntityPropertyTraits<graph::Property>::getPropertyEntityId(prop), 0);
	EXPECT_FALSE(EntityPropertyTraits<graph::Property>::hasProperty(prop, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Property>::removeProperty(prop, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Property>::clearProperties(prop));
	EXPECT_TRUE(EntityPropertyTraits<graph::Property>::getProperties(prop).empty());
	EXPECT_THROW(EntityPropertyTraits<graph::Property>::addProperty(prop, "k", graph::PropertyValue(1)),
				 std::runtime_error);
}

TEST(EntityPropertyTraitsTest, BlobDoesNotSupportProperties) {
	graph::Blob blob;
	EXPECT_FALSE(EntityPropertyTraits<graph::Blob>::supportsProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Blob>::supportsExternalProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Blob>::hasPropertyEntity(blob));
	EXPECT_EQ(EntityPropertyTraits<graph::Blob>::getPropertyStorageType(blob), graph::PropertyStorageType::NONE);
	EXPECT_EQ(EntityPropertyTraits<graph::Blob>::getPropertyEntityId(blob), 0);
	EXPECT_FALSE(EntityPropertyTraits<graph::Blob>::hasProperty(blob, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Blob>::removeProperty(blob, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Blob>::clearProperties(blob));
	EXPECT_TRUE(EntityPropertyTraits<graph::Blob>::getProperties(blob).empty());
	EXPECT_THROW(EntityPropertyTraits<graph::Blob>::addProperty(blob, "k", graph::PropertyValue(1)),
				 std::runtime_error);
}

TEST(EntityPropertyTraitsTest, IndexDoesNotSupportProperties) {
	graph::Index idx;
	EXPECT_FALSE(EntityPropertyTraits<graph::Index>::supportsProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Index>::supportsExternalProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::Index>::hasPropertyEntity(idx));
	EXPECT_EQ(EntityPropertyTraits<graph::Index>::getPropertyStorageType(idx), graph::PropertyStorageType::NONE);
	EXPECT_EQ(EntityPropertyTraits<graph::Index>::getPropertyEntityId(idx), 0);
	EXPECT_FALSE(EntityPropertyTraits<graph::Index>::hasProperty(idx, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Index>::removeProperty(idx, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Index>::clearProperties(idx));
	EXPECT_TRUE(EntityPropertyTraits<graph::Index>::getProperties(idx).empty());
	EXPECT_THROW(EntityPropertyTraits<graph::Index>::addProperty(idx, "k", graph::PropertyValue(1)),
				 std::runtime_error);
}

TEST(EntityPropertyTraitsTest, StateDoesNotSupportProperties) {
	graph::State state;
	EXPECT_FALSE(EntityPropertyTraits<graph::State>::supportsProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::State>::supportsExternalProperties);
	EXPECT_FALSE(EntityPropertyTraits<graph::State>::hasPropertyEntity(state));
	EXPECT_EQ(EntityPropertyTraits<graph::State>::getPropertyStorageType(state), graph::PropertyStorageType::NONE);
	EXPECT_EQ(EntityPropertyTraits<graph::State>::getPropertyEntityId(state), 0);
	EXPECT_FALSE(EntityPropertyTraits<graph::State>::hasProperty(state, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::State>::removeProperty(state, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::State>::clearProperties(state));
	EXPECT_TRUE(EntityPropertyTraits<graph::State>::getProperties(state).empty());
	EXPECT_THROW(EntityPropertyTraits<graph::State>::addProperty(state, "k", graph::PropertyValue(1)),
				 std::runtime_error);
}
