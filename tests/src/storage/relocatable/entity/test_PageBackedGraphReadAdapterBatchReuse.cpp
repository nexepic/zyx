#include "storage/relocatable/entity/PageBackedGraphReadAdapterTestFixture.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/debug/PerfTrace.hpp"
using namespace graph::storage;
using graph::storage::test::PageBackedGraphReadAdapterTest;
namespace {
Superblock initialSuperblock(){Superblock s;s.database_id=DatabaseId(0x1020304050607080ULL);s.generation=Generation(1);s.checkpoint_lsn=LogSequenceNumber(1);return s;}
NativeGraphWritePlannerOptions graphWriteOptions(){NativeGraphWritePlannerOptions o;o.first_slot=PhysicalSlotId(46);o.last_slot=PhysicalSlotId(70);o.entity_pool_owner_id=907;o.property_roots={PhysicalSlotId(50),PhysicalSlotId(70),903};return o;}
}
TEST_F(PageBackedGraphReadAdapterTest, BatchedTopologyRejectsLaterInconsistencyBeforeAnyEarlyStopCallback) {
    auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
    EntityPageStore entities(io, *metadata);
    SlottedPagePool pages(io, *metadata);
    EntityLabelSetStore labels(io, *metadata, pages);
    PropertyCollectionRootStore properties(pages, *metadata);
    BTreePageStore trees(pages, *metadata);
    NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
    auto transaction = planner.beginTransaction();
    const auto source = transaction.createNode({});
    const auto target = transaction.createNode({});
    const auto other = transaction.createNode({});
    const auto first = transaction.createEdge(source, target, 41);
    const auto second = transaction.createEdge(source, other, 43);
    ASSERT_TRUE(transaction.commit().committed());
    auto old = metadata->openSnapshot();
    auto wrong = entities.readEdge(old, second);
    // Publish valid main-record bytes through the lower storage seam without
    // its matching derived adjacency update, to model cross-structure damage.
    wrong.source_node_id = other;
    static_cast<void>(entities.updateEdge(old, second, wrong,
        LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)));
    const auto current = metadata->openSnapshot();
    PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
    const std::array sources{source};
    for (const bool stop : {false, true}) {
        size_t callbacks = 0;
        EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(current, sources,
            PageBackedGraphReadDirection::PBGRD_OUTGOING, 3,
            [&](const EntityRef &) { ++callbacks; return !stop; })), FormatException);
        EXPECT_EQ(callbacks, 0U);
    }
    std::vector<uint64_t> retained;
    EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(old, sources,
        PageBackedGraphReadDirection::PBGRD_OUTGOING, 3,
        [&](const EntityRef &edge) { retained.push_back(edge.entity_id); return true; }));
    EXPECT_EQ(retained, (std::vector<uint64_t>{first, second}));
}
TEST_F(PageBackedGraphReadAdapterTest, VisitsSeveralNativeAdjacencyRangesInOneBoundedBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t first = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t second = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t third = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = transaction.createEdge(first, second, 41U);
	const uint64_t secondEdge = transaction.createEdge(second, third, 43U);
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	NativeReadSession session;

	const std::array<uint64_t, 2U> outgoingSources{first, second};
	std::vector<EntityRef> outgoing;
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[&outgoing](const EntityRef &reference) {
				outgoing.push_back(reference);
				return true;
			}, &session));
	EXPECT_EQ(outgoing, (std::vector<EntityRef>{{firstEdge, true, first, second, 41U},
			{secondEdge, true, second, third, 43U}}));
	ASSERT_TRUE(session.edge(firstEdge).has_value());
	ASSERT_TRUE(session.edge(secondEdge).has_value());

	// Repeated batched traversal must reuse verified masters, while preserving
	// original adjacency order. A fresh session still gathers both masters.
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	(void) graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(snapshot, outgoingSources,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
		[](const EntityRef &) { return true; }, &session));
	const auto cachedTrace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(cachedTrace.contains("native_graph.read.edge_sorted_batch_calls"));
	NativeReadSession freshSession;
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(snapshot, outgoingSources,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
		[](const EntityRef &) { return true; }, &freshSession));
	const auto coldTrace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_EQ(coldTrace.at("native_graph.read.edge_sorted_batch_entries").totalValue, 2);
	graph::debug::PerfTrace::setValueCollectionEnabled(false);

	const std::array<uint64_t, 2U> incomingTargets{second, third};
	std::vector<EntityRef> incoming;
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, incomingTargets, PageBackedGraphReadDirection::PBGRD_INCOMING, 3U,
			[&incoming](const EntityRef &reference) {
				incoming.push_back(reference);
				return true;
			}, &session));
	EXPECT_EQ(incoming, outgoing);

	// A callback may fill the bounded FIFO. Captured verified records survive
	// that eviction and still publish the original second edge.
	std::vector<EntityRef> evicted;
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(snapshot,outgoingSources,
		PageBackedGraphReadDirection::PBGRD_OUTGOING,3U,[&](const EntityRef &ref){
			evicted.push_back(ref);
			for(uint64_t id=10000;id<10200;++id)session.putEdge(id,EdgeEntityRecord{});
			return true;
		},&session));
	EXPECT_EQ(evicted,outgoing);

	size_t stopped = 0U;
	EXPECT_FALSE(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[&stopped](const EntityRef &) {
				++stopped;
				return false;
			}, &session));
	EXPECT_EQ(stopped, 1U);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U,
			[](const EntityRef &) { return true; })), FormatException);
	const std::array<uint64_t, 2U> unsorted{second, first};
	const std::array<uint64_t, 2U> duplicate{first, first};
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, unsorted, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[](const EntityRef &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, duplicate, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[](const EntityRef &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, std::span<const uint64_t>{}, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[](const EntityRef &) { return true; })), FormatException);
	snapshot.release();
}
