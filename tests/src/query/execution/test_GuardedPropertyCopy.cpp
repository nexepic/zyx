#include <gtest/gtest.h>
#include <stdexcept>
#include "graph/query/execution/GuardedPropertyCopy.hpp"
#include "graph/query/execution/GuardedEntityCopy.hpp"
#include "graph/query/execution/GuardedRowCopy.hpp"
using namespace graph;
using namespace graph::query::execution;

TEST(GuardedPropertyCopyTest, CancelsNestedCopyWithoutChangingSourceAndRetries) {
	const PropertyValue source(PropertyValue::MapType{
		{"nested", PropertyValue(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})))},
		{"empty", PropertyValue(PropertyValue::MapType{})}});
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::copyPropertyValueGuarded(source, [&] {
		if (++checkpoints == 8U) throw std::runtime_error("cancel copy");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 8U);
	EXPECT_EQ(source.getMap().at("nested").getList().size(), 4096U);
	const auto copied = detail::copyPropertyValueGuarded(source, [] {});
	EXPECT_EQ(copied, source);
	EXPECT_NE(copied.getMap().at("nested").getList().data(), source.getMap().at("nested").getList().data());
}

TEST(GuardedPropertyCopyTest, CancelsLargeScalarBuffersAndRetriesExactPayloads) {
	const std::vector<PropertyValue> sources{
		PropertyValue(std::string(512U * 1024U, 'x')),
		PropertyValue(PropertyValue::Bytes(512U * 1024U, std::byte{0x37}))};
	for (const auto &source : sources) {
		SCOPED_TRACE(static_cast<int>(source.getType()));
		size_t checkpoints = 0U;
		EXPECT_THROW(static_cast<void>(detail::copyPropertyValueGuarded(source, [&] {
			if (++checkpoints == 4U) throw std::runtime_error("cancel scalar copy");
		})), std::runtime_error);
		EXPECT_EQ(checkpoints, 4U);
		const auto copied = detail::copyPropertyValueGuarded(source, [] {});
		EXPECT_EQ(copied, source);
		if (source.getType() == PropertyType::BYTES)
			EXPECT_NE(copied.getBytes().data(), source.getBytes().data());
		else
			EXPECT_NE(std::get<std::string>(copied.getVariant()).data(),
				std::get<std::string>(source.getVariant()).data());
	}
}

TEST(GuardedPropertyCopyTest, CancelsLargeMapKeyCopyAndPreservesEmbeddedNullsOnRetry) {
	std::string key(512U * 1024U, 'k');
	key[64U * 1024U] = '\0';
	const PropertyValue::MapType source{{key, PropertyValue(int64_t{42})}};
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::copyPropertyMapGuarded(source, [&] {
		if (++checkpoints == 4U) throw std::runtime_error("cancel map key copy");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 4U);
	EXPECT_EQ(source.at(key), PropertyValue(int64_t{42}));
	const auto copied = detail::copyPropertyMapGuarded(source, [] {});
	EXPECT_EQ(copied, source);
	EXPECT_NE(copied.begin()->first.data(), source.begin()->first.data());
}

TEST(GuardedPropertyCopyTest, CancelsWideStringVectorAndRetriesInOriginalOrder) {
	std::vector<std::string> source;
	for (size_t index = 0U; index < 4096U; ++index)
		source.push_back("label" + std::to_string(index));
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::copyPropertyStringsGuarded(source, [&] {
		if (++checkpoints == 8U) throw std::runtime_error("cancel labels copy");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 8U);
	EXPECT_EQ(source.size(), 4096U);
	EXPECT_EQ(source.front(), "label0");
	EXPECT_EQ(source.back(), "label4095");
	const auto copied = detail::copyPropertyStringsGuarded(source, [] {});
	EXPECT_EQ(copied, source);
	EXPECT_NE(copied.data(), source.data());
}

TEST(GuardedPropertyCopyTest, EntityCopiesCancelWithoutChangingIdentityOrPayloadAndRetry) {
	storage::LogicalGraphNode node{7U, {"Person"},
		{{"nested", PropertyValue(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})))}}};
	storage::LogicalGraphEdge edge{7U, 1U, 2U, "KNOWS",
		{{"binary", PropertyValue(PropertyValue::Bytes(512U * 1024U, std::byte{0x37}))}}};
	const auto verify = [](const auto &source) {
		size_t checkpoints = 0U;
		EXPECT_THROW(static_cast<void>(detail::copyLogicalEntityGuarded(source, [&] {
			if (++checkpoints == 12U) throw std::runtime_error("cancel entity copy");
		})), std::runtime_error);
		EXPECT_EQ(checkpoints, 12U);
		const auto copied = detail::copyLogicalEntityGuarded(source, [] {});
		EXPECT_EQ(copied, source);
		EXPECT_NE(copied.properties.data(), source.properties.data());
	};
	verify(node);
	verify(edge);
}

