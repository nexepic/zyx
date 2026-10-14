#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
using namespace relocatable_storage_backend_test;
namespace {
LogicalGraphMutationEntityReference graphNode(uint64_t id){return {LogicalGraphMutationEntityKind::LGMK_NODE,LogicalGraphMutationReferenceKind::LGRK_PERSISTED,id};}
std::vector<uint64_t> adjacentIds(const LogicalGraphReadSnapshot &snapshot,uint64_t node,LogicalGraphReadDirection direction){
	std::vector<uint64_t> result;const std::array<uint64_t,1> nodes{node};
	snapshot.visitAdjacentEdgeRefsForNodes(nodes,direction,16,[&](const EntityRef &ref){result.push_back(ref.entity_id);return true;});return result;
}
}
TEST_F(RelocatableStorageBackendTest, GraphBatchTraversalKeepsPinnedGenerationsAndAtomicOverlayMemberships) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);backend.open();
	const auto nodes=backend.createLogicalNodes({{{"Traversal"},{}},{{"Traversal"},{}}});
	const auto edges=backend.createLogicalEdges({{nodes[0],nodes[1],"STEP",{}},{nodes[0],nodes[1],"STEP",{}},{nodes[0],nodes[0],"STEP",{}}});
	auto pinned=backend.openLogicalGraphReadSnapshot();const auto outgoing=adjacentIds(*pinned,nodes[0],LogicalGraphReadDirection::LGRD_OUTGOING);
	EXPECT_EQ(outgoing,edges);EXPECT_EQ(adjacentIds(*pinned,nodes[1],LogicalGraphReadDirection::LGRD_INCOMING),(std::vector<uint64_t>{edges[0],edges[1]}));
	auto transaction=backend.beginLogicalGraphTransaction(false);LogicalGraphMutationBatch mutation;
	mutation.edge_deletes={{edges[0]}};mutation.edge_creates={{graphNode(nodes[1]),graphNode(nodes[0]),"BACK",{}}};
	LogicalGraphTransactionOverlay overlay(*transaction);
	const auto staged=overlay.executeLogicalGraphMutationBatch(mutation);
	auto stagedSnapshot=overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(adjacentIds(*stagedSnapshot,nodes[0],LogicalGraphReadDirection::LGRD_OUTGOING),(std::vector<uint64_t>{edges[1],edges[2]}));
	EXPECT_EQ(adjacentIds(*stagedSnapshot,nodes[1],LogicalGraphReadDirection::LGRD_OUTGOING),staged.created_edge_ids);
	EXPECT_EQ(adjacentIds(*pinned,nodes[0],LogicalGraphReadDirection::LGRD_OUTGOING),outgoing);
	stagedSnapshot.reset();(void)transaction->commit();
	// The old session already contains masters. Neither deletion nor the new
	// opposite-direction membership can leak through those cached records.
	EXPECT_EQ(adjacentIds(*pinned,nodes[0],LogicalGraphReadDirection::LGRD_OUTGOING),outgoing);
	auto current=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(adjacentIds(*current,nodes[0],LogicalGraphReadDirection::LGRD_OUTGOING),(std::vector<uint64_t>{edges[1],edges[2]}));
	EXPECT_EQ(adjacentIds(*current,nodes[1],LogicalGraphReadDirection::LGRD_OUTGOING),staged.created_edge_ids);
	EXPECT_EQ(adjacentIds(*current,nodes[0],LogicalGraphReadDirection::LGRD_INCOMING),(std::vector<uint64_t>{edges[2],staged.created_edge_ids[0]}));
	current.reset();pinned.reset();EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();
	current=backend.openLogicalGraphReadSnapshot();EXPECT_EQ(adjacentIds(*current,nodes[1],LogicalGraphReadDirection::LGRD_OUTGOING),staged.created_edge_ids);current.reset();backend.close();
}
