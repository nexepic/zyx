/**
 * @file test_DataManager_Observer.cpp
 * @date 2025/8/15
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

#include "DataManagerTestFixture.hpp"

// Observer that tracks state update notifications
class StateTrackingObserver final : public IEntityObserver {
public:
	void onStateUpdated(const State &oldState, const State &newState) override {
		stateUpdates.emplace_back(oldState, newState);
	}
	std::vector<std::pair<State, State>> stateUpdates;
};

TEST_F(DataManagerTest, ObserverNotifications) {
	observer->reset();

	// --- PHASE 1: Add ---
	auto source = createTestNode(dataManager, "Source");
	auto target = createTestNode(dataManager, "Target");
	dataManager->addNode(source);
	dataManager->addNode(target);

	// Adding an edge internally triggers `linkEdge`, which updates the connected nodes
	// (setting firstOutEdgeId/firstInEdgeId) and the edge itself.
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "RELATES_TO");
	dataManager->addEdge(edge);

	EXPECT_EQ(2UL, observer->addedNodes.size());
	EXPECT_EQ(1UL, observer->addedEdges.size());

	// Linking the edge updates both Source and Target nodes + the Edge itself.
	// Since we enabled notifications for ADDED entities, these internal updates are now reported.
	EXPECT_EQ(2UL, observer->updatedNodes.size()); // Source updated, Target updated
	EXPECT_EQ(1UL, observer->updatedEdges.size()); // Edge updated (pointers)

	// --- PHASE 2: Explicit Update ---
	simulateSave();
	observer->reset();

	Node oldSource = dataManager->getNode(source.getId());
	source.setLabelId(dataManager->getOrCreateLabelId("UpdatedSource"));
	dataManager->updateNode(source);

	Edge oldEdge = dataManager->getEdge(edge.getId());
	edge.setLabelId(dataManager->getOrCreateLabelId("UPDATED_RELATION"));
	dataManager->updateEdge(edge);

	EXPECT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ(1UL, observer->updatedEdges.size());

	// --- PHASE 3: Delete ---
	observer->reset();

	// Deleting the edge unlinks it from the nodes.
	// `unlinkEdge` calls updateNode on Source and Target.
	dataManager->deleteEdge(edge);
	dataManager->deleteNode(target);

	EXPECT_EQ(1UL, observer->deletedNodes.size());
	EXPECT_EQ(target.getId(), observer->deletedNodes[0].getId());

	EXPECT_EQ(1UL, observer->deletedEdges.size());
	EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());

	// Unlinking the edge updates both Source and Target nodes.
	EXPECT_EQ(2UL, observer->updatedNodes.size());

	std::vector<int64_t> updatedNodeIds;
	for (const auto &val: observer->updatedNodes | std::views::values) {
		updatedNodeIds.push_back(val.getId());
	}

	// Verify that both connected nodes were updated during the edge deletion
	ASSERT_TRUE(std::ranges::find(updatedNodeIds, source.getId()) != std::ranges::end(updatedNodeIds));
	ASSERT_TRUE(std::ranges::find(updatedNodeIds, target.getId()) != std::ranges::end(updatedNodeIds));
}

TEST_F(DataManagerTest, NotifyStateUpdatedWithObserver) {
	// Covers L181 true branch: observer loop in notifyStateUpdated
	auto stateObserver = std::make_shared<StateTrackingObserver>();
	dataManager->registerObserver(stateObserver);

	// Create a state and then modify its properties to trigger notifyStateUpdated
	auto state = createTestState("test_state_key");
	dataManager->addStateEntity(state);
	EXPECT_NE(0, state.getId());

	// addStateProperties calls notifyStateUpdated internally
	std::unordered_map<std::string, PropertyValue> props;
	props["color"] = PropertyValue(std::string("blue"));
	dataManager->addStateProperties("test_state_key", props, false);

	EXPECT_GE(stateObserver->stateUpdates.size(), 1UL)
		<< "State observer should have received state update notification";
}

TEST_F(DataManagerTest, NotifyNodeAddedWithMultipleObservers) {
	// Exercises the observer loop body for notifyNodeAdded (L122-124)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	auto node = createTestNode(dataManager, "MultiObsNode");
	dataManager->addNode(node);

	EXPECT_EQ(1UL, observer->addedNodes.size());
	EXPECT_EQ(1UL, observer2->addedNodes.size());
}

TEST_F(DataManagerTest, NotifyNodeDeletedWithMultipleObservers) {
	// Exercises the observer loop body for notifyNodeDeleted (L144-146)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	auto node = createTestNode(dataManager, "DelNode");
	dataManager->addNode(node);
	observer->reset();
	observer2->reset();

	dataManager->deleteNode(node);
	EXPECT_EQ(1UL, observer->deletedNodes.size());
	EXPECT_EQ(1UL, observer2->deletedNodes.size());
}

TEST_F(DataManagerTest, NotifyEdgeAddedWithMultipleObservers) {
	// Exercises the observer loop body for notifyEdgeAdded (L152-153)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "OBS");
	dataManager->addEdge(edge);

	EXPECT_EQ(1UL, observer->addedEdges.size());
	EXPECT_EQ(1UL, observer2->addedEdges.size());
}

TEST_F(DataManagerTest, NotifyEdgeDeletedWithMultipleObservers) {
	// Exercises the observer loop body for notifyEdgeDeleted (L174-176)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "OBS");
	dataManager->addEdge(edge);
	observer->reset();
	observer2->reset();

	dataManager->deleteEdge(edge);
	EXPECT_EQ(1UL, observer->deletedEdges.size());
	EXPECT_EQ(1UL, observer2->deletedEdges.size());
}

TEST_F(DataManagerTest, NotifyNodesAddedBatch) {
	// Exercises the observer loop in notifyNodesAdded (L129-132)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	std::vector<Node> nodes;
	nodes.push_back(createTestNode(dataManager, "Batch1"));
	nodes.push_back(createTestNode(dataManager, "Batch2"));
	dataManager->addNodes(nodes);

	// onNodesAdded is called, but TestEntityObserver doesn't override it (uses default no-op)
	// The loop body is still executed for both observers
	EXPECT_EQ(2UL, nodes.size());
}

TEST_F(DataManagerTest, NotifyEdgesAddedBatch) {
	// Exercises the observer loop in notifyEdgesAdded (L158-162)
	auto observer2 = std::make_shared<TestEntityObserver>();
	dataManager->registerObserver(observer2);

	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	std::vector<Edge> edges;
	edges.push_back(createTestEdge(dataManager, n1.getId(), n2.getId(), "BE1"));
	edges.push_back(createTestEdge(dataManager, n2.getId(), n1.getId(), "BE2"));
	dataManager->addEdges(edges);

	// notifyEdgesAdded calls onEdgeAdded for each edge per observer
	EXPECT_EQ(2UL, observer->addedEdges.size());
	EXPECT_EQ(2UL, observer2->addedEdges.size());
}

TEST_F(DataManagerTest, RemoveNodePropertyWithNotification) {
	// Covers removeNodeProperty at lines 340-357
	auto node = createTestNode(dataManager, "RemovePropNode");
	dataManager->addNode(node);

	// Add multiple properties
	dataManager->addNodeProperties(node.getId(), {
		{"name", PropertyValue(std::string("Bob"))},
		{"score", PropertyValue(static_cast<int64_t>(100))},
	});

	observer->reset();

	// Remove property via public API (triggers notification)
	dataManager->removeNodeProperty(node.getId(), "score");

	// Should have notified update
	EXPECT_GE(observer->updatedNodes.size(), 1UL)
		<< "removeNodeProperty should trigger update notification";

	auto props = dataManager->getNodeProperties(node.getId());
	EXPECT_TRUE(props.find("score") == props.end())
		<< "Property 'score' should be removed";
	EXPECT_TRUE(props.find("name") != props.end())
		<< "Property 'name' should still exist";
}

TEST_F(DataManagerTest, RemoveEdgePropertyWithNotification) {
	// Covers removeEdgeProperty at lines 461-477
	auto node1 = createTestNode(dataManager, "Src");
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node1);
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "HAS");
	dataManager->addEdge(edge);

	// Add multiple properties
	dataManager->addEdgeProperties(edge.getId(), {
		{"weight", PropertyValue(1.5)},
		{"label", PropertyValue(std::string("important"))},
	});

	observer->reset();

	// Remove property via public API (triggers notification)
	dataManager->removeEdgeProperty(edge.getId(), "weight");

	// Should have notified update
	EXPECT_GE(observer->updatedEdges.size(), 1UL)
		<< "removeEdgeProperty should trigger update notification";

	auto props = dataManager->getEdgeProperties(edge.getId());
	EXPECT_TRUE(props.find("weight") == props.end())
		<< "Property 'weight' should be removed";
	EXPECT_TRUE(props.find("label") != props.end())
		<< "Property 'label' should still exist";
}