TEST(GuardedPropertyCopyTest, RowCopiesPreserveAllCachesAndCancelWithoutChangingSource) {
	LogicalPlanReadRow source;
	const PropertyValue nested(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	storage::LogicalGraphNode node;
	node.node_id = 1U;
	node.labels = {"Person"};
	node.properties = {{"payload", nested}};
	storage::LogicalGraphEdge edge;
	edge.edge_id = 2U;
	edge.source_node_id = 1U;
	edge.target_node_id = 3U;
	edge.type = "KNOWS";
	edge.properties = {{"payload", nested}};
	source.values.emplace("n", node);
	source.values.emplace("r", edge);
	source.values.emplace("list", nested);
	source.values.emplace("ref", storage::EntityRef{3U, false, 0U, 0U, 0U});
	source.property_cache["n"].emplace("payload", nested);
	source.entity_metadata_cache["n"].emplace("labels", PropertyValue("Person"));
	source.complete_entity_property_cache.insert("n");
	source.path_cache["p"].nodes.push_back(node);
	source.path_cache["p"].edges.push_back(edge);
	source.path_cache["p"].node_refs.push_back(storage::EntityRef{3U, false, 0U, 0U, 0U});
	source.path_cache["p"].edge_refs.push_back(storage::EntityRef{2U, true, 1U, 3U, 0U});
	const auto original = source;
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::copyLogicalReadRowGuarded(source, [&] {
		if (++checkpoints == 12U) throw std::runtime_error("cancel row copy");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 12U);
	EXPECT_EQ(source, original);
	const auto copied = detail::copyLogicalReadRowGuarded(source, [] {});
	EXPECT_EQ(copied, source);
	EXPECT_NE(std::get<PropertyValue>(copied.values.at("list")).getList().data(),
		std::get<PropertyValue>(source.values.at("list")).getList().data());
	EXPECT_NE(copied.property_cache.at("n").at("payload").getList().data(),
		source.property_cache.at("n").at("payload").getList().data());
	EXPECT_NE(copied.path_cache.at("p").nodes.data(), source.path_cache.at("p").nodes.data());
}

TEST(GuardedPropertyCopyTest, PathCopiesCancelInsideNestedEntityAndRetryReferences) {
	LogicalPlanReadPath source;
	storage::LogicalGraphNode node;
	node.node_id = 1U;
	node.properties = {{"payload", PropertyValue(std::vector<PropertyValue>(4096U, PropertyValue{}))}};
	source.nodes.push_back(node);
	source.node_refs.push_back(storage::EntityRef{3U, false, 0U, 0U, 0U});
	source.edge_refs.push_back(storage::EntityRef{2U, true, 1U, 3U, 0U});
	const auto original = source;
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::copyLogicalReadPathGuarded(source, [&] {
		if (++checkpoints == 12U) throw std::runtime_error("cancel path copy");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 12U);
	EXPECT_EQ(source, original);
	const auto copy = detail::copyLogicalReadPathGuarded(source, [] {});
	EXPECT_EQ(copy, source);
	EXPECT_NE(copy.nodes.front().properties.front().value.getList().data(),
		source.nodes.front().properties.front().value.getList().data());
}

TEST(GuardedPropertyCopyTest, RejectsExcessiveNestingAndCopiesTheDepthBoundary) {
    PropertyValue source(int64_t{42});
    for (size_t depth = 0U; depth < 64U; ++depth) {
        std::vector<PropertyValue> elements;
        elements.push_back(std::move(source));
        source = PropertyValue(std::move(elements));
    }
    const auto copied = detail::copyPropertyValueGuarded(source, [] {});
    EXPECT_EQ(copied, source);
    const PropertyValue::MapType wrapped{{"nested", source}};
    EXPECT_THROW(static_cast<void>(detail::copyPropertyMapGuarded(wrapped, [] {})), std::length_error);
    std::vector<PropertyValue> elements;
    elements.push_back(std::move(source));
    const PropertyValue excessive(std::move(elements));
    EXPECT_THROW(static_cast<void>(detail::copyPropertyValueGuarded(excessive, [] {})), std::length_error);
    EXPECT_EQ(excessive.getList().size(), 1U);
    EXPECT_EQ(detail::copyPropertyValueGuarded(PropertyValue(int64_t{7}), [] {}), PropertyValue(int64_t{7}));
}
