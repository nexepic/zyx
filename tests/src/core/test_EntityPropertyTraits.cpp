/**
 * @file test_EntityPropertyTraits.cpp
 * @author Nexepic
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
	EntityPropertyTraits<graph::Node>::setPropertyEntityId(node, 42, graph::PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_TRUE(EntityPropertyTraits<graph::Node>::hasPropertyEntity(node));
	EXPECT_EQ(EntityPropertyTraits<graph::Node>::getPropertyEntityId(node), 42);
	EXPECT_EQ(EntityPropertyTraits<graph::Node>::getPropertyStorageType(node), graph::PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::addProperty(node, "k", graph::PropertyValue(1)));
	EXPECT_TRUE(EntityPropertyTraits<graph::Node>::hasProperty(node, "k"));
	auto nodeProps = EntityPropertyTraits<graph::Node>::takeProperties(node);
	EXPECT_EQ(nodeProps.size(), 1U);
	EXPECT_FALSE(EntityPropertyTraits<graph::Node>::hasProperty(node, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::addProperty(node, "k", graph::PropertyValue(1)));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::removeProperty(node, "k"));
	EXPECT_FALSE(EntityPropertyTraits<graph::Node>::hasProperty(node, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::clearProperties(node));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Node>::getProperties(node));
}

TEST(EntityPropertyTraitsTest, EdgeSupportsProperties) {
	graph::Edge edge;
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::supportsProperties);
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::supportsExternalProperties);
	EntityPropertyTraits<graph::Edge>::setPropertyEntityId(edge, 84, graph::PropertyStorageType::BLOB_ENTITY);
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::hasPropertyEntity(edge));
	EXPECT_EQ(EntityPropertyTraits<graph::Edge>::getPropertyEntityId(edge), 84);
	EXPECT_EQ(EntityPropertyTraits<graph::Edge>::getPropertyStorageType(edge), graph::PropertyStorageType::BLOB_ENTITY);
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::addProperty(edge, "k", graph::PropertyValue(2)));
	EXPECT_TRUE(EntityPropertyTraits<graph::Edge>::hasProperty(edge, "k"));
	auto edgeProps = EntityPropertyTraits<graph::Edge>::takeProperties(edge);
	EXPECT_EQ(edgeProps.size(), 1U);
	EXPECT_FALSE(EntityPropertyTraits<graph::Edge>::hasProperty(edge, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::addProperty(edge, "k", graph::PropertyValue(2)));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::removeProperty(edge, "k"));
	EXPECT_FALSE(EntityPropertyTraits<graph::Edge>::hasProperty(edge, "k"));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::clearProperties(edge));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Edge>::getProperties(edge));
}

TEST(EntityPropertyTraitsTest, NonPropertyEntitiesIgnoreExternalPropertyEntityMutators) {
	graph::Property prop;
	graph::Blob blob;
	graph::Index idx;
	graph::State state;

	EXPECT_NO_THROW(EntityPropertyTraits<graph::Property>::setPropertyEntityId(
			prop, 1, graph::PropertyStorageType::PROPERTY_ENTITY));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Blob>::setPropertyEntityId(
			blob, 2, graph::PropertyStorageType::BLOB_ENTITY));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::Index>::setPropertyEntityId(
			idx, 3, graph::PropertyStorageType::PROPERTY_ENTITY));
	EXPECT_NO_THROW(EntityPropertyTraits<graph::State>::setPropertyEntityId(
			state, 4, graph::PropertyStorageType::BLOB_ENTITY));
	EXPECT_FALSE(EntityPropertyTraits<graph::Property>::hasPropertyEntity(prop));
	EXPECT_FALSE(EntityPropertyTraits<graph::Blob>::hasPropertyEntity(blob));
	EXPECT_FALSE(EntityPropertyTraits<graph::Index>::hasPropertyEntity(idx));
	EXPECT_FALSE(EntityPropertyTraits<graph::State>::hasPropertyEntity(state));
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
