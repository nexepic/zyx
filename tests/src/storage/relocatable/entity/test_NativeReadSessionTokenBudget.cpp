/**
 * @file test_NativeReadSessionTokenBudget.cpp
 * @brief Logical retained-name budgets for snapshot-bound token caches.
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#include <gtest/gtest.h>
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"

using graph::storage::NativeReadSession;

TEST(NativeReadSessionTokenBudgetTest, SchemaCachesRejectSpareCapacityAndRetainSmallDefinitions) {
	const size_t cap = NativeReadSession::MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES;
	const auto verify = [&]<typename Definition>(auto put, auto get) {
		std::vector<Definition> definitions(1U);
		definitions.reserve(cap / sizeof(Definition) + 1U);
		put(std::move(definitions));
		EXPECT_EQ(get(), nullptr);
		definitions = std::vector<Definition>(1U);
		definitions.front().name = "small";
		definitions.front().name.reserve(cap + 1U);
		put(std::move(definitions));
		EXPECT_EQ(get(), nullptr);
		definitions = std::vector<Definition>(1U);
		definitions.front().property_names.reserve(cap / sizeof(std::string) + 1U);
		put(std::move(definitions));
		EXPECT_EQ(get(), nullptr);
		definitions = std::vector<Definition>(1U);
		definitions.front().property_names.emplace_back("key");
		definitions.front().property_names.front().reserve(cap + 1U);
		put(std::move(definitions));
		EXPECT_EQ(get(), nullptr);
		definitions = std::vector<Definition>(1U);
		definitions.front().name = "small";
		definitions.front().property_names = {"key"};
		put(std::move(definitions));
		ASSERT_NE(get(), nullptr);
		EXPECT_EQ(get()->front().name, "small");
		EXPECT_EQ(get()->front().property_names, (std::vector<std::string>{"key"}));
	};
	NativeReadSession session;
	verify.template operator()<graph::storage::IndexDefinition>(
		[&](auto definitions) { session.putIndexDefinitions(std::move(definitions)); },
		[&] { return session.indexDefinitions(64U); });
	verify.template operator()<graph::storage::ConstraintDefinition>(
		[&](auto definitions) { session.putConstraintDefinitions(std::move(definitions)); },
		[&] { return session.constraintDefinitions(64U); });
}

TEST(NativeReadSessionTokenBudgetTest, IndexMetricCapacityCannotBypassSchemaCacheBudget) {
	NativeReadSession session;
	std::vector<graph::storage::IndexDefinition> definitions(1U);
	definitions.front().vector_metric = "cosine";
	definitions.front().vector_metric.reserve(NativeReadSession::MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES + 1U);
	session.putIndexDefinitions(std::move(definitions));
	EXPECT_EQ(session.indexDefinitions(64U), nullptr);
	definitions = std::vector<graph::storage::IndexDefinition>(1U);
	definitions.front().vector_metric = "cosine";
	session.putIndexDefinitions(std::move(definitions));
	ASSERT_NE(session.indexDefinitions(64U), nullptr);
	EXPECT_EQ(session.indexDefinitions(64U)->front().vector_metric, "cosine");
}

TEST(NativeReadSessionTokenBudgetTest, NamedTokenNamespacesAdmitBeforeCopyAndRefundEvictedNames) {
	const size_t cap = NativeReadSession::MAXIMUM_CACHED_TOKEN_NAME_BYTES;
	const std::string first(cap / 4U, 'a');
	const std::string second(cap / 4U, 'b');
	const std::string full(cap / 2U, 'c');
	const std::string oversized(cap / 2U + 1U, 'd');
	const auto verify = [&](auto put, auto get) {
		put(first, 1U);
		put(second, 2U);
		ASSERT_TRUE(get(first).has_value());
		ASSERT_TRUE(get(second).has_value());
		// Updates to hits/misses neither copy the key nor charge it twice.
		put(first, std::nullopt);
		ASSERT_TRUE(get(first).has_value());
		EXPECT_FALSE(get(first)->has_value());
		put(oversized, 3U);
		EXPECT_FALSE(get(oversized).has_value());
		EXPECT_TRUE(get(first).has_value());
		EXPECT_TRUE(get(second).has_value());
		// Both map and FIFO own the key, so a half-cap name fills this cache.
		put(full, 4U);
		EXPECT_FALSE(get(first).has_value());
		EXPECT_FALSE(get(second).has_value());
		ASSERT_TRUE(get(full).has_value());
		EXPECT_EQ(**get(full), 4U);
		put(first, 5U);
		EXPECT_FALSE(get(full).has_value());
		put(second, 6U);
		ASSERT_TRUE(get(first).has_value());
		ASSERT_TRUE(get(second).has_value());
		EXPECT_EQ(**get(first), 5U);
		EXPECT_EQ(**get(second), 6U);
	};
	NativeReadSession session;
	verify([&](std::string_view name, std::optional<uint32_t> token) { session.putPropertyToken(name, token); },
			[&](std::string_view name) { return session.propertyToken(name); });
	verify([&](std::string_view name, std::optional<uint64_t> token) { session.putNodeLabelToken(name, token); },
			[&](std::string_view name) { return session.nodeLabelToken(name); });
	verify([&](std::string_view name, std::optional<uint64_t> token) { session.putRelationshipTypeToken(name, token); },
			[&](std::string_view name) { return session.relationshipTypeToken(name); });
	// Namespace budgets are independent; later label/type insertions keep properties.
	EXPECT_TRUE(session.propertyToken(first).has_value());
	EXPECT_TRUE(session.nodeLabelToken(first).has_value());
	EXPECT_TRUE(session.relationshipTypeToken(first).has_value());
}

TEST(NativeReadSessionTokenBudgetTest, TokenNameReplacementEvictsOtherKindsAndRefundsOldPayload) {
	using Kind = NativeReadSession::TokenNameKind;
	const size_t cap = NativeReadSession::MAXIMUM_CACHED_TOKEN_NAME_BYTES;
	const std::string half(cap / 2U, 'a');
	const std::string full(cap, 'b');
	NativeReadSession session;
	session.putTokenName(Kind::TNK_PROPERTY_KEY, 1U, half);
	session.putTokenName(Kind::TNK_NODE_LABEL, 1U, half);
	session.putTokenName(Kind::TNK_PROPERTY_KEY, 1U, full);
	EXPECT_EQ(session.tokenName(Kind::TNK_PROPERTY_KEY, 1U), full);
	EXPECT_FALSE(session.tokenName(Kind::TNK_NODE_LABEL, 1U).has_value());
	// Shrinking a retained value restores room for another namespace.
	session.putTokenName(Kind::TNK_PROPERTY_KEY, 1U, half);
	session.putTokenName(Kind::TNK_RELATIONSHIP_TYPE, 1U, half);
	EXPECT_TRUE(session.tokenName(Kind::TNK_PROPERTY_KEY, 1U).has_value());
	EXPECT_TRUE(session.tokenName(Kind::TNK_RELATIONSHIP_TYPE, 1U).has_value());
	const std::string oversized(cap + 1U, 'c');
	session.putTokenName(Kind::TNK_PROPERTY_KEY, 1U, oversized);
	EXPECT_FALSE(session.tokenName(Kind::TNK_PROPERTY_KEY, 1U).has_value());
	EXPECT_TRUE(session.tokenName(Kind::TNK_RELATIONSHIP_TYPE, 1U).has_value());
	EXPECT_EQ(oversized.size(), cap + 1U);
	session.putTokenName(Kind::TNK_NODE_LABEL, 2U, half);
	EXPECT_TRUE(session.tokenName(Kind::TNK_NODE_LABEL, 2U).has_value());
	EXPECT_TRUE(session.tokenName(Kind::TNK_RELATIONSHIP_TYPE, 1U).has_value());
	// Rejection refunds the removed value, allowing a full-cap retry.
	session.putTokenName(Kind::TNK_PROPERTY_KEY, 1U, full);
	EXPECT_EQ(session.tokenName(Kind::TNK_PROPERTY_KEY, 1U), full);
	EXPECT_FALSE(session.tokenName(Kind::TNK_NODE_LABEL, 2U).has_value());
	EXPECT_FALSE(session.tokenName(Kind::TNK_RELATIONSHIP_TYPE, 1U).has_value());
}

TEST(NativeReadSessionTokenBudgetTest, TokenNameUpdatesPreserveFifoAndEntryCap) {
	using Kind = NativeReadSession::TokenNameKind;
	NativeReadSession session;
	for (uint64_t id = 1U; id <= NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES; ++id) {
		session.putTokenName(Kind::TNK_NODE_LABEL, id, "label");
	}
	session.putTokenName(Kind::TNK_NODE_LABEL, 1U, "replacement");
	session.putTokenName(Kind::TNK_NODE_LABEL, 129U, "last");
	EXPECT_FALSE(session.tokenName(Kind::TNK_NODE_LABEL, 1U).has_value());
	EXPECT_EQ(session.tokenName(Kind::TNK_NODE_LABEL, 2U), "label");
	EXPECT_EQ(session.tokenName(Kind::TNK_NODE_LABEL, 129U), "last");
}

TEST(NativeReadSessionTokenBudgetTest, CachedNameCopyAdmitsBeforeAllocationAndPreservesCacheOnRejection) {
	NativeReadSession session;
	const std::string name(4096U, 'x');
	for (const auto kind: {NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,
		NativeReadSession::TokenNameKind::TNK_NODE_LABEL, NativeReadSession::TokenNameKind::TNK_RELATIONSHIP_TYPE}) {
		session.putTokenName(kind, 41U, name);
		size_t admittedBytes = 0U;
		EXPECT_THROW(static_cast<void>(session.tokenName(kind, 41U, [&](size_t bytes) {
			admittedBytes = bytes;
			throw std::runtime_error("owned token copy rejected");
		})), std::runtime_error);
		EXPECT_GE(admittedBytes, name.size());
		EXPECT_EQ(session.tokenName(kind, 41U), name);
		auto copied = session.tokenName(kind, 41U, [&](size_t bytes) { admittedBytes = bytes; });
		ASSERT_TRUE(copied.has_value());
		EXPECT_EQ(*copied, name);
		copied->front() = 'y';
		EXPECT_EQ(session.tokenName(kind, 41U), name);
		size_t missingCalls = 0U;
		EXPECT_FALSE(session.tokenName(kind, 42U, [&](size_t) { ++missingCalls; }).has_value());
		EXPECT_EQ(missingCalls, 0U);
	}
}

TEST(NativeReadSessionTokenBudgetTest, DirectPropertyAdmissionDoesNotConsumeOversizedRvalues) {
	NativeReadSession session;
	graph::PropertyValue oversized(std::string(NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U, 'x'));
	const auto &source = std::get<std::string>(oversized.getVariant());
	const auto *sourceData = source.data();
	session.putProperty(false, 1U, 1U, std::move(oversized));
	EXPECT_FALSE(session.property(false, 1U, 1U).has_value());
	EXPECT_EQ(std::get<std::string>(oversized.getVariant()).size(), NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U);
	EXPECT_EQ(std::get<std::string>(oversized.getVariant()).data(), sourceData);
	graph::PropertyValue small(std::string(256U, 's'));
	session.putProperty(false, 1U, 1U, small);
	const auto retained = session.property(false, 1U, 1U);
	ASSERT_TRUE(retained && retained->has_value());
	EXPECT_EQ(**retained, small);
	EXPECT_NE(std::get<std::string>((**retained).getVariant()).data(), std::get<std::string>(small.getVariant()).data());
	session.putProperty(false, 1U, 2U, graph::PropertyValue());
	const auto nullValue = session.property(false, 1U, 2U);
	ASSERT_TRUE(nullValue && nullValue->has_value());
	EXPECT_EQ((**nullValue).getType(), graph::PropertyType::NULL_TYPE);
}

TEST(NativeReadSessionTokenBudgetTest, PropertyAdmissionChargesSpareListAndByteCapacity) {
	NativeReadSession session;
	std::vector<graph::PropertyValue> list{graph::PropertyValue(int64_t{7})};
	list.reserve(NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES / sizeof(graph::PropertyValue) + 1U);
	graph::PropertyValue listValue(std::move(list));
	const auto *listData = listValue.getList().data();
	session.putProperty(false, 1U, 1U, std::move(listValue));
	EXPECT_FALSE(session.property(false, 1U, 1U).has_value());
	EXPECT_EQ(listValue.getList().size(), 1U);
	EXPECT_EQ(listValue.getList().data(), listData);
	graph::PropertyValue::Bytes bytes{std::byte{7}};
	bytes.reserve(NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U);
	graph::PropertyValue bytesValue(std::move(bytes));
	const auto *byteData = std::get<graph::PropertyValue::Bytes>(bytesValue.getVariant()).data();
	session.putProperty(true, 1U, 1U, std::move(bytesValue));
	EXPECT_FALSE(session.property(true, 1U, 1U).has_value());
	EXPECT_EQ(std::get<graph::PropertyValue::Bytes>(bytesValue.getVariant()).size(), 1U);
	EXPECT_EQ(std::get<graph::PropertyValue::Bytes>(bytesValue.getVariant()).data(), byteData);
}

TEST(NativeReadSessionTokenBudgetTest, PropertyCopiesChargeActualCapacityAndRejectSpareMapBuckets) {
	NativeReadSession session;
	const size_t cap = NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES;
	std::vector<graph::PropertyValue> entries{graph::PropertyValue(int64_t{7})};
	entries.reserve(cap / (2U * sizeof(graph::PropertyValue)));
	const graph::PropertyValue source(std::move(entries));
	session.putProperty(false, 1U, 1U, source);
	session.putProperty(false, 1U, 2U, graph::PropertyValue(std::string(cap - 1024U, 's')));
	EXPECT_TRUE(session.property(false, 1U, 1U).has_value());
	EXPECT_TRUE(session.property(false, 1U, 2U).has_value());
	graph::PropertyValue::MapType map;
	map.rehash(cap / sizeof(void *) + 1U);
	graph::PropertyValue mapValue(std::move(map));
	const auto buckets = mapValue.getMap().bucket_count();
	session.putProperty(true, 2U, 3U, std::move(mapValue));
	EXPECT_FALSE(session.property(true, 2U, 3U).has_value());
	EXPECT_EQ(mapValue.getMap().bucket_count(), buckets);
}
