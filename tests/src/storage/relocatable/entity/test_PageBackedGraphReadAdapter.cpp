/**
 * @file test_PageBackedGraphReadAdapter.cpp
 * @brief Tests for the logical read view of native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "storage/relocatable/entity/PageBackedGraphReadAdapterTestFixture.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest nodePool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId, uint64_t physicalSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(physicalSlot),
				.last_slot = PhysicalSlotId(physicalSlot)};
	}

	NodeEntityRecord node(std::array<uint64_t, RSS_ENTITY_INLINE_LABEL_CAPACITY> labels, uint8_t count) {
		return {.inline_label_ids = labels,
				.inline_label_count = count};
	}

	std::vector<uint64_t> labelIds(uint64_t first, size_t count) {
		std::vector<uint64_t> result;
		result.reserve(count);
		for (size_t index = 0U; index < count; ++index) {
			result.push_back(first + static_cast<uint64_t>(index) * 2U);
		}
		return result;
	}

	PropertyCollectionRoot propertyRoot(uint64_t ownerId, uint32_t ownerType,
										std::vector<PropertyCollectionRootEntry> entries) {
		return {ownerId, ownerType, std::move(entries)};
	}

	PropertyCollectionRootStoreOptions propertyOptions() { return {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U}; }

	NativeGraphWritePlannerOptions graphWriteOptions() {
		NativeGraphWritePlannerOptions result;
		result.first_slot = PhysicalSlotId(46U);
		result.last_slot = PhysicalSlotId(70U);
		result.entity_pool_owner_id = 907U;
		result.property_roots = propertyOptions();
		return result;
	}

	class TestPageBackedGraphNameResolver final : public PageBackedGraphNameResolver {
	public:
		explicit TestPageBackedGraphNameResolver(bool duplicateLabels = false) : duplicate_labels_(duplicateLabels) {}

		[[nodiscard]] std::string resolveNodeLabelToken(const MetadataSnapshotPin &, uint64_t tokenId) const override {
			++node_label_resolutions_;
			switch (tokenId) {
				case 7U:
					return "Person";
				case 9U:
					return duplicate_labels_ ? "Person" : "VIP";
				default:
					return {};
			}
		}

		[[nodiscard]] std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &,
																														 uint64_t tokenId) const override {
			++relationship_type_resolutions_;
			switch (tokenId) {
				case 41U:
					return "KNOWS";
				case 43U:
					return "FOLLOWS";
				default:
					return {};
			}
		}

		[[nodiscard]] std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t tokenId) const override {
			++property_key_resolutions_;
			switch (tokenId) {
				case 3U:
					return "name";
				case 5U:
					return "age";
				case 7U:
					return "weight";
				case 9U:
					return "name";
				default:
					return {};
			}
		}

		[[nodiscard]] std::optional<uint32_t> resolvePropertyToken(const MetadataSnapshotPin &,
																		 std::string_view name) const override {
			++property_token_resolutions_;
			if (name == "name") {
				return 3U;
			}
			if (name == "age") {
				return 5U;
			}
			if (name == "weight") {
				return 7U;
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<uint64_t> resolveNodeLabelName(const MetadataSnapshotPin &,
																		 std::string_view name) const override {
			++node_label_token_resolutions_;
			if (name == "Person") {
				return 7U;
			}
			if (name == "VIP") {
				return 9U;
			}
			return std::nullopt;
		}

		[[nodiscard]] uint64_t relationshipTypeResolutionCount() const { return relationship_type_resolutions_; }
		[[nodiscard]] uint64_t propertyKeyResolutionCount() const { return property_key_resolutions_; }
		[[nodiscard]] uint64_t propertyTokenResolutionCount() const { return property_token_resolutions_; }
		[[nodiscard]] uint64_t nodeLabelTokenResolutionCount() const { return node_label_token_resolutions_; }

	private:
		bool duplicate_labels_ = false;
		mutable uint64_t node_label_resolutions_ = 0U;
		mutable uint64_t relationship_type_resolutions_ = 0U;
		mutable uint64_t property_key_resolutions_ = 0U;
		mutable uint64_t property_token_resolutions_ = 0U;
		mutable uint64_t node_label_token_resolutions_ = 0U;
	};

} // namespace

TEST(PageBackedGraphNameResolverContractTest, SupportsVirtualDestructionThroughThePublicInterface) {
	std::unique_ptr<PageBackedGraphNameResolver> resolver = std::make_unique<TestPageBackedGraphNameResolver>();
	EXPECT_NE(resolver, nullptr);
}

TEST(NativeReadSessionTest, KeepsTokenNamesBoundedAndReplacesExistingNames) {
	NativeReadSession session;
	using TokenKind = NativeReadSession::TokenNameKind;
	for (uint64_t tokenId = 1U; tokenId <= NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES; ++tokenId) {
		session.putTokenName(TokenKind::TNK_PROPERTY_KEY, tokenId, "key" + std::to_string(tokenId));
	}
	ASSERT_EQ(session.tokenName(TokenKind::TNK_PROPERTY_KEY, 1U), std::optional<std::string>{"key1"});
	session.putTokenName(TokenKind::TNK_PROPERTY_KEY, 1U, "replacement");
	EXPECT_EQ(session.tokenName(TokenKind::TNK_PROPERTY_KEY, 1U), std::optional<std::string>{"replacement"});
	session.putTokenName(TokenKind::TNK_NODE_LABEL, 1U, "Person");
	EXPECT_EQ(session.tokenName(TokenKind::TNK_NODE_LABEL, 1U), std::optional<std::string>{"Person"});
	session.putTokenName(TokenKind::TNK_PROPERTY_KEY, NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES + 1U,
						 "overflow");
	EXPECT_FALSE(session.tokenName(TokenKind::TNK_PROPERTY_KEY, 1U).has_value());
	EXPECT_FALSE(session.propertyToken("name").has_value());
	session.putPropertyToken("name", 3U);
	ASSERT_TRUE(session.propertyToken("name").has_value());
	EXPECT_EQ(*session.propertyToken("name"), std::optional<uint32_t>{3U});
	session.putPropertyToken("unknown", std::nullopt);
	ASSERT_TRUE(session.propertyToken("unknown").has_value());
	EXPECT_FALSE(session.propertyToken("unknown")->has_value());
	session.putNodeLabelToken("Person", 7U);
	ASSERT_TRUE(session.nodeLabelToken("Person").has_value());
	EXPECT_EQ(*session.nodeLabelToken("Person"), std::optional<uint64_t>{7U});
	session.putNodeLabelToken("Unknown", std::nullopt);
	ASSERT_TRUE(session.nodeLabelToken("Unknown").has_value());
	EXPECT_FALSE(session.nodeLabelToken("Unknown")->has_value());
	EXPECT_THROW(session.putPropertyToken("", 3U), std::invalid_argument);
	EXPECT_THROW(session.putPropertyToken("invalid", 0U), std::invalid_argument);
	EXPECT_THROW(session.putNodeLabelToken("", 7U), std::invalid_argument);
	EXPECT_THROW(session.putNodeLabelToken("invalid", 0U), std::invalid_argument);
	EXPECT_THROW(session.putTokenName(TokenKind::TNK_PROPERTY_KEY, 0U, "invalid"), std::invalid_argument);
	EXPECT_THROW(session.putTokenName(TokenKind::TNK_PROPERTY_KEY, 1U, ""), std::invalid_argument);
}

TEST(NativeReadSessionTest, KeepsNodeLabelTokenSetsBoundedAndReplacesExistingSets) {
	NativeReadSession session;
	EXPECT_FALSE(session.nodeLabels(17U).has_value());
	session.putNodeLabels(17U, {3U, 9U});
	ASSERT_TRUE(session.nodeLabels(17U).has_value());
	EXPECT_EQ(*session.nodeLabels(17U), (std::vector<uint64_t>{3U, 9U}));
	session.putNodeLabels(17U, {5U});
	EXPECT_EQ(*session.nodeLabels(17U), (std::vector<uint64_t>{5U}));
	EXPECT_THROW(session.putNodeLabels(0U, {}), std::invalid_argument);
	EXPECT_THROW(session.putNodeLabels(17U, {9U, 3U}), std::invalid_argument);
}

TEST(NativeReadSessionTest, CachesRelationshipTypeTokenHitsAndMissesWithTheTokenBound) {
	NativeReadSession session;
	EXPECT_FALSE(session.relationshipTypeToken("KNOWS").has_value());
	session.putRelationshipTypeToken("KNOWS", 11U);
	session.putRelationshipTypeToken("MISSING", std::nullopt);
	ASSERT_TRUE(session.relationshipTypeToken("KNOWS").has_value());
	EXPECT_EQ(*session.relationshipTypeToken("KNOWS"), std::optional<uint64_t>{11U});
	ASSERT_TRUE(session.relationshipTypeToken("MISSING").has_value());
	EXPECT_FALSE(session.relationshipTypeToken("MISSING")->has_value());
	EXPECT_THROW(session.putRelationshipTypeToken("", 1U), std::invalid_argument);
	EXPECT_THROW(session.putRelationshipTypeToken("invalid", 0U), std::invalid_argument);
	for (size_t index = 0U; index < NativeReadSession::MAXIMUM_CACHED_TOKEN_NAMES; ++index) {
		session.putRelationshipTypeToken("type" + std::to_string(index), index + 1U);
	}
	session.putRelationshipTypeToken("replacement", 99U);
	EXPECT_FALSE(session.relationshipTypeToken("KNOWS").has_value());
}

TEST(NativeReadSessionTest, KeepsSelectedPropertyValuesAndMissingFieldsBounded) {
	NativeReadSession session;
	EXPECT_FALSE(session.property(false, 11U, 3U).has_value());
	session.putProperty(false, 11U, 3U, graph::PropertyValue(int64_t{42}));
	session.putProperty(true, 11U, 3U, std::nullopt);
	ASSERT_TRUE(session.property(false, 11U, 3U).has_value());
	ASSERT_TRUE(session.property(false, 11U, 3U)->has_value());
	EXPECT_EQ(**session.property(false, 11U, 3U), graph::PropertyValue(int64_t{42}));
	ASSERT_TRUE(session.property(true, 11U, 3U).has_value());
	EXPECT_FALSE(session.property(true, 11U, 3U)->has_value());
	EXPECT_THROW(session.putProperty(false, 0U, 3U, std::nullopt), std::invalid_argument);
	EXPECT_THROW(session.putProperty(false, 11U, 0U, std::nullopt), std::invalid_argument);
}

TEST(NativeReadSessionTest, CachesPresentAndAbsentNodeExistenceWithTheEntityBound) {
	NativeReadSession session;
	EXPECT_FALSE(session.nodeExists(41U).has_value());
	session.putNodeExists(41U, true);
	session.putNodeExists(42U, false);
	EXPECT_EQ(session.nodeExists(41U), std::optional<bool>{true});
	EXPECT_EQ(session.nodeExists(42U), std::optional<bool>{false});
	session.putNodeExists(41U, false);
	EXPECT_EQ(session.nodeExists(41U), std::optional<bool>{false});
	for (uint64_t id = 1U; id <= NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS; ++id) {
		session.putNodeExists(1000U + id, true);
	}
	session.putNodeExists(2000U, true);
	EXPECT_FALSE(session.nodeExists(1001U).has_value());
	EXPECT_EQ(session.nodeExists(2000U), std::optional<bool>{true});
	EXPECT_THROW(session.putNodeExists(0U, true), std::invalid_argument);
}

TEST(NativeReadSessionTest, EntityRecordCacheReplacementKeepsBoundedFifoOrder) {
	NativeReadSession session;
	for (uint64_t id = 1U; id <= NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS; ++id) {
		session.putNode(id, graph::storage::NodeEntityRecord{});
	}
	graph::storage::NodeEntityRecord replacement{};
	replacement.inline_label_count = 1U;
	session.putNode(1U, replacement);
	ASSERT_TRUE(session.node(1U).has_value());
	EXPECT_EQ(session.node(1U)->inline_label_count, 1U);
	session.putNode(NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 1U,
			graph::storage::NodeEntityRecord{});
	EXPECT_FALSE(session.node(1U).has_value());
	EXPECT_TRUE(session.node(2U).has_value());
	ASSERT_TRUE(session.node(NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 1U).has_value());
	EXPECT_EQ(session.node(NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 1U)->inline_label_count, 0U);
}

TEST(NativeReadSessionTest, DoesNotRetainPropertyValuesOverTheByteBudget) {
	NativeReadSession session;
	graph::PropertyValue::Bytes bytes(NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U);
	session.putProperty(false, 11U, 3U, graph::PropertyValue(std::move(bytes)));
	EXPECT_FALSE(session.property(false, 11U, 3U).has_value());

	session.putProperty(false, 11U, 3U, graph::PropertyValue(int64_t{7}));
	ASSERT_TRUE(session.property(false, 11U, 3U).has_value());
	ASSERT_TRUE(session.property(false, 11U, 3U)->has_value());
	EXPECT_EQ(**session.property(false, 11U, 3U), graph::PropertyValue(int64_t{7}));

	graph::PropertyValue::Bytes nestedBytes(NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U);
	graph::PropertyValue nestedList(std::vector<graph::PropertyValue>{graph::PropertyValue(std::move(nestedBytes))});
	session.putProperty(false, 11U, 4U, std::move(nestedList));
	EXPECT_FALSE(session.property(false, 11U, 4U).has_value());
	graph::PropertyValue::MapType nestedMap;
	nestedMap.emplace("payload", graph::PropertyValue(graph::PropertyValue::Bytes(
			NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U)));
	session.putProperty(false, 11U, 5U, graph::PropertyValue(std::move(nestedMap)));
	EXPECT_FALSE(session.property(false, 11U, 5U).has_value());
}

TEST(NativeReadSessionTest, ReplacementPayloadsEvictOtherKeysBeforeExceedingTheRetainedCap) {
	NativeReadSession session;
	const size_t bound = NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES;
	session.putProperty(false, 11U, 3U, graph::PropertyValue(graph::PropertyValue::Bytes(bound / 2U, static_cast<std::byte>('a'))));
	session.putProperty(true, 11U, 3U, graph::PropertyValue(graph::PropertyValue::Bytes(bound / 2U, static_cast<std::byte>('b'))));
	session.putProperty(false, 11U, 3U, graph::PropertyValue(graph::PropertyValue::Bytes(bound, static_cast<std::byte>('c'))));
	EXPECT_TRUE(session.hasProperty(false, 11U, 3U));
	EXPECT_FALSE(session.hasProperty(true, 11U, 3U));
	{
		const auto value = session.property(false, 11U, 3U);
		ASSERT_TRUE(value && *value);
		EXPECT_EQ(std::get<graph::PropertyValue::Bytes>((**value).getVariant()), graph::PropertyValue::Bytes(bound, static_cast<std::byte>('c')));
	}
	session.putProperty(false, 11U, 3U, graph::PropertyValue(graph::PropertyValue::Bytes(bound / 2U, static_cast<std::byte>('d'))));
	session.putProperty(true, 11U, 3U, graph::PropertyValue(graph::PropertyValue::Bytes(bound / 2U, static_cast<std::byte>('e'))));
	session.putProperty(false, 12U, 3U, std::nullopt);
	ASSERT_TRUE(session.property(false, 12U, 3U));
	EXPECT_FALSE(*session.property(false, 12U, 3U));
	session.putProperty(false, 13U, 3U, graph::PropertyValue(int64_t{1}));
	EXPECT_FALSE(session.hasProperty(false, 11U, 3U));
	EXPECT_TRUE(session.hasProperty(true, 11U, 3U));
	EXPECT_TRUE(session.hasProperty(false, 12U, 3U));
	EXPECT_TRUE(session.hasProperty(false, 13U, 3U));
}

TEST(NativeReadSessionTest, OversizedCachePublicationDoesNotConsumeCallerPayloadAndRemovesOldEntry) {
	NativeReadSession session;
	const size_t size = NativeReadSession::MAXIMUM_CACHED_PROPERTY_BYTES + 1U;
	session.putProperty(false, 11U, 3U, graph::PropertyValue(int64_t{7}));
	std::optional<graph::PropertyValue> value{graph::PropertyValue(std::string(size, 'x'))};
	session.putProperty(false, 11U, 3U, std::move(value));
	EXPECT_FALSE(session.hasProperty(false, 11U, 3U));
	ASSERT_TRUE(value);
	EXPECT_EQ(std::get<std::string>(value->getVariant()).size(), size);
	const std::optional<graph::PropertyValue> borrowed{graph::PropertyValue(std::string(512U, 'p'))};
	session.putProperty(false, 11U, 3U, borrowed);
	auto independent = session.property(false, 11U, 3U);
	ASSERT_TRUE(independent && *independent);
	**independent = graph::PropertyValue("changed");
	EXPECT_EQ(std::get<std::string>(borrowed->getVariant()), std::string(512U, 'p'));
	EXPECT_EQ(**session.property(false, 11U, 3U), *borrowed);
}

using graph::storage::test::PageBackedGraphReadAdapterTest;

TEST_F(PageBackedGraphReadAdapterTest, RejectsReusingAReadSessionAcrossMetadataGenerations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	MetadataSnapshotPin before = metadata->openSnapshot();
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(40U, 910U, 901U, 60U), 1001U, node({0U, 0U, 0U}, 0U),
										 LogSequenceNumber(2U))
						.has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();

	NativeReadSession session;
	session.bind(before);
	session.putNode(1001U, node({0U, 0U, 0U}, 0U));
	EXPECT_THROW(session.bind(current), std::invalid_argument);
	EXPECT_TRUE(session.node(1001U).has_value());

	before.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ResolvesLogicalIdentityAndCompleteOverflowLabelsThroughOneSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node({7U, 9U, 11U}, 3U),
									  LogSequenceNumber(2U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);

	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(before, 1001U), (PageBackedGraphNode{1001U, {7U, 9U, 11U}, {}}));

	const std::vector<uint64_t> overflowLabels = labelIds(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 3U);
	EntityLabelSetWritePlan plan;
	plan.append_page = EntityLabelSetAppendRequest{SegmentId(31U), PageId(102U)};
	static_cast<void>(labels.updateNodeLabels(before, 1001U, overflowLabels, plan, LogSequenceNumber(3U)));

	// The original Entity Map and Node-page frame remain reachable to this pin.
	EXPECT_EQ(adapter.readNode(before, 1001U), (PageBackedGraphNode{1001U, {7U, 9U, 11U}, {}}));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(current, 1001U), (PageBackedGraphNode{1001U, overflowLabels, {}}));
	const std::array<uint64_t, 1U> orderedNodeIds{1001U};
	EXPECT_EQ(adapter.readNodeLabelTokensSorted(current, orderedNodeIds),
			  (std::vector<std::vector<uint64_t>>{overflowLabels}));
	EXPECT_THROW(static_cast<void>(adapter.readNodeLabelTokensSorted(
			current, std::array<uint64_t, 2U>{1001U, 1001U})), std::exception);
	before.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReturnsFullEdgeTopologyWithoutPersistedLogicalIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const EdgeEntityRecord edge{1001U, 1002U, 0U};
	ASSERT_TRUE(entities.allocateEdge(nodePool(32U, 902U, 201U, 41U), 2001U, edge, LogSequenceNumber(2U)).has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(adapter.readEdge(snapshot, 2001U), (PageBackedGraphEdge{2001U, 1001U, 1002U, 0U, {}}));
	std::vector<LogicalGraphEdgeTopology> topologies;
	EXPECT_TRUE(logical.visitEdgeTopologiesUntil(snapshot, 2U, [&topologies](const LogicalGraphEdgeTopology &edge) {
		topologies.push_back(edge);
		return true;
	}));
	EXPECT_EQ(topologies, (std::vector<LogicalGraphEdgeTopology>{{2001U, 1001U, 1002U, ""}}));
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ColumnAdmissionCoversColdReadsCacheHitsAndDuplicateOutputs) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	auto transaction = planner.beginTransaction();
	const uint64_t n1 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t n2 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t e1 = transaction.createEdge(n1, n2, 41U);
	const uint64_t e2 = transaction.createEdge(n2, n1, 41U);
	ASSERT_TRUE(transaction.commit().committed());
	const graph::PropertyValue payload(std::string(512U, 'p'));
	const std::array roots{
		propertyRoot(n1, graph::Node::typeId, {{3U, payload}, {5U, graph::PropertyValue()}}),
		propertyRoot(e1, graph::Edge::typeId, {{5U, graph::PropertyValue()}, {7U, payload}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	auto snapshot = metadata->openSnapshot();
	for (const bool isEdge: {false, true}) {
		for (const int cacheState: {0, 1, 2}) {
			SCOPED_TRACE(isEdge);
			SCOPED_TRACE(cacheState);
			NativeReadSession session;
			const std::array<uint64_t, 2U> ids{isEdge ? e1 : n1, isEdge ? e2 : n2};
			const std::array<std::string_view, 4U> keys{
				isEdge ? "weight" : "name", "age", isEdge ? "weight" : "name", "missing"};
			const auto read = [&](const SelectedPropertyReadAdmission *admission) {
				return isEdge ? logical.readEdgePropertyColumnsSorted(snapshot, ids, keys, &session, admission)
						: logical.readNodePropertyColumnsSorted(snapshot, ids, keys, &session, admission);
			};
			if (cacheState == 1) static_cast<void>(read(nullptr));
			if (cacheState == 2) {
				session.bind(snapshot);
				session.putProperty(isEdge, ids[0], isEdge ? 7U : 3U, payload);
			}
			size_t used = 0U;
			size_t limit = 0U;
			std::vector<size_t> admitted;
			const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
				if (bytes > limit - used) throw std::runtime_error("column admission exhausted");
				used += bytes;
				admitted.push_back(bytes);
			};
			EXPECT_THROW(static_cast<void>(read(&admission)), std::runtime_error);
			EXPECT_EQ(used, 0U);
			limit = 2000U;
			const auto result = read(&admission);
			ASSERT_EQ(result.size(), 4U);
			EXPECT_EQ(result[0][0], payload);
			EXPECT_EQ(result[2][0], payload);
			EXPECT_EQ(result[1][0], graph::PropertyValue());
			for (const auto &column: result) EXPECT_EQ(column[1], graph::PropertyValue());
			std::ranges::sort(admitted);
			// String entry: token(4) + value header(6) + string length(4) + payload(512).
			EXPECT_EQ(admitted, (std::vector<size_t>{10U, 526U, 526U}));
			used = 0U;
			limit = 1000U; // Unique cells fit; the second payload output does not.
			EXPECT_THROW(static_cast<void>(read(&admission)), std::runtime_error);
			EXPECT_LE(used, limit);
			const auto retry = read(nullptr);
			EXPECT_EQ(retry, result);
		}
	}
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, PropertyColumnOwnershipPreservesDuplicateKeysNullsAndSessionReuse) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	auto transaction = planner.beginTransaction();
	const uint64_t n1 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t n2 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t e1 = transaction.createEdge(n1, n2, 41U);
	const uint64_t e2 = transaction.createEdge(n2, n1, 41U);
	ASSERT_TRUE(transaction.commit().committed());
	const graph::PropertyValue nodePayload(std::string(512U, 'n'));
	const graph::PropertyValue edgePayload(std::string(512U, 'e'));
	const std::array roots{
		propertyRoot(n1, graph::Node::typeId, {{3U, nodePayload}, {5U, graph::PropertyValue()}}),
		propertyRoot(e1, graph::Edge::typeId, {{5U, graph::PropertyValue()}, {7U, edgePayload}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	auto snapshot = metadata->openSnapshot();
	const std::array<uint64_t, 2U> nodeIds{n1, n2};
	const std::array<uint64_t, 2U> edgeIds{e1, e2};
	const std::array<std::string_view, 4U> nodeKeys{"name", "missing", "name", "age"};
	const std::array<std::string_view, 4U> edgeKeys{"weight", "age", "weight", "missing"};
	std::vector<std::vector<graph::PropertyValue>> retainedNodes;
	std::vector<std::vector<graph::PropertyValue>> retainedEdges;
	{
		NativeReadSession session;
		auto nodes = logical.readNodePropertyColumnsSorted(snapshot, nodeIds, nodeKeys, &session);
		auto edges = logical.readEdgePropertyColumnsSorted(snapshot, edgeIds, edgeKeys, &session);
		ASSERT_EQ(nodes.size(), 4U);
		ASSERT_EQ(edges.size(), 4U);
		for (const auto &column: nodes) ASSERT_EQ(column.size(), 2U);
		for (const auto &column: edges) ASSERT_EQ(column.size(), 2U);
		EXPECT_EQ(nodes[0][0], nodePayload);
		EXPECT_EQ(nodes[2][0], nodePayload);
		EXPECT_EQ(edges[0][0], edgePayload);
		EXPECT_EQ(edges[2][0], edgePayload);
		for (const auto &column: nodes) EXPECT_EQ(column[1], graph::PropertyValue());
		for (const auto &column: edges) EXPECT_EQ(column[1], graph::PropertyValue());
		EXPECT_EQ(nodes[1][0], graph::PropertyValue());
		EXPECT_EQ(nodes[3][0], graph::PropertyValue());
		EXPECT_EQ(edges[1][0], graph::PropertyValue());
		EXPECT_EQ(edges[3][0], graph::PropertyValue());
		EXPECT_NE(std::get<std::string>(nodes[0][0].getVariant()).data(),
				std::get<std::string>(nodes[2][0].getVariant()).data());
		nodes[0][0] = graph::PropertyValue("changed");
		edges[0][0] = graph::PropertyValue("changed");
		EXPECT_EQ(nodes[2][0], nodePayload);
		EXPECT_EQ(edges[2][0], edgePayload);
		retainedNodes = logical.readNodePropertyColumnsSorted(snapshot, nodeIds, nodeKeys, &session);
		retainedEdges = logical.readEdgePropertyColumnsSorted(snapshot, edgeIds, edgeKeys, &session);
		EXPECT_EQ(retainedNodes[0][0], nodePayload);
		EXPECT_EQ(retainedEdges[0][0], edgePayload);
		const auto nulls = pages.readNodePropertyColumnsSorted(snapshot, nodeIds,
				std::array<uint32_t, 1U>{5U}, &session);
		ASSERT_TRUE(nulls[0][0].has_value());
		EXPECT_EQ(*nulls[0][0], graph::PropertyValue());
		EXPECT_FALSE(nulls[0][1].has_value());
	}
	snapshot.release();
	EXPECT_EQ(retainedNodes[0][0], nodePayload);
	EXPECT_EQ(retainedNodes[2][0], nodePayload);
	EXPECT_EQ(retainedEdges[0][0], edgePayload);
	EXPECT_EQ(retainedEdges[2][0], edgePayload);
}

TEST_F(PageBackedGraphReadAdapterTest, ReadsOwnerKeyedPropertiesAndPreservesAnOlderSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(33U, 903U, 301U, 42U), 1001U, node({7U, 0U, 0U}, 1U), LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(
			entities.allocateNode(nodePool(35U, 905U, 303U, 44U), 1002U, node({0U, 0U, 0U}, 0U), LogSequenceNumber(3U))
					.has_value());
		const EdgeEntityRecord edge{1001U, 1002U, 41U};
	ASSERT_TRUE(entities.allocateEdge(nodePool(34U, 904U, 302U, 43U), 2001U, edge, LogSequenceNumber(4U)).has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);

	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_TRUE(adapter.readNode(before, 1001U).properties.empty());
	EXPECT_TRUE(adapter.readEdge(before, 2001U).properties.empty());
	EXPECT_EQ(logical.readEdge(before, 2001U).type, "KNOWS");
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 1U);

	const std::vector<PropertyCollectionRootEntry> nodeProperties{{3U, graph::PropertyValue("alpha")},
																  {5U, graph::PropertyValue(int64_t(42))}};
	const std::vector<PropertyCollectionRootEntry> edgeProperties{{7U, graph::PropertyValue(true)}};
	const std::vector<PropertyCollectionRootEntry> secondNodeProperties{{3U, graph::PropertyValue("beta")}};
	const std::array roots{propertyRoot(1001U, graph::Node::typeId, nodeProperties),
							propertyRoot(1002U, graph::Node::typeId, secondNodeProperties),
							propertyRoot(2001U, graph::Edge::typeId, edgeProperties)};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(current, 1001U).properties, nodeProperties);
	EXPECT_EQ(adapter.readEdge(current, 2001U).properties, edgeProperties);
	NativeReadSession selectedSession;
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "name", &selectedSession), graph::PropertyValue("alpha"));
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "name", &selectedSession), graph::PropertyValue("alpha"));
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "age", &selectedSession), graph::PropertyValue(int64_t(42)));
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "missing", &selectedSession), graph::PropertyValue());
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "missing", &selectedSession), graph::PropertyValue());
	EXPECT_EQ(logical.readEdgeProperty(current, 2001U, "weight", &selectedSession), graph::PropertyValue(true));
	const std::array<uint64_t, 1U> nodeIds{1001U};
	const std::array<uint64_t, 1U> edgeIds{2001U};
	EXPECT_EQ(adapter.readNodePropertiesSorted(current, nodeIds, 3U),
				  (std::vector<std::optional<graph::PropertyValue>>{graph::PropertyValue("alpha")}));
		const std::array<uint64_t, 2U> bulkNodeIds{1001U, 1002U};
		const auto bulkNodeProperties = adapter.readNodePropertiesSorted(current, bulkNodeIds);
		ASSERT_EQ(bulkNodeProperties.size(), bulkNodeIds.size());
		EXPECT_EQ(bulkNodeProperties[0], nodeProperties);
		EXPECT_EQ(bulkNodeProperties[1], secondNodeProperties);
		EXPECT_THROW(static_cast<void>(adapter.readNodePropertiesSorted(current, std::array<uint64_t, 2U>{1002U, 1001U})),
					 std::exception);
	EXPECT_EQ(logical.readNodePropertiesSorted(current, nodeIds, "name", &selectedSession),
			  (std::vector<graph::PropertyValue>{graph::PropertyValue("alpha")}));
	EXPECT_EQ(adapter.readEdgePropertiesSorted(current, edgeIds, 7U),
			  (std::vector<std::optional<graph::PropertyValue>>{graph::PropertyValue(true)}));
	EXPECT_EQ(logical.readEdgePropertiesSorted(current, edgeIds, "weight", &selectedSession),
			  (std::vector<graph::PropertyValue>{graph::PropertyValue(true)}));
	const LogicalGraphNode shapedNode = logical.readNodeShaped(
			current, 1001U, EntityReadShape{.include_labels = true, .property_tokens = {3U}}, &selectedSession);
	EXPECT_EQ(shapedNode.labels, (std::vector<std::string>{"Person"}));
	ASSERT_EQ(shapedNode.properties.size(), 1U);
	EXPECT_EQ(shapedNode.properties.front().key, "name");
	EXPECT_EQ(shapedNode.properties.front().value, graph::PropertyValue("alpha"));
	const LogicalGraphEdge shapedEdge = logical.readEdgeShaped(
			current, 2001U, EntityReadShape{.include_type = true, .property_tokens = {7U}}, &selectedSession);
	EXPECT_EQ(shapedEdge.type, "KNOWS");
	ASSERT_EQ(shapedEdge.properties.size(), 1U);
	EXPECT_EQ(shapedEdge.properties.front().key, "weight");
	EXPECT_EQ(shapedEdge.properties.front().value, graph::PropertyValue(true));
	NativeReadSession shapedSession;
	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	const EntityRef shapedNodeRef = logical.readNodeRef(
			current, 1001U, EntityReadShape{.include_labels = true, .property_tokens = {3U}}, &shapedSession);
	EXPECT_EQ(shapedNodeRef, (EntityRef{1001U, false, 0U, 0U, 0U}));
	EXPECT_EQ(logical.readNodeProperty(current, 1001U, "name", &shapedSession), graph::PropertyValue("alpha"));
	const EntityRef shapedEdgeRef = logical.readEdgeRef(
			current, 2001U, EntityReadShape{.include_type = true, .property_tokens = {7U}}, &shapedSession);
	EXPECT_EQ(shapedEdgeRef.type_token, 41U);
	EXPECT_EQ(logical.readEdgeProperty(current, 2001U, "weight", &shapedSession), graph::PropertyValue(true));
	const graph::debug::PerfTrace::Snapshot prefetchTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	ASSERT_TRUE(prefetchTrace.contains("native_graph.read.property_selected_decode"));
	EXPECT_EQ(prefetchTrace.at("native_graph.read.property_selected_decode").totalValue, 2);
	EXPECT_EQ(names.propertyTokenResolutionCount(), 6U);
	EXPECT_TRUE(logical.nodeHasLabels(current, 1001U, {"Person"}, &selectedSession));
	EXPECT_TRUE(logical.nodeHasLabels(current, 1001U, {"Person"}, &selectedSession));
	EXPECT_FALSE(logical.nodeHasLabels(current, 1001U, {"Missing"}, &selectedSession));
	EXPECT_FALSE(logical.nodeHasLabels(current, 1001U, {"Missing"}, &selectedSession));
	EXPECT_EQ(names.nodeLabelTokenResolutionCount(), 2U);
	EXPECT_EQ(logical.readEdge(current, 2001U).type, "KNOWS");
	// A new metadata generation cannot reuse a logical-name cache entry from
	// the earlier pinned snapshot, even when the durable token name is unchanged.
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 4U);
	EXPECT_TRUE(adapter.readNode(before, 1001U).properties.empty());
	EXPECT_TRUE(adapter.readEdge(before, 2001U).properties.empty());
	// `all_properties` must not implicitly widen the logical shape.  Callers
	// that need only properties can omit labels/type while still receiving the
	// complete owner-keyed Property set.
	const LogicalGraphNode propertyOnlyNode = logical.readNodeShaped(
			current, 1001U, EntityReadShape{.all_properties = true}, &shapedSession);
	EXPECT_TRUE(propertyOnlyNode.labels.empty());
	EXPECT_EQ(propertyOnlyNode.properties.size(), 2U);
	const LogicalGraphEdge propertyOnlyEdge = logical.readEdgeShaped(
			current, 2001U, EntityReadShape{.all_properties = true}, &shapedSession);
	EXPECT_TRUE(propertyOnlyEdge.type.empty());
	EXPECT_EQ(propertyOnlyEdge.properties.size(), 1U);
	before.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ShapeAwareNodeScanAvoidsUnrequestedLabelsAndProperties) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(36U, 906U, 601U, 45U), 1001U, node({7U, 9U, 0U}, 2U),
									 LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(entities.allocateNode(nodePool(37U, 907U, 602U, 46U), 1002U, node({9U, 0U, 0U}, 1U),
									 LogSequenceNumber(3U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array roots{
			propertyRoot(1001U, graph::Node::typeId,
						 {{3U, graph::PropertyValue("alpha")}, {5U, graph::PropertyValue(int64_t{42})}}),
			propertyRoot(1002U, graph::Node::typeId, {{3U, graph::PropertyValue("beta")}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	snapshot.release();
	snapshot = metadata->openSnapshot();

	const PageBackedGraphNodePage identity =
			adapter.scanNodesAfter(snapshot, 0U, 8U, 8U, EntityReadShape{});
	ASSERT_EQ(identity.nodes.size(), 2U);
	EXPECT_EQ(identity.nodes[0].node_id, 1001U);
	EXPECT_TRUE(identity.nodes[0].label_token_ids.empty());
	EXPECT_TRUE(identity.nodes[0].properties.empty());
	EXPECT_TRUE(identity.nodes[1].label_token_ids.empty());
	EXPECT_TRUE(identity.nodes[1].properties.empty());

	const PageBackedGraphNodePage labelOnly = adapter.scanNodesAfter(
			snapshot, 0U, 8U, 8U, EntityReadShape{.include_labels = true});
	ASSERT_EQ(labelOnly.nodes.size(), 2U);
	EXPECT_EQ(labelOnly.nodes[0].label_token_ids, (std::vector<uint64_t>{7U, 9U}));
	EXPECT_EQ(labelOnly.nodes[1].label_token_ids, (std::vector<uint64_t>{9U}));
	EXPECT_TRUE(labelOnly.nodes[0].properties.empty());

	const PageBackedGraphNodePage selected = adapter.scanNodesAfter(
			snapshot, 0U, 8U, 8U, EntityReadShape{.property_tokens = {3U}});
	ASSERT_EQ(selected.nodes.size(), 2U);
	EXPECT_TRUE(selected.nodes[0].label_token_ids.empty());
	EXPECT_EQ(selected.nodes[0].properties,
			  (std::vector<PropertyCollectionRootEntry>{{3U, graph::PropertyValue("alpha")}}));
	EXPECT_EQ(selected.nodes[1].properties,
			  (std::vector<PropertyCollectionRootEntry>{{3U, graph::PropertyValue("beta")}}));

	const PageBackedGraphNodePage complete = adapter.scanNodesAfter(
			snapshot, 0U, 8U, 8U, EntityReadShape{.include_labels = true, .all_properties = true});
	ASSERT_EQ(complete.nodes.size(), 2U);
	EXPECT_EQ(complete.nodes[0].label_token_ids, (std::vector<uint64_t>{7U, 9U}));
	EXPECT_EQ(complete.nodes[0].properties.size(), 2U);
	EXPECT_EQ(complete.nodes[1].properties.size(), 1U);

	const PageBackedGraphNodePage continuation =
			adapter.scanNodesAfter(snapshot, 1001U, 8U, 8U, EntityReadShape{});
	ASSERT_EQ(continuation.nodes.size(), 1U);
	EXPECT_EQ(continuation.nodes.front().node_id, 1002U);
	EXPECT_FALSE(continuation.may_have_more);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(
				snapshot, 0U, 8U, 8U, EntityReadShape{.property_tokens = {5U, 3U}})), std::invalid_argument);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReusesNodeLabelsWithinOneReadSession) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(35U, 905U, 501U, 44U), 1001U, node({7U, 9U, 0U}, 2U),
																				 LogSequenceNumber(2U))
							.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	NativeReadSession session;
	const std::array<uint64_t, 1U> nodeIds{1001U};
	const std::array<uint64_t, 1U> requiredLabels{7U};
	// A prior label-only lookup must not be treated as an authenticated entity
	// record when the shaped batch still needs the Node payload.
	session.putNodeLabels(1001U, {7U, 9U});
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	EXPECT_EQ(adapter.readNodeLabelMatchesSorted(snapshot, nodeIds, requiredLabels, &session),
			  (std::vector<bool>{true}));
	const auto first = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_EQ(first.at("native_graph.read.node_sorted_batch_calls").totalValue, 1);
	EXPECT_EQ(adapter.readNodeLabelMatchesSorted(snapshot, nodeIds, requiredLabels, &session),
			  (std::vector<bool>{true}));
	const auto second = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(second.contains("native_graph.read.node_sorted_batch_calls"));
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReadsStrictlyOrderedShapedEntityBatchesThroughOneSession) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(36U, 906U, 601U, 45U), 1001U, node({7U, 0U, 0U}, 1U),
																 LogSequenceNumber(2U))
							.has_value());
	ASSERT_TRUE(entities.allocateNode(nodePool(37U, 907U, 602U, 46U), 1002U, node({9U, 0U, 0U}, 1U),
																 LogSequenceNumber(3U))
							.has_value());
		ASSERT_TRUE(entities.allocateEdge(nodePool(38U, 908U, 603U, 47U), 2001U,
															 EdgeEntityRecord{1001U, 1002U, 41U}, LogSequenceNumber(4U))
							.has_value());
		SlottedPagePool slottedPages(io, *metadata);
		EntityLabelSetStore labels(io, *metadata, slottedPages);
		PropertyCollectionRootStore properties(slottedPages, *metadata);
		BTreePageStore trees(slottedPages, *metadata);
		PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
		TestPageBackedGraphNameResolver names;
		PageBackedLogicalGraphReadAdapter logical(pages, names);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		NativeReadSession session;
		const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
		const std::vector<LogicalGraphNode> nodes = logical.readNodesShapedSorted(
				snapshot, nodeIds, EntityReadShape{.include_labels = true}, &session);
		ASSERT_EQ(nodes.size(), 2U);
		EXPECT_EQ(nodes[0].node_id, 1001U);
		EXPECT_EQ(nodes[0].labels, (std::vector<std::string>{"Person"}));
		EXPECT_EQ(nodes[1].node_id, 1002U);
		EXPECT_EQ(nodes[1].labels, (std::vector<std::string>{"VIP"}));
		const std::array<uint64_t, 1U> edgeIds{2001U};
		const std::vector<LogicalGraphEdge> edges = logical.readEdgesShapedSorted(
				snapshot, edgeIds, EntityReadShape{.include_type = true}, &session);
		ASSERT_EQ(edges.size(), 1U);
		EXPECT_EQ(edges.front(), (LogicalGraphEdge{2001U, 1001U, 1002U, "KNOWS", {}}));
		EXPECT_THROW(static_cast<void>(logical.readNodesShapedSorted(
				snapshot, std::array<uint64_t, 2U>{1002U, 1001U}, EntityReadShape{}, &session)),
					 std::exception);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, PreservesShapedBatchResultsBeyondSessionEntityCacheCapacity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	constexpr uint64_t FIRST_NODE_ID = 10'001U;
	constexpr size_t NODE_COUNT = NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 1U;
	// A native Entity page has a bounded fixed-record capacity. Build three pages
	// with one batch mutation each so the read request spans more entries than the
	// session cache can retain.
	constexpr size_t FIRST_PAGE_NODES = 60U;
	constexpr size_t SECOND_PAGE_NODES = 60U;
	const auto allocate = [&](uint64_t nodeId, uint64_t segmentId, uint64_t pageId, uint64_t slot,
							 LogSequenceNumber lsn) {
		return entities.allocateNode(nodePool(segmentId, 5'000U + segmentId, pageId, slot), nodeId,
								 node({0U, 0U, 0U}, 0U), lsn);
	};
	ASSERT_TRUE(allocate(FIRST_NODE_ID, 1'001U, 20'001U, 1'001U, LogSequenceNumber(2U)).has_value());
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	std::vector<EntityPageNodeInsertion> firstInsertions;
	firstInsertions.reserve(FIRST_PAGE_NODES - 1U);
	for (size_t index = 1U; index < FIRST_PAGE_NODES; ++index) {
		firstInsertions.push_back({FIRST_NODE_ID + index, node({0U, 0U, 0U}, 0U)});
	}
	const std::array<EntityPageBatchMutation, 1U> firstMutation{{
		{PageId(20'001U), EntityPageRecordKind::EPRK_NODE, {}, std::move(firstInsertions), {}, {}, {}, {}, {}, {}}}};
	const EntityPageBatchResult firstResult = entities.applyBatch(firstSnapshot, firstMutation, LogSequenceNumber(3U));
	firstSnapshot.release();
	ASSERT_FALSE(firstResult.pages.empty());

	ASSERT_TRUE(allocate(FIRST_NODE_ID + FIRST_PAGE_NODES, 1'002U, 20'002U, 1'002U, LogSequenceNumber(4U)).has_value());
	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	std::vector<EntityPageNodeInsertion> secondInsertions;
	secondInsertions.reserve(SECOND_PAGE_NODES - 1U);
	for (size_t index = 1U; index < SECOND_PAGE_NODES; ++index) {
		secondInsertions.push_back({FIRST_NODE_ID + FIRST_PAGE_NODES + index, node({0U, 0U, 0U}, 0U)});
	}
	const std::array<EntityPageBatchMutation, 1U> secondMutation{{
		{PageId(20'002U), EntityPageRecordKind::EPRK_NODE, {}, std::move(secondInsertions), {}, {}, {}, {}, {}, {}}}};
	const EntityPageBatchResult secondResult = entities.applyBatch(secondSnapshot, secondMutation, LogSequenceNumber(5U));
	secondSnapshot.release();
	ASSERT_FALSE(secondResult.pages.empty());

	const uint64_t thirdNodeId = FIRST_NODE_ID + FIRST_PAGE_NODES + SECOND_PAGE_NODES;
	ASSERT_TRUE(allocate(thirdNodeId, 1'003U, 20'003U, 1'003U, LogSequenceNumber(6U)).has_value());
	MetadataSnapshotPin thirdSnapshot = metadata->openSnapshot();
	std::vector<EntityPageNodeInsertion> thirdInsertions;
	thirdInsertions.reserve(NODE_COUNT - FIRST_PAGE_NODES - SECOND_PAGE_NODES - 1U);
	for (size_t index = 1U; index < NODE_COUNT - FIRST_PAGE_NODES - SECOND_PAGE_NODES; ++index) {
		thirdInsertions.push_back({thirdNodeId + index, node({0U, 0U, 0U}, 0U)});
	}
	const std::array<EntityPageBatchMutation, 1U> thirdMutation{{
		{PageId(20'003U), EntityPageRecordKind::EPRK_NODE, {}, std::move(thirdInsertions), {}, {}, {}, {}, {}, {}}}};
	const EntityPageBatchResult thirdResult = entities.applyBatch(thirdSnapshot, thirdMutation, LogSequenceNumber(7U));
	thirdSnapshot.release();
	ASSERT_FALSE(thirdResult.pages.empty());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	NativeReadSession session;
	std::vector<uint64_t> nodeIds;
	nodeIds.reserve(NODE_COUNT);
	for (size_t index = 0U; index < NODE_COUNT; ++index) {
		nodeIds.push_back(FIRST_NODE_ID + index);
	}
	const std::vector<NodeEntityRecord> records = adapter.readNodeRecordsSorted(snapshot, nodeIds, &session);
	ASSERT_EQ(records.size(), NODE_COUNT);
	EXPECT_TRUE(std::ranges::all_of(records, [](const NodeEntityRecord &record) {
		return record.inline_label_count == 0U;
	}));
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, PreservesTopologyBatchResultsBeyondSessionEntityCacheCapacity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	constexpr uint64_t FIRST_EDGE_ID = 30'001U;
	constexpr size_t EDGE_COUNT = NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 1U;
	constexpr size_t FIRST_PAGE_EDGES = 100U;
	const EdgeEntityRecord edgeRecord{1U, 2U, 0U};
	ASSERT_TRUE(entities.allocateEdge(nodePool(2'001U, 7'001U, 30'001U, 2'001U), FIRST_EDGE_ID, edgeRecord,
									 LogSequenceNumber(2U))
						.has_value());
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	std::vector<EntityPageEdgeInsertion> firstInsertions;
	firstInsertions.reserve(FIRST_PAGE_EDGES - 1U);
	for (size_t index = 1U; index < FIRST_PAGE_EDGES; ++index) {
		firstInsertions.push_back({FIRST_EDGE_ID + index, edgeRecord});
	}
	const std::array<EntityPageBatchMutation, 1U> firstMutation{{
		{PageId(30'001U), EntityPageRecordKind::EPRK_EDGE, {}, {}, {}, {}, std::move(firstInsertions), {}, {}, {}}}};
	const EntityPageBatchResult firstResult = entities.applyBatch(firstSnapshot, firstMutation, LogSequenceNumber(3U));
	firstSnapshot.release();
	ASSERT_FALSE(firstResult.pages.empty());

	const uint64_t secondFirstEdge = FIRST_EDGE_ID + FIRST_PAGE_EDGES;
	ASSERT_TRUE(entities.allocateEdge(nodePool(2'002U, 7'002U, 30'002U, 2'002U), secondFirstEdge, edgeRecord,
									 LogSequenceNumber(4U))
						.has_value());
	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	std::vector<EntityPageEdgeInsertion> secondInsertions;
	secondInsertions.reserve(EDGE_COUNT - FIRST_PAGE_EDGES - 1U);
	for (size_t index = 1U; index < EDGE_COUNT - FIRST_PAGE_EDGES; ++index) {
		secondInsertions.push_back({secondFirstEdge + index, edgeRecord});
	}
	const std::array<EntityPageBatchMutation, 1U> secondMutation{{
		{PageId(30'002U), EntityPageRecordKind::EPRK_EDGE, {}, {}, {}, {}, std::move(secondInsertions), {}, {}, {}}}};
	const EntityPageBatchResult secondResult = entities.applyBatch(secondSnapshot, secondMutation, LogSequenceNumber(5U));
	secondSnapshot.release();
	ASSERT_FALSE(secondResult.pages.empty());

	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	NativeReadSession session;
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	std::vector<uint64_t> edgeIds;
	edgeIds.reserve(EDGE_COUNT);
	for (size_t index = 0U; index < EDGE_COUNT; ++index) {
		edgeIds.push_back(FIRST_EDGE_ID + index);
	}
	const std::vector<EdgeEntityRecord> records = adapter.readEdgeTopologiesSorted(snapshot, edgeIds, &session);
	ASSERT_EQ(records.size(), EDGE_COUNT);
	EXPECT_TRUE(std::ranges::all_of(records, [&](const EdgeEntityRecord &record) { return record == edgeRecord; }));
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ConvertsNativeRecordsToDirectOwnerKeyedLogicalProperties) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(42U, 912U, 801U, 51U), 1001U, node({7U, 9U, 0U}, 2U),
																								 LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(43U, 913U, 802U, 52U), 2001U,
											 EdgeEntityRecord{1001U, 1002U, 41U},
														 LogSequenceNumber(3U))
					.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	const std::array roots{
			propertyRoot(1001U, graph::Node::typeId,
						 {{3U, graph::PropertyValue("Ada")}, {5U, graph::PropertyValue(int64_t(42))}}),
			propertyRoot(2001U, graph::Edge::typeId, {{7U, graph::PropertyValue(int64_t(2))}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter adapter(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_EQ(adapter.readNode(snapshot, 1001U),
			  (LogicalGraphNode{1001U,
									{"Person", "VIP"},
									{{"name", graph::PropertyValue("Ada")}, {"age", graph::PropertyValue(int64_t(42))}}}));
	EXPECT_EQ(adapter.readEdge(snapshot, 2001U),
			  (LogicalGraphEdge{2001U,
									1001U,
									1002U,
									"KNOWS",
									{{"weight", graph::PropertyValue(int64_t(2))}}}));
	const LogicalGraphNodePage page = adapter.scanNodesAfter(snapshot, 0U, 1U, 8U);
	ASSERT_EQ(page.nodes.size(), 1U);
	EXPECT_EQ(page.nodes.front().properties.size(), 2U);
	std::vector<EntityRef> nodeRefs;
	EXPECT_TRUE(adapter.visitNodeRefsUntil(snapshot, 8U, [&nodeRefs](const EntityRef &reference) {
		nodeRefs.push_back(reference);
		return true;
	}));
	EXPECT_EQ(nodeRefs, (std::vector<EntityRef>{{1001U, false, 0U, 0U, 0U}}));
	std::vector<std::pair<EntityRef, bool>> labelMatches;
	EXPECT_TRUE(adapter.visitNodeRefsWithLabelMatchesUntil(
			snapshot, 8U, {"Person"}, [&labelMatches](const EntityRef &reference, const bool matches) {
				labelMatches.emplace_back(reference, matches);
				return true;
			}));
	ASSERT_EQ(labelMatches.size(), 1U);
	EXPECT_EQ(labelMatches.front().first, (EntityRef{1001U, false, 0U, 0U, 0U}));
	EXPECT_TRUE(labelMatches.front().second);
	labelMatches.clear();
	EXPECT_TRUE(adapter.visitNodeRefsWithLabelMatchesUntil(
			snapshot, 8U, {"Missing"}, [&labelMatches](const EntityRef &reference, const bool matches) {
				labelMatches.emplace_back(reference, matches);
				return true;
			}));
	ASSERT_EQ(labelMatches.size(), 1U);
	EXPECT_FALSE(labelMatches.front().second);
	std::vector<EntityRef> edgeRefs;
	NativeReadSession scanSession;
	EXPECT_TRUE(pages.visitNodeRefsUntil(snapshot, 8U, [](const EntityRef &) { return true; }, &scanSession));
	ASSERT_TRUE(scanSession.node(1001U).has_value());
	EXPECT_EQ(scanSession.node(1001U)->inline_label_count, 2U);
	const std::array<uint64_t, 1U> personLabelToken{7U};
	EXPECT_TRUE(pages.visitNodeRefsWithLabelTokenMatchesUntil(
			snapshot, 8U, personLabelToken, [](const EntityRef &, bool) { return true; }, &scanSession));
	// Label-aware scans reuse the same authenticated fixed record rather than
	// forcing a second Entity Map lookup at the result/binding boundary.
	ASSERT_TRUE(scanSession.node(1001U).has_value());
	EXPECT_TRUE(pages.visitEdgeRefsUntil(snapshot, 8U, [&edgeRefs](const EntityRef &reference) {
			edgeRefs.push_back(reference);
			return true;
	}, &scanSession));
	EXPECT_EQ(edgeRefs, (std::vector<EntityRef>{{2001U, true, 1001U, 1002U, 41U}}));
	ASSERT_TRUE(scanSession.edge(2001U).has_value());
	EXPECT_EQ(scanSession.edge(2001U)->type_id, 41U);
	EXPECT_TRUE(pages.visitEdgeTopologiesUntil(
			snapshot, 8U, [&scanSession](uint64_t edgeId, const EdgeEntityRecord &) {
				return scanSession.edge(edgeId).has_value();
			}, &scanSession));
	TestPageBackedGraphNameResolver duplicateNames(true);
	PageBackedLogicalGraphReadAdapter duplicateAdapter(pages, duplicateNames);
	EXPECT_THROW(static_cast<void>(duplicateAdapter.readNode(snapshot, 1001U)), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsUnknownLogicalNamesButAllowsAnUntypedNativeRelationship) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(44U, 914U, 803U, 53U), 1001U, node({11U, 0U, 0U}, 1U),
																										 LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(45U, 915U, 804U, 54U), 2001U,
																								 EdgeEntityRecord{1001U, 1002U, 99U},
																								 LogSequenceNumber(3U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(46U, 916U, 805U, 55U), 2003U,
																								 EdgeEntityRecord{1001U, 1002U, 0U},
																								 LogSequenceNumber(4U))
					.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitEdgeTopologiesUntil(
				 snapshot, 3U, [](const LogicalGraphEdgeTopology &) { return true; })),
			 FormatException);
	const LogicalGraphEdge untyped = logical.readEdge(snapshot, 2003U);
	EXPECT_TRUE(untyped.type.empty());
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsDuplicateAndUnknownPropertyNamesAtTheLogicalBoundary) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(48U, 918U, 807U, 57U), 1001U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(entities.allocateNode(nodePool(49U, 919U, 808U, 58U), 1003U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(3U))
						.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(50U, 920U, 809U, 59U), 2001U,
																							 EdgeEntityRecord{1001U, 1003U, 41U},
																							 LogSequenceNumber(4U))
						.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(51U, 921U, 810U, 60U), 2003U,
																							 EdgeEntityRecord{1003U, 1001U, 41U},
																							 LogSequenceNumber(5U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	const std::array roots{
			propertyRoot(1001U, graph::Node::typeId,
						 {{3U, graph::PropertyValue("Ada")}, {9U, graph::PropertyValue("duplicate")}}),
			propertyRoot(1003U, graph::Node::typeId, {{11U, graph::PropertyValue("unknown")}}),
			propertyRoot(2001U, graph::Edge::typeId,
						 {{3U, graph::PropertyValue("first")}, {9U, graph::PropertyValue("duplicate")}}),
			propertyRoot(2003U, graph::Edge::typeId, {{11U, graph::PropertyValue("unknown")}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1003U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2003U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdgesShapedSorted(snapshot, std::array<uint64_t, 1U>{2001U},
		EntityReadShape{.all_properties = true})), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ScansNativeEntityMapInLogicalOrderWithResumableNodeBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(35U, 905U, 401U, 44U), 1001U, node({7U, 0U, 0U}, 1U),
																								 LogSequenceNumber(2U))
					.has_value());
	MetadataSnapshotPin afterFirstNode = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(afterFirstNode, PageId(401U), 1003U, node({9U, 0U, 0U}, 1U),
																									 LogSequenceNumber(3U)));
	MetadataSnapshotPin afterSecondNode = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(afterSecondNode, PageId(401U), 1005U, node({11U, 0U, 0U}, 1U),
																									  LogSequenceNumber(4U)));
	ASSERT_TRUE(entities.allocateEdge(nodePool(36U, 906U, 402U, 45U), 2001U,
											 EdgeEntityRecord{1001U, 1003U, 31U},
														 LogSequenceNumber(5U))
					.has_value());
	MetadataSnapshotPin afterFirstEdge = metadata->openSnapshot();
	static_cast<void>(entities.insertEdge(afterFirstEdge, PageId(402U), 2003U,
																	  EdgeEntityRecord{1003U, 1005U, 33U},
																			  LogSequenceNumber(6U)));
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);

	std::vector<uint64_t> oldNodeIds;
	EXPECT_TRUE(adapter.visitNodesUntil(afterFirstNode, 4U, [&oldNodeIds](const PageBackedGraphNode &record) {
		oldNodeIds.push_back(record.node_id);
		return true;
	}));
	EXPECT_EQ(oldNodeIds, (std::vector<uint64_t>{1001U}));

	MetadataSnapshotPin current = metadata->openSnapshot();
	std::vector<uint64_t> boundedNodeIds;
	EXPECT_FALSE(adapter.visitNodesUntil(current, 2U, [&boundedNodeIds](const PageBackedGraphNode &record) {
		boundedNodeIds.push_back(record.node_id);
		return true;
	}));
	EXPECT_EQ(boundedNodeIds, (std::vector<uint64_t>{1001U, 1003U}));
	const PageBackedGraphNodePage firstPage = adapter.scanNodesAfter(current, 1001U, 1U, 8U);
	ASSERT_EQ(firstPage.nodes.size(), 1U);
	EXPECT_EQ(firstPage.nodes.front().node_id, 1003U);
	EXPECT_TRUE(firstPage.may_have_more);
	EXPECT_GT(firstPage.metadata_pages_visited, 0U);
	EXPECT_LE(firstPage.metadata_pages_visited, 8U);
	const PageBackedGraphNodePage finalPage = adapter.scanNodesAfter(current, 1003U, 4U, 8U);
	ASSERT_EQ(finalPage.nodes.size(), 1U);
	EXPECT_EQ(finalPage.nodes.front().node_id, 1005U);
	EXPECT_FALSE(finalPage.may_have_more);
	const PageBackedGraphNodePage terminalPage =
			adapter.scanNodesAfter(current, std::numeric_limits<uint64_t>::max(), 1U, 1U);
	EXPECT_TRUE(terminalPage.nodes.empty());
	EXPECT_FALSE(terminalPage.may_have_more);
	EXPECT_EQ(terminalPage.metadata_pages_visited, 0U);

	std::vector<uint64_t> edgeIds;
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	EXPECT_TRUE(adapter.visitEdgesUntil(current, 4U, [&edgeIds](const PageBackedGraphEdge &record) {
		edgeIds.push_back(record.edge_id);
		return true;
	}));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{2001U, 2003U}));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_native_materialization"));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_logical_visitor"));
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(current, 0U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(current, 0U, 1U, 0U)), FormatException);
	afterFirstNode.release();
	afterSecondNode.release();
	afterFirstEdge.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ResumesNativeAdjacencyThroughLogicalEdgeIdsOnly) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = transaction.createEdge(source, firstTarget, 41U);
	const uint64_t secondEdge = transaction.createEdge(source, secondTarget, 43U);
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin current = metadata->openSnapshot();

	const PageBackedGraphAdjacencyPage firstPage = adapter.scanAdjacentEdges(
			current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(firstPage.edges.size(), 1U);
	EXPECT_EQ(firstPage.edges.front().edge_id, firstEdge);
	ASSERT_TRUE(firstPage.continuation.has_value());
	EXPECT_EQ(*firstPage.continuation, (PageBackedGraphAdjacencyContinuation{firstEdge, secondEdge}));
	const PageBackedGraphAdjacencyPage finalPage = adapter.scanAdjacentEdges(
			current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, firstPage.continuation, 1U);
	ASSERT_EQ(finalPage.edges.size(), 1U);
	EXPECT_EQ(finalPage.edges.front().edge_id, secondEdge);
	EXPECT_FALSE(finalPage.continuation.has_value());
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
				 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING,
				 PageBackedGraphAdjacencyContinuation{firstEdge, secondEdge + 1U}, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
			 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U,
			 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeTopologiesUntil(
			 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U,
			 [](uint64_t, const EdgeEntityRecord &) { return true; })),
			 FormatException);
	size_t stoppedTopologies = 0U;
	EXPECT_FALSE(adapter.visitAdjacentEdgeTopologiesUntil(
			 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, 2U,
			 [&stoppedTopologies](uint64_t, const EdgeEntityRecord &) {
				++stoppedTopologies;
				return false;
			}));
	EXPECT_EQ(stoppedTopologies, 1U);
	PageBackedGraphAdjacencyCursor pageCursor =
			adapter.openAdjacentEdgeCursor(current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING);
	PageBackedGraphAdjacencyCursor movedPageCursor = std::move(pageCursor);
	EXPECT_THROW(static_cast<void>(pageCursor.next(current, std::nullopt, 1U)), FormatException);
	EXPECT_EQ(movedPageCursor.next(current, std::nullopt, 1U).edges.front().edge_id, firstEdge);

	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);
	NativeReadSession readSession;
	std::vector<LogicalGraphEdgeTopology> topologies;
	EXPECT_TRUE(logical.visitEdgeTopologiesUntil(current, 3U, [&topologies](const LogicalGraphEdgeTopology &record) {
		topologies.push_back(record);
		return true;
	}, &readSession));
	EXPECT_EQ(topologies,
			  (std::vector<LogicalGraphEdgeTopology>{{firstEdge, source, firstTarget, "KNOWS"},
														 {secondEdge, source, secondTarget, "FOLLOWS"}}));
	// Topology-only consumers must not enter the owner Property namespace.
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	std::vector<LogicalGraphEdgeTopology> adjacentTopologies;
	EXPECT_TRUE(logical.visitAdjacentEdgeTopologiesUntil(
				current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
				[&adjacentTopologies](const LogicalGraphEdgeTopology &record) {
					adjacentTopologies.push_back(record);
					return true;
				}, &readSession));
	EXPECT_EQ(adjacentTopologies,
			  (std::vector<LogicalGraphEdgeTopology>{{firstEdge, source, firstTarget, "KNOWS"},
																																				 {secondEdge, source, secondTarget, "FOLLOWS"}}));
	NativeReadSession topologyBatchSession;
	TestPageBackedGraphNameResolver topologyNames;
	PageBackedLogicalGraphReadAdapter topologyLogical(adapter, topologyNames);
	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	EXPECT_EQ(topologyLogical.scanAdjacentEdgeTopologies(current, source, LogicalGraphReadDirection::LGRD_OUTGOING,
			std::nullopt, 2U, &topologyBatchSession).edges.size(), 2U);
	const auto firstTopologyTrace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_GE(firstTopologyTrace.at("native_graph.read.edge_sorted_batch_calls").totalValue, 1);
	EXPECT_EQ(firstTopologyTrace.at("native_graph.read.edge_sorted_batch_entries").totalValue, 2);
	EXPECT_EQ(topologyLogical.scanAdjacentEdgeTopologies(current, source, LogicalGraphReadDirection::LGRD_OUTGOING,
			std::nullopt, 2U, &topologyBatchSession).edges.size(), 2U);
	const auto secondTopologyTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	EXPECT_EQ(secondTopologyTrace.contains("native_graph.read.edge_sorted_batch_calls"), false);
	std::vector<LogicalGraphEdgeTopology> incomingTopologies;
	EXPECT_TRUE(logical.visitAdjacentEdgeTopologiesUntil(
			current, firstTarget, LogicalGraphReadDirection::LGRD_INCOMING, 2U,
			[&incomingTopologies](const LogicalGraphEdgeTopology &record) {
				incomingTopologies.push_back(record);
				return true;
			}, &readSession));
	EXPECT_EQ(incomingTopologies,
			  (std::vector<LogicalGraphEdgeTopology>{{firstEdge, source, firstTarget, "KNOWS"}}));
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	const LogicalGraphAdjacencyPage logicalFirst =
			logical.scanAdjacentEdges(current, source, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U,
																&readSession);
	ASSERT_EQ(logicalFirst.edges.size(), 1U);
	EXPECT_EQ(logicalFirst.edges.front().edge_id, firstEdge);
	EXPECT_EQ(logicalFirst.edges.front().type, "KNOWS");
	ASSERT_TRUE(logicalFirst.continuation.has_value());
	EXPECT_EQ(*logicalFirst.continuation, (LogicalGraphAdjacencyContinuation{firstEdge, secondEdge}));
	const LogicalGraphAdjacencyPage logicalFinal = logical.scanAdjacentEdges(
			current, source, LogicalGraphReadDirection::LGRD_OUTGOING, logicalFirst.continuation, 1U, &readSession);
	ASSERT_EQ(logicalFinal.edges.size(), 1U);
	EXPECT_EQ(logicalFinal.edges.front().edge_id, secondEdge);
	EXPECT_EQ(logicalFinal.edges.front().type, "FOLLOWS");
	EXPECT_FALSE(logicalFinal.continuation.has_value());

	const LogicalGraphAdjacencyPage incoming =
			logical.scanAdjacentEdges(current, firstTarget, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U,
																&readSession);
	ASSERT_EQ(incoming.edges.size(), 1U);
	EXPECT_EQ(incoming.edges.front().edge_id, firstEdge);
	EXPECT_THROW(static_cast<void>(logical.scanAdjacentEdges(
				 current, source, static_cast<LogicalGraphReadDirection>(99U), std::nullopt, 1U)),
			 FormatException);

	PageBackedLogicalGraphAdjacencyCursor cursor =
			logical.openAdjacentEdgeCursor(current, source, LogicalGraphReadDirection::LGRD_OUTGOING, &readSession);
	const LogicalGraphAdjacencyPage cursorFirst = cursor.next(current, std::nullopt, 1U);
	ASSERT_TRUE(cursorFirst.continuation.has_value());
	EXPECT_EQ(cursorFirst.edges.front().edge_id, firstEdge);
	PageBackedLogicalGraphAdjacencyCursor movedCursor(std::move(cursor));
	EXPECT_THROW(static_cast<void>(cursor.next(current, std::nullopt, 1U)), FormatException);
	PageBackedLogicalGraphAdjacencyCursor assignedCursor =
			logical.openAdjacentEdgeCursor(current, source, LogicalGraphReadDirection::LGRD_OUTGOING, &readSession);
	assignedCursor = std::move(movedCursor);
	EXPECT_THROW(static_cast<void>(movedCursor.next(current, std::nullopt, 1U)), FormatException);
	const LogicalGraphAdjacencyPage cursorFinal = assignedCursor.next(current, cursorFirst.continuation, 1U);
	ASSERT_EQ(cursorFinal.edges.size(), 1U);
	EXPECT_EQ(cursorFinal.edges.front().edge_id, secondEdge);
	EXPECT_FALSE(cursorFinal.continuation.has_value());

	std::vector<uint64_t> logicalNodes;
	EXPECT_TRUE(logical.visitNodesUntil(current, 4U, [&logicalNodes](const LogicalGraphNode &record) {
		logicalNodes.push_back(record.node_id);
		return true;
	}, &readSession));
	EXPECT_EQ(logicalNodes, (std::vector<uint64_t>{source, firstTarget, secondTarget}));
	std::vector<uint64_t> logicalEdges;
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	EXPECT_TRUE(logical.visitEdgesUntil(current, 3U, [&logicalEdges](const LogicalGraphEdge &record) {
		logicalEdges.push_back(record.edge_id);
		return true;
	}, &readSession));
	graph::debug::PerfTrace::setEnabled(false);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_EQ(logicalEdges, (std::vector<uint64_t>{firstEdge, secondEdge}));
	EXPECT_GT(trace.at("native_graph.read.edge_entity_record").calls, 0U);
	EXPECT_GT(trace.at("native_graph.read.edge_name_materialization").calls, 0U);
	size_t untracedLogicalEdges = 0U;
	EXPECT_FALSE(logical.visitEdgesUntil(current, 3U, [&](const LogicalGraphEdge &) {
		++untracedLogicalEdges;
		return false;
	}, &readSession));
	EXPECT_EQ(untracedLogicalEdges, 1U);
	std::vector<uint64_t> visitedAdjacent;
	EXPECT_TRUE(logical.visitAdjacentEdgesUntil(
			current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
			[&visitedAdjacent](const LogicalGraphEdge &record) {
				visitedAdjacent.push_back(record.edge_id);
				return true;
			}, &readSession));
	EXPECT_EQ(visitedAdjacent, (std::vector<uint64_t>{firstEdge, secondEdge}));
	// The same pinned snapshot resolves each relationship token once even when
	// multiple traversal APIs revisit the same native Edge records.
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	EXPECT_THROW(static_cast<void>(logical.visitNodesUntil(current, 3U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitEdgesUntil(current, 2U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitAdjacentEdgesUntil(
				 current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U, {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitEdgeTopologiesUntil(current, 2U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitAdjacentEdgeTopologiesUntil(
				 current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U, {})),
			 FormatException);
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, StreamsSupernodeVisitorsInBoundedLogicalBatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	std::vector<uint64_t> expected;
	expected.reserve(257U);
	for (size_t ordinal = 0U; ordinal < 257U; ++ordinal) {
		const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
		expected.push_back(transaction.createEdge(source, target, 41U));
	}
	ASSERT_TRUE(transaction.commit().committed());

	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	std::vector<uint64_t> visited;
	EXPECT_TRUE(adapter.visitAdjacentEdgesUntil(
			snapshot, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, expected.size(),
			[&visited](const PageBackedGraphEdge &edge) {
				visited.push_back(edge.edge_id);
				return true;
			}));
	EXPECT_EQ(visited, expected);

	size_t earlyVisits = 0U;
	EXPECT_FALSE(adapter.visitAdjacentEdgesUntil(
			snapshot, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, expected.size(),
			[&earlyVisits](const PageBackedGraphEdge &) {
				++earlyVisits;
				return false;
			}));
	EXPECT_EQ(earlyVisits, 1U);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, StreamsLargeAdjacencyTopologyThroughTypedEntityBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t unrelatedSource = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t unrelatedTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	static_cast<void>(transaction.createEdge(unrelatedSource, unrelatedTarget, 47U));
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	constexpr size_t EDGE_COUNT = 513U;
	std::vector<uint64_t> expectedEdgeIds;
	expectedEdgeIds.reserve(EDGE_COUNT);
	for (size_t ordinal = 0U; ordinal < EDGE_COUNT; ++ordinal) {
		expectedEdgeIds.push_back(transaction.createEdge(source, target, ordinal % 2U == 0U ? 41U : 43U));
	}
	ASSERT_TRUE(transaction.commit().committed());

	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	const std::array propertyRoots{
		propertyRoot(expectedEdgeIds.front(), graph::Edge::typeId, {{7U, graph::PropertyValue("present")}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, propertyRoots, propertyOptions()));
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();

	size_t visited = 0U;
	uint64_t firstTarget = 0U;
	uint64_t lastEdgeId = 0U;
	EXPECT_TRUE(logical.visitAdjacentEdgeTopologiesUntil(snapshot, source, LogicalGraphReadDirection::LGRD_OUTGOING,
														 EDGE_COUNT + 1U, [&](const LogicalGraphEdgeTopology &edge) {
															 if (visited == 0U) {
																 firstTarget = edge.target_node_id;
															 }
															 ++visited;
															 lastEdgeId = edge.edge_id;
															 return true;
														 }));
	EXPECT_EQ(visited, EDGE_COUNT);
	EXPECT_EQ(lastEdgeId, expectedEdgeIds.back());
	EXPECT_NE(firstTarget, 0U);
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	const auto batchTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	EXPECT_EQ(batchTrace.at("native_graph.read.edge_sorted_batch_calls").totalValue, 1);
	EXPECT_EQ(batchTrace.at("native_graph.read.edge_sorted_batch_entries").totalValue,
			  static_cast<int64_t>(EDGE_COUNT));
	visited = 0U;
	EXPECT_FALSE(logical.visitAdjacentEdgeTopologiesUntil(snapshot, source, LogicalGraphReadDirection::LGRD_OUTGOING,
														  EDGE_COUNT + 1U, [&](const LogicalGraphEdgeTopology &) {
															  ++visited;
															  return visited < 3U;
														  }));
	EXPECT_EQ(visited, 3U);

	std::vector<uint64_t> incomingEdgeIds;
	EXPECT_TRUE(adapter.visitAdjacentEdgeTopologiesUntil(
			snapshot, target, PageBackedGraphReadDirection::PBGRD_INCOMING, EDGE_COUNT + 1U,
			[&incomingEdgeIds, source, target](uint64_t edgeId, const EdgeEntityRecord &edge) {
				EXPECT_EQ(edge.source_node_id, source);
				EXPECT_EQ(edge.target_node_id, target);
				incomingEdgeIds.push_back(edgeId);
				return true;
			}));
	EXPECT_EQ(incomingEdgeIds, expectedEdgeIds);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReopensAndScansNativeEntityMapWithoutFixedRecords) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		EntityPageStore entities(io, *metadata);
		ASSERT_TRUE(entities.allocateNode(nodePool(39U, 909U, 601U, 48U), 1001U, node({7U, 0U, 0U}, 1U),
																								   LogSequenceNumber(2U))
					.has_value());
		MetadataSnapshotPin first = metadata->openSnapshot();
		static_cast<void>(entities.insertNode(first, PageId(601U), 1002U, node({9U, 0U, 0U}, 1U),
																										LogSequenceNumber(3U)));
		first.release();
	}

	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *reopened);
	SlottedPagePool slottedPages(io, *reopened);
	EntityLabelSetStore labels(io, *reopened, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *reopened);
	BTreePageStore trees(slottedPages, *reopened);
	PageBackedGraphReadAdapter adapter(*reopened, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const PageBackedGraphNodePage page = adapter.scanNodesAfter(snapshot, 0U, 4U, 8U);
	ASSERT_EQ(page.nodes.size(), 2U);
	EXPECT_EQ(page.nodes[0].node_id, 1001U);
	EXPECT_EQ(page.nodes[1].node_id, 1002U);
	EXPECT_FALSE(page.may_have_more);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, PointReadsAndTypedNodeScansRejectCorruptEntityMap) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(40U, 910U, 701U, 49U), 1001U, node({7U, 0U, 0U}, 1U),
																								 LogSequenceNumber(2U))
					.has_value());
	const std::optional<EntityPageInsertResult> edge = entities.allocateEdge(
			nodePool(41U, 911U, 702U, 50U), 2001U, EdgeEntityRecord{1001U, 1002U, 41U},
			LogSequenceNumber(3U));
	ASSERT_TRUE(edge.has_value());
	const EntityPageLocation corruptNodeLocation{1002U, edge->location.page_id, edge->location.slot_id,
																										 edge->location.slot_generation};
	const std::array corruptNodes{corruptNodeLocation};
	static_cast<void>(metadata->upsertEntityPageLocations(corruptNodes, {}, {}, {}, LogSequenceNumber(4U)));
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	// Point reads and typed Entity-directory scans must reject a Node route into
	// an Edge page. A cross-directory mapping is corruption, not a record to skip.
	EXPECT_THROW(static_cast<void>(adapter.readNode(snapshot, 1002U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(snapshot, 4U,
			[](const PageBackedGraphNode &) { return true; })), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsZeroLogicalIdsBeforeAccessingDirectories) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(adapter.readNode(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.readEdge(snapshot, 0U)), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, MovesAdjacencyCursorsWithoutLeavingTheirOldScanStateUsable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(47U, 917U, 806U, 56U), 1001U, node({7U, 0U, 0U}, 1U),
																	 LogSequenceNumber(2U))
					.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	PageBackedGraphAdjacencyCursor cursor =
			adapter.openAdjacentEdgeCursor(snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING);
	PageBackedGraphAdjacencyCursor moved(std::move(cursor));
	EXPECT_THROW(static_cast<void>(cursor.next(snapshot, std::nullopt, 1U)), FormatException);
	cursor = std::move(moved);
	EXPECT_THROW(static_cast<void>(moved.next(snapshot, std::nullopt, 1U)), FormatException);
	EXPECT_NO_THROW(static_cast<void>(cursor.next(snapshot, std::nullopt, 1U)));
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsInvalidAdjacencyBoundsDirectionsAndContinuationBeforeScanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(47U, 917U, 806U, 56U), 1001U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(2U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);

	EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, properties, trees, 0U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(
					 *metadata, entities, labels, properties, trees, std::numeric_limits<size_t>::max())),
			 std::invalid_argument);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees, 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 0U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 0U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt,
			 std::numeric_limits<size_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING,
			 PageBackedGraphAdjacencyContinuation{0U, 1U}, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING,
			 PageBackedGraphAdjacencyContinuation{1U, 0U}, 1U)),
			 FormatException);
    // Invalid order rejects before any source-node lookup (node 999 is absent).
    for (const auto invalid : {PageBackedGraphAdjacencyContinuation{2U, 1U},
                              PageBackedGraphAdjacencyContinuation{2U, 2U}}) {
        try {
            static_cast<void>(adapter.scanAdjacentEdges(snapshot, 999U,
                PageBackedGraphReadDirection::PBGRD_OUTGOING, invalid, 1U));
            FAIL() << "invalid continuation order was accepted";
        } catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
        }
        try {
            static_cast<void>(adapter.scanAdjacentEdgeTopologies(snapshot, 999U,
                PageBackedGraphReadDirection::PBGRD_OUTGOING, invalid, 1U));
            FAIL() << "invalid topology continuation order was accepted";
        } catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
        }
    }
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, static_cast<PageBackedGraphReadDirection>(99U), std::nullopt, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.openAdjacentEdgeCursor(
			 snapshot, 0U, PageBackedGraphReadDirection::PBGRD_OUTGOING)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.openAdjacentEdgeCursor(
			 snapshot, 1001U, static_cast<PageBackedGraphReadDirection>(99U))),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(snapshot, 0U, 0U, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(snapshot, 0U, 1U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(snapshot, 0U,
			 [](const PageBackedGraphNode &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(snapshot, 1U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgesUntil(snapshot, 0U,
			 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgesUntil(snapshot, 1U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgeTopologiesUntil(snapshot, 0U,
			 [](uint64_t, const EdgeEntityRecord &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgeTopologiesUntil(snapshot, 1U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 0U,
			 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U, {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeTopologiesUntil(
			 snapshot, 0U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U,
			 [](uint64_t, const EdgeEntityRecord &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeTopologiesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 0U,
			 [](uint64_t, const EdgeEntityRecord &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeTopologiesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING,
			 std::numeric_limits<size_t>::max(), [](uint64_t, const EdgeEntityRecord &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeTopologiesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U, {})),
			 FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsStoresFromAnotherMetadataDomain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);

	const std::filesystem::path foreignPath = path.string() + "_foreign";
	{
		auto foreignFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																			   std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		EntityPageStore foreignEntities(foreignIo, *foreignMetadata);
		SlottedPagePool foreignSlottedPages(foreignIo, *foreignMetadata);
		EntityLabelSetStore foreignLabels(foreignIo, *foreignMetadata, foreignSlottedPages);
		PropertyCollectionRootStore foreignProperties(foreignSlottedPages, *foreignMetadata);
		BTreePageStore foreignTrees(foreignSlottedPages, *foreignMetadata);

		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, foreignEntities, labels, properties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, foreignLabels, properties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, foreignProperties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, properties, foreignTrees)),
					 std::invalid_argument);
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(PageBackedGraphReadAdapterTest, CombinedColumnsAndCountsAdmitColdWarmAndPartialCachedPayloads) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	auto transaction = planner.beginTransaction();
	const uint64_t n1 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t n2 = transaction.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(transaction.commit().committed());
	const graph::PropertyValue payload(std::string(512U, 'p'));
	const std::array roots{propertyRoot(n1, graph::Node::typeId, {{3U, payload}, {5U, graph::PropertyValue()}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	const std::array<uint64_t, 2U> ids{n1, n2};
	const std::array<uint32_t, 3U> tokens{3U, 5U, 7U};
	for (int cacheState: {0, 1, 2}) {
		SCOPED_TRACE(cacheState);
		NativeReadSession session;
		if (cacheState == 1) static_cast<void>(pages.readNodePropertyColumnsAndCountsSorted(snapshot, ids, tokens, &session));
		if (cacheState == 2) static_cast<void>(pages.readNodePropertyColumnsSorted(snapshot, ids, std::array<uint32_t, 1U>{3U}, &session));
		SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("combined source rejected"); };
		EXPECT_THROW(static_cast<void>(pages.readNodePropertyColumnsAndCountsSorted(snapshot, ids, tokens, &session, &reject)), std::length_error);
		size_t admitted = 0U;
		SelectedPropertyReadAdmission accept = [&](size_t bytes) { admitted += bytes; };
		auto result = pages.readNodePropertyColumnsAndCountsSorted(snapshot, ids, tokens, &session, &accept);
		EXPECT_EQ(admitted, 536U);
		ASSERT_EQ(result.first.size(), 3U);
		ASSERT_EQ(result.first[0].size(), 2U);
		ASSERT_TRUE(result.first[0][0]);
		EXPECT_EQ(*result.first[0][0], payload);
		ASSERT_TRUE(result.first[1][0]);
		EXPECT_EQ(result.first[1][0]->getType(), graph::PropertyType::NULL_TYPE);
		EXPECT_FALSE(result.first[2][0]);
		for (const auto &column: result.first) EXPECT_FALSE(column[1]);
		EXPECT_EQ(result.second, (std::vector<std::optional<size_t>>{2U, 0U}));
		admitted = 0U;
		auto warm = pages.readNodePropertyColumnsAndCountsSorted(snapshot, ids, tokens, &session, &accept);
		EXPECT_EQ(warm, result);
		EXPECT_EQ(admitted, 536U);
	}
	const std::array<EntityPropertyKey, 2U> owners{{{EntityPropertyOwnerKind::EPOK_NODE, n1}, {EntityPropertyOwnerKind::EPOK_NODE, n2}}};
	std::vector<std::optional<PropertyCollectionRootEntry>> entries(6U);
	std::vector<std::optional<size_t>> counts(2U);
	SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("noncursor source rejected"); };
	EXPECT_THROW(properties.readEntityPropertiesAndCountsOptionalSorted(snapshot, owners, tokens, entries, counts, nullptr, &reject), std::length_error);
	size_t admitted = 0U;
	SelectedPropertyReadAdmission accept = [&](size_t bytes) { admitted += bytes; };
	EXPECT_NO_THROW(properties.readEntityPropertiesAndCountsOptionalSorted(snapshot, owners, tokens, entries, counts, nullptr, &accept));
	EXPECT_EQ(admitted, 536U);
	EXPECT_EQ(counts, (std::vector<std::optional<size_t>>{2U, 0U}));
	ASSERT_TRUE(entries[0]);
	EXPECT_EQ(entries[0]->value, payload);
	ASSERT_TRUE(entries[1]);
	EXPECT_EQ(entries[1]->value.getType(), graph::PropertyType::NULL_TYPE);
	for (size_t index = 2U; index < entries.size(); ++index) EXPECT_FALSE(entries[index]);

}

TEST_F(PageBackedGraphReadAdapterTest, ExactValidationAdmitsActualStoredValueBeforeComparisonAndCanRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	auto transaction = planner.beginTransaction();
	const uint64_t node = transaction.createNode(std::array<uint64_t, 1U>{17U});
	ASSERT_TRUE(transaction.commit().committed());
	const graph::PropertyValue payload(std::string(512U, 'p'));
	const std::array roots{propertyRoot(node, graph::Node::typeId, {{3U, payload}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	const std::array<uint64_t, 1U> ids{node};
	const std::array<uint32_t, 1U> tokens{3U};
	const std::array<graph::PropertyValue, 1U> wrongExpected{graph::PropertyValue(1)};
	SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("actual payload rejected"); };
	EXPECT_THROW(pages.validateNodeExactPropertiesSorted(snapshot, ids, 17U, tokens, wrongExpected, 1U, nullptr, &reject), std::length_error);
	EXPECT_THROW(pages.validateNodeExactPropertiesSorted(snapshot, ids, 17U, tokens, wrongExpected, 1U), FormatException);
	const std::array<graph::PropertyValue, 1U> expected{payload};
	size_t admitted = 0U;
	SelectedPropertyReadAdmission accept = [&](size_t bytes) { admitted += bytes; };
	EXPECT_NO_THROW(pages.validateNodeExactPropertiesSorted(snapshot, ids, 17U, tokens, expected, 1U, nullptr, &accept));
	EXPECT_EQ(admitted, 526U);
}

TEST_F(PageBackedGraphReadAdapterTest, CompletePropertyAdmissionRejectsRetriesAndValidatesWholeIdBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	auto transaction = planner.beginTransaction();
	const uint64_t n1 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t n2 = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t e1 = transaction.createEdge(n1, n2, 41U);
	const uint64_t e2 = transaction.createEdge(n2, n1, 41U);
	ASSERT_TRUE(transaction.commit().committed());
	const graph::PropertyValue payload(std::string(512U, 'p'));
	const std::array roots{
		propertyRoot(n1, graph::Node::typeId, {{3U, payload}, {5U, graph::PropertyValue()}}),
		propertyRoot(e1, graph::Edge::typeId, {{3U, payload}, {5U, graph::PropertyValue()}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	NativeReadSession session;
	for (bool edge : {false, true}) {
		SCOPED_TRACE(edge);
		const std::array<uint64_t, 2U> ids = edge ? std::array{e1, e2} : std::array{n1, n2};
		size_t calls = 0U;
		const SelectedPropertyReadAdmission reject = [&](size_t) {
			++calls;
			throw std::length_error("complete Property source rejected");
		};
		const auto read = [&](std::span<const uint64_t> selected, const SelectedPropertyReadAdmission *admission) {
			return edge ? pages.readEdgePropertiesSorted(snapshot, selected, &session, admission)
						: pages.readNodePropertiesSorted(snapshot, selected, &session, admission);
		};
		EXPECT_THROW(static_cast<void>(read(ids, &reject)), std::length_error);
		EXPECT_EQ(calls, 1U);
		size_t admitted = 0U;
		const SelectedPropertyReadAdmission accept = [&](size_t bytes) { admitted += bytes; };
		for (int retry = 0; retry < 2; ++retry) {
			admitted = 0U;
			const auto result = read(ids, &accept);
			ASSERT_EQ(result.size(), 2U);
			EXPECT_EQ(result[0], roots[edge ? 1U : 0U].entries);
			EXPECT_TRUE(result[1].empty());
			// String: token 4 + header 6 + length 4 + payload 512; NULL: 4 + 6.
			EXPECT_EQ(admitted, 536U);
		}
		const std::array<uint64_t, 2U> invalid{ids[0], 0U};
		EXPECT_THROW(static_cast<void>(read(invalid, &reject)), FormatException);
		EXPECT_EQ(calls, 1U);
		EXPECT_THROW(static_cast<void>(edge ? pages.readEdgeProperties(snapshot, ids[0], &session, &reject)
				: pages.readNodeProperties(snapshot, ids[0], &session, &reject)), std::length_error);
		admitted = 0U;
		const auto point = edge ? pages.readEdgeProperties(snapshot, ids[0], &session, &accept)
				: pages.readNodeProperties(snapshot, ids[0], &session, &accept);
		EXPECT_EQ(point, roots[edge ? 1U : 0U].entries);
		EXPECT_EQ(admitted, 536U);
	}
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	const EntityReadShape complete{.all_properties = true};
	for (bool edge : {false, true}) {
		const std::array<uint64_t, 2U> ids = edge ? std::array{e1, e2} : std::array{n1, n2};
		const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("logical complete source rejected"); };
		if (edge) EXPECT_THROW(static_cast<void>(logical.readEdgesShapedSorted(snapshot, ids, complete, &session, &reject)), std::length_error);
		else EXPECT_THROW(static_cast<void>(logical.readNodesShapedSorted(snapshot, ids, complete, &session, &reject)), std::length_error);
		for (int retry = 0; retry < 2; ++retry) {
			size_t admitted = 0U;
			const SelectedPropertyReadAdmission accept = [&](size_t bytes) { admitted += bytes; };
			auto propertiesRead = edge
				? logical.readEdgesShapedSorted(snapshot, ids, complete, &session, &accept).front().properties
				: logical.readNodesShapedSorted(snapshot, ids, complete, &session, &accept).front().properties;
			EXPECT_EQ(admitted, 536U);
			ASSERT_EQ(propertiesRead.size(), 2U);
			EXPECT_EQ(propertiesRead[0].key, "name");
			EXPECT_EQ(propertiesRead[0].value, payload);
			EXPECT_EQ(propertiesRead[1].key, "age");
			EXPECT_EQ(propertiesRead[1].value.getType(), graph::PropertyType::NULL_TYPE);
			propertiesRead[0].value = graph::PropertyValue("changed result");
			const auto cached = session.property(edge, ids[0], 3U);
			ASSERT_TRUE(cached);
			EXPECT_EQ(*cached, payload);
		}
	}
	size_t retainedFrames = 0U, peakFrames = 0U;
	const auto framePage = logical.scanAdjacentEdges(snapshot, n1, LogicalGraphReadDirection::LGRD_OUTGOING,
		{}, 1U, &session, nullptr, [&](size_t bytes) { retainedFrames = bytes; peakFrames = std::max(peakFrames, bytes); });
	EXPECT_EQ(retainedFrames, framePage.edges.capacity() * sizeof(LogicalGraphEdge) + framePage.edges.front().type.capacity()
		+ framePage.edges.front().properties.capacity() * sizeof(LogicalGraphPropertyEntry) + framePage.edges.front().properties[0].key.capacity() + framePage.edges.front().properties[1].key.capacity());
	EXPECT_GE(peakFrames, retainedFrames + sizeof(PageBackedGraphEdge) + 2U * sizeof(PropertyCollectionRootEntry) + 2U * sizeof(std::string_view));
	EXPECT_THROW(static_cast<void>(logical.scanAdjacentEdges(snapshot, n1, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, &session, nullptr, [&](size_t bytes) { if (bytes > retainedFrames + sizeof(PageBackedGraphEdge) + 2U * sizeof(PropertyCollectionRootEntry)) throw std::length_error("dedup peak rejected"); })), std::length_error);
	EXPECT_THROW(static_cast<void>(logical.scanAdjacentEdges(snapshot, n1, LogicalGraphReadDirection::LGRD_OUTGOING,
		{}, 1U, &session, nullptr, [](size_t) { throw std::length_error("logical frames rejected"); })), std::length_error);
	const SelectedPropertyReadAdmission rejectAdjacency = [](size_t) { throw std::length_error("adjacency rejected"); };
	EXPECT_THROW(static_cast<void>(logical.scanAdjacentEdges(snapshot, n1, LogicalGraphReadDirection::LGRD_OUTGOING,
		{}, 1U, &session, &rejectAdjacency)), std::length_error);
	for (int retry = 0; retry < 2; ++retry) {
		size_t admitted = 0U;
		const SelectedPropertyReadAdmission acceptAdjacency = [&](size_t bytes) { admitted += bytes; };
		const auto page = logical.scanAdjacentEdges(snapshot, n1, LogicalGraphReadDirection::LGRD_OUTGOING,
			{}, 1U, &session, &acceptAdjacency);
		ASSERT_EQ(page.edges.size(), 1U);
		EXPECT_EQ(page.edges.front().edge_id, e1);
		EXPECT_EQ(page.edges.front().properties.front().value, payload);
		EXPECT_EQ(admitted, 536U);
		EXPECT_FALSE(page.continuation.has_value());
	}

}
