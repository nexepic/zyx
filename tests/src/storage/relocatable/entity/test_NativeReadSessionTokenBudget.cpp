/**
 * @file test_NativeReadSessionTokenBudget.cpp
 * @brief Logical retained-name budgets for snapshot-bound token caches.
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#include <gtest/gtest.h>
#include "graph/utils/OwnedPmrObject.hpp"
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
		if constexpr (requires { definitions.front().property_names.reserveString(0U, cap + 1U); }) {
            definitions.front().property_names.reserveString(0U, cap + 1U);
        } else {
            definitions.front().property_names.front().reserve(cap + 1U);
        }
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

namespace {
class SessionCountingResource final : public std::pmr::memory_resource {
public:
 size_t live = 0U;
 size_t allocations = 0U;
 size_t fail_after = (std::numeric_limits<size_t>::max)();
 bool deny_admission = false;
 size_t denied_allocations = 0U;
private:
 void *do_allocate(size_t bytes, size_t alignment) override {
  if (allocations >= fail_after) {
   ++denied_allocations;
   if (deny_admission) throw graph::query::execution::detail::ResourceAdmissionDenied();
   throw std::bad_alloc();
  }
  void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
  ++allocations; live += bytes; return result;
 }
 void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
  std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); live -= bytes;
 }
 bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(NativeReadSessionTokenBudgetTest, OwnedLabelsAndTokenNamesChargePayloadAndReleaseTogether) {
 auto memory = std::make_shared<SessionCountingResource>();
 {
  NativeReadSession session(memory);
  const size_t baseline = memory->live;
  const std::string name(4096U, 'n');
  session.putPropertyToken(name, 9U);
  EXPECT_GE(memory->live, baseline + 2U * (name.size() + 1U));
  const size_t beforeName = memory->live;
  session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 9U, name);
  EXPECT_GE(memory->live, beforeName + name.size() + 1U);
  const size_t beforeLabels = memory->live;
  std::vector<uint64_t> labels(1000U);
  for (size_t i = 0U; i < labels.size(); ++i) labels[i] = i + 1U;
  session.putNodeLabels(1U, labels);
  EXPECT_GE(memory->live, beforeLabels + labels.size() * sizeof(uint64_t));
  EXPECT_EQ(session.nodeLabels(1U), labels);
  EXPECT_EQ(session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 9U), name);
 }
 EXPECT_EQ(memory->live, 0U);
}

TEST(NativeReadSessionTokenBudgetTest, FailedConstructionRefundsEveryPartialContainer) {
 for (size_t failure = 0U; failure < 24U; ++failure) {
  auto memory = std::make_shared<SessionCountingResource>();
  memory->fail_after = failure;
  try { NativeReadSession session(memory); } catch (const std::bad_alloc &) {}
  EXPECT_EQ(memory->live, 0U) << failure;
 }
}

TEST(NativeReadSessionTokenBudgetTest, FailedNamedTokenInsertionLeavesNoPhantomHitAndCanRetry) {
 auto memory = std::make_shared<SessionCountingResource>();
 NativeReadSession session(memory);
 session.putPropertyToken("retained", 1U);
 const std::string name(4096U, 'x');
 memory->fail_after = memory->allocations + 2U;
 EXPECT_THROW(session.putPropertyToken(name, 2U), std::bad_alloc);
 EXPECT_FALSE(session.propertyToken(name).has_value());
 ASSERT_TRUE(session.propertyToken("retained").has_value());
 EXPECT_EQ(**session.propertyToken("retained"), 1U);
 memory->fail_after = (std::numeric_limits<size_t>::max)();
 session.putPropertyToken(name, 2U);
 ASSERT_TRUE(session.propertyToken(name).has_value());
 EXPECT_EQ(**session.propertyToken(name), 2U);
}

namespace {
struct SessionObjectBase { virtual ~SessionObjectBase() = default; };
struct SessionOwnedObject final : SessionObjectBase, graph::utils::OwnedPmrObject {
 explicit SessionOwnedObject(bool fail = false) { if (fail) throw std::runtime_error("constructor fault"); }
 alignas(std::max_align_t) std::byte data[64]{};
};
static_assert(alignof(SessionOwnedObject) <= alignof(std::max_align_t));
}
TEST(NativeReadSessionTokenBudgetTest, OwnedObjectSurvivesOwnerReleaseAndDeletesThroughBase) {
 auto memory = std::make_shared<SessionCountingResource>();
 std::weak_ptr<SessionCountingResource> weak = memory;
 std::unique_ptr<SessionObjectBase> object(new (memory) SessionOwnedObject);
 EXPECT_GE(memory->live, sizeof(SessionOwnedObject));
 EXPECT_EQ(reinterpret_cast<uintptr_t>(object.get()) % alignof(SessionOwnedObject), 0U);
 memory.reset();
 EXPECT_FALSE(weak.expired());
 object.reset();
 EXPECT_TRUE(weak.expired());
}
TEST(NativeReadSessionTokenBudgetTest, OwnedObjectConstructorFailureRefundsPrefixAndBody) {
 auto memory = std::make_shared<SessionCountingResource>();
 EXPECT_THROW(new (memory) SessionOwnedObject(true), std::runtime_error);
 EXPECT_EQ(memory->live, 0U);
 memory->fail_after = memory->allocations;
 EXPECT_THROW(new (memory) SessionOwnedObject, std::bad_alloc);
 EXPECT_EQ(memory->live, 0U);
}

TEST(NativeReadSessionTokenBudgetTest, OwnedCanonicalPropertyCodecMatchesExistingWireAndRollsBackFailure) {
 using graph::PropertyValue;
 using graph::storage::CanonicalPropertyValueCodec;
 const PropertyValue nested(PropertyValue::MapType{
  {"z", PropertyValue(std::vector<PropertyValue>{PropertyValue(), PropertyValue(true), PropertyValue(int64_t{-9})})},
  {"a", PropertyValue(std::string(4096U, 's'))},
  {std::string("\xFF", 1U), PropertyValue(PropertyValue::Bytes(1024U, std::byte{0x37}))}});
 std::vector<std::byte> expected;
 CanonicalPropertyValueCodec::append(expected, nested);
 auto memory = std::make_shared<SessionCountingResource>();
 {
  std::pmr::vector<std::byte> output(memory.get());
  output.reserve(expected.size() + 1U);
  output.push_back(std::byte{0x42});
  const size_t baseline = memory->live;
  memory->fail_after = memory->allocations;
  EXPECT_THROW(CanonicalPropertyValueCodec::append(output, nested), std::bad_alloc);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output.front(), std::byte{0x42});
  EXPECT_EQ(memory->live, baseline);
  memory->fail_after = (std::numeric_limits<size_t>::max)();
  CanonicalPropertyValueCodec::append(output, nested);
  EXPECT_EQ(output.size(), expected.size() + 1U);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin() + 1));
  // All map ordering scratch is freed; only the retained byte vector remains.
  EXPECT_EQ(memory->live, baseline);
  size_t offset = 1U;
  EXPECT_EQ(CanonicalPropertyValueCodec::decodeNext(output, offset), nested);
  EXPECT_EQ(offset, output.size());
 }
 EXPECT_EQ(memory->live, 0U);
}

TEST(NativeReadSessionTokenBudgetTest, OwnedPropertyCachePreservesNestedNullAndAbsentValues) {
 using graph::PropertyValue;
 auto memory = std::make_shared<SessionCountingResource>();
 {
  NativeReadSession session(memory);
  const size_t baseline = memory->live;
  PropertyValue nested(PropertyValue::MapType{{"list", PropertyValue(std::vector<PropertyValue>{
   PropertyValue(std::string(8192U, 'v')), PropertyValue(PropertyValue::Bytes(2048U, std::byte{0x55}))})}});
  session.putProperty(false, 1U, 1U, nested);
  EXPECT_GE(memory->live, baseline + 10240U);
  auto result = session.property(false, 1U, 1U);
  ASSERT_TRUE(result.has_value()); ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, nested);
  session.putProperty(false, 1U, 2U, PropertyValue());
  session.putProperty(false, 1U, 3U, std::optional<PropertyValue>{});
  ASSERT_TRUE(session.property(false, 1U, 2U)->has_value());
  EXPECT_EQ(**session.property(false, 1U, 2U), PropertyValue());
  ASSERT_TRUE(session.property(false, 1U, 3U).has_value());
  EXPECT_FALSE(session.property(false, 1U, 3U)->has_value());
  EXPECT_FALSE(session.property(false, 1U, 4U).has_value());
  size_t admitted = 0U;
  const graph::storage::SelectedPropertyReadAdmission admission = [&](size_t bytes) { admitted = bytes; };
  result = session.property(false, 1U, 1U, &admission);
  EXPECT_EQ(admitted, graph::storage::CanonicalPropertyValueCodec::encodedSize(nested) + sizeof(uint32_t));
 }
 EXPECT_EQ(memory->live, 0U);
}

TEST(NativeReadSessionTokenBudgetTest, DeniedOwnedPropertyReplacementPreservesOldValueAndSource) {
 using graph::PropertyValue;
 auto memory = std::make_shared<SessionCountingResource>();
 NativeReadSession session(memory);
 session.putProperty(false, 1U, 1U, PropertyValue(int64_t{7}));
 PropertyValue replacement(PropertyValue::MapType{{"key", PropertyValue(std::string(8192U, 'x'))}});
 const PropertyValue original = replacement;
 const size_t baseline = memory->live;
 // First allocation admits encoded destination; the map ordering scratch fails.
 memory->fail_after = memory->allocations + 1U;
 EXPECT_THROW(session.putProperty(false, 1U, 1U, std::move(replacement)), std::bad_alloc);
 EXPECT_EQ(replacement, original);
 EXPECT_EQ(memory->live, baseline);
 ASSERT_TRUE(session.property(false, 1U, 1U)->has_value());
 EXPECT_EQ(**session.property(false, 1U, 1U), PropertyValue(int64_t{7}));
 memory->fail_after = (std::numeric_limits<size_t>::max)();
 session.putProperty(false, 1U, 1U, replacement);
 EXPECT_EQ(**session.property(false, 1U, 1U), original);
}

namespace {
std::shared_ptr<graph::query::execution::detail::PrepaidMemoryResource> sessionGrant(size_t maximum = 1024U * 1024U) {
 using namespace graph::query::execution::detail;
 auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
 auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
 auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, maximum);
 return std::make_shared<PrepaidMemoryResource>(std::move(account), maximum);
}
}
TEST(NativeReadSessionTokenBudgetTest, NativeGrantChargesAdoptedSchemaBuffersAndReleasesOldDefinitions) {
 auto memory = sessionGrant();
 {
  NativeReadSession session(memory);
  const size_t baseline = memory->used();
  std::vector<graph::storage::IndexDefinition> definitions(1U);
  definitions.front().name = std::string(4096U, 'i');
  definitions.front().property_names = {std::string(1024U, 'p')};
  definitions.front().vector_metric = "cosine";
  using namespace graph::utils::detail;
  const size_t adopted = standardVectorHeapBufferBytes(definitions) +
   dynamicStringBufferBytes(definitions.front().name) +
   standardVectorHeapBufferBytes(definitions.front().property_names) +
   dynamicStringBufferBytes(definitions.front().property_names.front()) +
   dynamicStringBufferBytes(definitions.front().vector_metric);
  session.putIndexDefinitions(std::move(definitions));
  ASSERT_NE(session.indexDefinitions(64U), nullptr);
  EXPECT_EQ(memory->used(), baseline + adopted);
  session.putIndexDefinitions({});
  ASSERT_NE(session.indexDefinitions(64U), nullptr);
  EXPECT_TRUE(session.indexDefinitions(64U)->empty());
  EXPECT_EQ(memory->used(), baseline + graph::utils::detail::standardContainerProxyBytes());
  std::vector<graph::storage::ConstraintDefinition> constraints(1U);
  constraints.front().name = std::string(1024U, 'c');
  session.putConstraintDefinitions(std::move(constraints));
  ASSERT_NE(session.constraintDefinitions(64U), nullptr);
  EXPECT_GT(memory->used(), baseline + 1024U);
 }
 EXPECT_EQ(memory->used(), 0U);
}

TEST(NativeReadSessionTokenBudgetTest, DeniedSchemaAdoptionPreservesCachedDefinitionAndRetriesAfterPressure) {
 using graph::query::execution::detail::PrepaidMemoryResource;
 auto memory = sessionGrant(32768U);
 NativeReadSession session(memory);
 std::vector<graph::storage::IndexDefinition> original(1U);
 original.front().name = "retained";
 session.putIndexDefinitions(std::move(original));
 const size_t baseline = memory->used();
 {
  PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity() - baseline);
  std::vector<graph::storage::IndexDefinition> replacement(1U);
  replacement.front().name = std::string(4096U, 'r');
  EXPECT_NO_THROW(session.putIndexDefinitions(std::move(replacement)));
  ASSERT_NE(session.indexDefinitions(64U), nullptr);
  EXPECT_EQ(session.indexDefinitions(64U)->front().name, "retained");
 }
 EXPECT_EQ(memory->used(), baseline);
 std::vector<graph::storage::IndexDefinition> replacement(1U);
 replacement.front().name = "replacement";
 session.putIndexDefinitions(std::move(replacement));
 EXPECT_EQ(session.indexDefinitions(64U)->front().name, "replacement");
}

TEST(NativeReadSessionTokenBudgetTest, EntityPoolCopiesAreChargedThroughTheSessionResource) {
 auto memory = std::make_shared<SessionCountingResource>();
 {
  NativeReadSession session(memory);
  const size_t baseline = memory->live;
  std::vector<graph::storage::CatalogEntry> pools(1024U);
  session.putEntityPagePools(pools);
  ASSERT_NE(session.entityPagePools(), nullptr);
  EXPECT_EQ(session.entityPagePools()->size(), pools.size());
  EXPECT_GE(memory->live, baseline + pools.size() * sizeof(pools.front()));
  memory->fail_after = memory->allocations;
  EXPECT_THROW(session.putEntityPagePools(pools), std::bad_alloc);
  EXPECT_EQ(session.entityPagePools()->size(), pools.size());
 }
 EXPECT_EQ(memory->live, 0U);
}

TEST(NativeReadSessionTokenBudgetTest, UnownedObjectUsesPermanentStorageAcrossAnAmbientDefaultChange) {
 SessionCountingResource ambient;
 std::unique_ptr<SessionObjectBase> object;
 {
  struct RestoreDefault {
   std::pmr::memory_resource *previous;
   ~RestoreDefault() { std::pmr::set_default_resource(previous); }
  } restore{std::pmr::set_default_resource(&ambient)};
  object.reset(new (std::shared_ptr<std::pmr::memory_resource>{}) SessionOwnedObject);
  EXPECT_EQ(ambient.allocations, 0U);
 }
 object.reset();
 EXPECT_EQ(ambient.live, 0U);
}

#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
TEST(NativeReadSessionTokenBudgetTest, CachedNameCopyUsesSourceAdmissionAndKeepsItsOriginalSessionOwner) {
 auto memory = std::make_shared<SessionCountingResource>();
 NativeReadSession session(memory);
 EXPECT_EQ(session.memoryOwner(), memory);
 const std::string name(8192U, 'k');
 session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 17U, name);
 const auto retained = memory->live;
 auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(small);
  EXPECT_THROW(static_cast<void>(session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 17U)),
   graph::query::execution::MaterializedByteLimitExceeded);
  EXPECT_EQ(small->used(), 0U);
 }
 EXPECT_EQ(memory->live, retained);
 EXPECT_EQ(session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 17U), name);
 auto enough = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(enough);
  const auto copied = session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 17U);
  EXPECT_EQ(copied, name); EXPECT_GE(enough->used(), name.size() + 1U);
 }
 EXPECT_EQ(enough->used(), 0U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(small);
  size_t admitted = 0U;
  const auto copied = session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY, 17U,
   [&](size_t bytes) { admitted = bytes; });
  EXPECT_EQ(copied, name); EXPECT_GE(admitted, name.size());
  EXPECT_EQ(small->used(), 0U);
 }
 EXPECT_EQ(memory->live, retained);
}

TEST(NativeReadSessionTokenBudgetTest, CachedIndexCopiesReleasePrivateQueryAuthorityBeforeSessionEnds) {
    auto query = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536U);
    auto cache = sessionGrant();
    {
        NativeReadSession session(cache);
        std::optional<graph::utils::OwnedStdVector<graph::storage::IndexDefinition>> source;
        const std::string name(4096U, 'q');
        {
            graph::utils::OwnedAllocationScope scope(query);
            source.emplace(); source->emplace_back(); source->front().name = name;
        }
        ASSERT_GT(query->used(), 4096U);
        session.putIndexDefinitions(*source);
        ASSERT_NE(session.indexDefinitions(64U), nullptr);
        source.reset();
        EXPECT_EQ(query->used(), 0U);
        EXPECT_EQ(session.indexDefinitions(64U)->front().name, name);
    }
    EXPECT_EQ(cache->used(), 0U);
}

TEST(NativeReadSessionTokenBudgetTest, CachedConstraintCopiesReleasePrivateQueryAuthorityBeforeSessionEnds) {
    auto query = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536U);
    auto cache = sessionGrant();
    {
        NativeReadSession session(cache);
        std::optional<graph::utils::OwnedStdVector<graph::storage::ConstraintDefinition>> source;
        const std::string name(4096U, 'q');
        {
            graph::utils::OwnedAllocationScope scope(query);
            source.emplace(); source->emplace_back(); source->front().name = name;
        }
        ASSERT_GT(query->used(), 4096U);
        session.putConstraintDefinitions(*source);
        ASSERT_NE(session.constraintDefinitions(64U), nullptr);
        source.reset();
        EXPECT_EQ(query->used(), 0U);
        EXPECT_EQ(session.constraintDefinitions(64U)->front().name, name);
    }
    EXPECT_EQ(cache->used(), 0U);
}

TEST(NativeReadSessionTokenBudgetTest, OptionalEntityCacheDeclinesAdmissionAndRetriesAcrossFifoGrowth) {
 for (size_t allocationOffset = 0U; allocationOffset < 3U; ++allocationOffset) {
  auto memory = std::make_shared<SessionCountingResource>();
  {
   NativeReadSession session(memory);
   memory->deny_admission = true;
   size_t declined = 0U;
   size_t partialInsertions = 0U;
   for (uint64_t id = 1U; id <= 4U * NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS; ++id) {
    const auto before = memory->allocations;
    const auto denials = memory->denied_allocations;
    memory->fail_after = before + allocationOffset;
    EXPECT_NO_THROW(session.putNodeExists(id, (id % 2U) == 0U));
    if (memory->denied_allocations != denials) {
     ++declined;
     if (memory->allocations > before) ++partialInsertions;
     EXPECT_FALSE(session.nodeExists(id).has_value());
     if (id > 1U) EXPECT_EQ(session.nodeExists(id - 1U), ((id - 1U) % 2U) == 0U);
    }
    memory->fail_after = (std::numeric_limits<size_t>::max)();
    session.putNodeExists(id, (id % 2U) == 0U);
    EXPECT_EQ(session.nodeExists(id), (id % 2U) == 0U);
    if (id > NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS) {
     EXPECT_FALSE(session.nodeExists(id - NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS).has_value());
    }
   }
   if (allocationOffset == 0U) EXPECT_GT(declined, 0U);
   // FIFO growth occurs after the map node has already been inserted.
   if (allocationOffset == 1U) EXPECT_GT(partialInsertions, 0U);
  }
  EXPECT_EQ(memory->live, 0U);
 }
}

TEST(NativeReadSessionTokenBudgetTest, OptionalEntityCachePreservesNonAdmissionAllocationFailures) {
 auto memory = std::make_shared<SessionCountingResource>();
 NativeReadSession session(memory);
 session.putNodeExists(1U, true);
 memory->fail_after = memory->allocations;
 EXPECT_THROW(session.putNodeExists(2U, false), std::bad_alloc);
 EXPECT_EQ(session.nodeExists(1U), true);
 EXPECT_FALSE(session.nodeExists(2U).has_value());
 memory->fail_after = (std::numeric_limits<size_t>::max)();
 session.putNodeExists(2U, false);
 EXPECT_EQ(session.nodeExists(2U), false);
}

TEST(NativeReadSessionTokenBudgetTest, OptionalNamedTokenAdmissionPreservesNamespacesAndFifoAcrossRetries) {
 const auto nameFor=[](uint64_t id){return std::string(32U,'n')+std::to_string(id);};
 for(size_t offset=0U;offset<5U;++offset) {
  auto memory=std::make_shared<SessionCountingResource>();
  {
   NativeReadSession session(memory);memory->deny_admission=true;
   const auto verify=[&](auto put,auto get) {
    size_t declined=0U;
    for(uint64_t id=1U;id<=4U*NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES;++id) {
     const auto name=nameFor(id);const auto denied=memory->denied_allocations;
     memory->fail_after=memory->allocations+offset;
     ASSERT_NO_THROW(put(name,id));
     const bool rejected=memory->denied_allocations!=denied;
     memory->fail_after=(std::numeric_limits<size_t>::max)();
     if(rejected){++declined;EXPECT_FALSE(get(name).has_value());}
     put(name,id);
     const auto cached=get(name);ASSERT_TRUE(cached.has_value());ASSERT_TRUE(cached->has_value());EXPECT_EQ(**cached,id);
     if(id>NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES)
      EXPECT_FALSE(get(nameFor(id-NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES)).has_value());
    }
    if(offset==0U)EXPECT_GT(declined,0U);
   };
   verify([&](auto name,auto id){session.putPropertyToken(name,static_cast<uint32_t>(id));},[&](auto name){return session.propertyToken(name);});
   verify([&](auto name,auto id){session.putNodeLabelToken(name,id);},[&](auto name){return session.nodeLabelToken(name);});
   verify([&](auto name,auto id){session.putRelationshipTypeToken(name,id);},[&](auto name){return session.relationshipTypeToken(name);});
   memory->fail_after=(std::numeric_limits<size_t>::max)();
  }
  EXPECT_EQ(memory->live,0U);
 }
}

TEST(NativeReadSessionTokenBudgetTest, OptionalPayloadPresenceAndPoolCachesDeclineOnlyHierarchicalAdmission) {
 const std::string text(4096U,'p');
 const std::vector<uint64_t> labels{1U,2U,3U,4U};
 for(size_t offset=0U;offset<5U;++offset) {
  auto memory=std::make_shared<SessionCountingResource>();
  {
   NativeReadSession session(memory);memory->deny_admission=true;
   const auto verify=[&](auto put,auto present) {
    const auto denied=memory->denied_allocations;
    memory->fail_after=memory->allocations+offset;
    ASSERT_NO_THROW(put());
    const bool rejected=memory->denied_allocations!=denied;
    memory->fail_after=(std::numeric_limits<size_t>::max)();
    if(rejected)EXPECT_FALSE(present());
    put();EXPECT_TRUE(present());
   };
   for(uint64_t id=1U;id<=64U;++id) {
    verify([&]{session.putNodeLabels(id,labels);},[&]{return session.nodeLabelsView(id).has_value();});
    EXPECT_EQ(session.nodeLabels(id),labels);
    verify([&]{session.putPropertyPresence(false,id,1U,true);},[&]{return session.propertyPresence(false,id,1U).has_value();});
    EXPECT_EQ(session.propertyPresence(false,id,1U),true);
    verify([&]{session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,id,text);},
        [&]{return session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,id).has_value();});
    EXPECT_EQ(session.tokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,id),text);
    graph::PropertyValue input(text);
    verify([&]{session.putProperty(false,id,1U,std::move(input));},[&]{return session.hasProperty(false,id,1U);});
    EXPECT_EQ(std::get<std::string>(input.getVariant()),text);
    const auto value=session.property(false,id,1U);ASSERT_TRUE(value.has_value());ASSERT_TRUE(value->has_value());
    EXPECT_EQ(std::get<std::string>((*value)->getVariant()),text);
   }
   std::vector<graph::storage::CatalogEntry> pools(1U);pools.front().segment_id=graph::storage::SegmentId(17U);
   verify([&]{session.putEntityPagePools(pools);},[&]{return session.entityPagePools()!=nullptr;});
   EXPECT_EQ(session.entityPagePools()->front().segment_id,graph::storage::SegmentId(17U));
   memory->deny_admission=false;
   const auto fails=[&](auto put) {
    memory->fail_after=memory->allocations;
    EXPECT_THROW(put(),std::bad_alloc);
    memory->fail_after=(std::numeric_limits<size_t>::max)();
   };
   fails([&]{session.putNodeLabels(1000U,labels);});
   fails([&]{session.putPropertyPresence(false,1000U,1U,true);});
   fails([&]{session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,1000U,text);});
   fails([&]{session.putProperty(false,1000U,1U,graph::PropertyValue(text));});
   fails([&]{session.putEntityPagePools(pools);});
   EXPECT_THROW(session.putNodeLabels(0U,labels),std::invalid_argument);
   EXPECT_THROW(session.putPropertyPresence(false,0U,1U,true),std::invalid_argument);
   EXPECT_THROW(session.putTokenName(NativeReadSession::TokenNameKind::TNK_PROPERTY_KEY,0U,text),std::invalid_argument);
  }
  EXPECT_EQ(memory->live,0U);
 }
}
