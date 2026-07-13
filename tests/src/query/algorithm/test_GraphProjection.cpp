/**
 * @file test_GraphProjection.cpp
 * @author Nexepic
 * @date 2026/4/9
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <utility>
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/GraphProjectionManager.hpp"
#include "graph/query/algorithm/CsrProjection.hpp"
#include "graph/query/execution/operators/GdsOperators.hpp"

namespace fs = std::filesystem;

class GraphProjectionTest : public ::testing::Test {
protected:
	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_projection_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (database) database->close();
		database.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	void flushAndReopen() {
		database->getStorage()->flush();
		database->close();
		database->open();
		dataManager = database->getStorage()->getDataManager();
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
};

// ============================================================================
// GraphProjection::build Tests
// ============================================================================

TEST_F(GraphProjectionTest, BuildEmptyGraph) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 0u);
	EXPECT_EQ(proj.edgeCount(), 0u);
	EXPECT_FALSE(proj.isWeighted());
}

TEST_F(GraphProjectionTest, BuildSingleNode) {
	graph::Node n(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 1u);
	EXPECT_EQ(proj.edgeCount(), 0u);
	EXPECT_TRUE(proj.getNodeIds().contains(n.getId()));
}

TEST_F(GraphProjectionTest, BuildWithEdges) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	dataManager->addEdge(e);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_EQ(proj.edgeCount(), 1u);

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].targetId, n2.getId());
	EXPECT_DOUBLE_EQ(out[0].weight, 1.0);

	const auto &in = proj.getInNeighbors(n2.getId());
	ASSERT_EQ(in.size(), 1u);
	EXPECT_EQ(in[0].targetId, n1.getId());
}

TEST_F(GraphProjectionTest, BuildWithNodeLabelFilter) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n3(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	graph::Edge e1(0, n1.getId(), n3.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	graph::Edge e2(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("LIVES_IN"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	flushAndReopen();

	// Filter: only Person nodes
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Person");
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_FALSE(proj.getNodeIds().contains(n2.getId()));

	// Edge to n2 (City) should be excluded since n2 is not in projection
	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].targetId, n3.getId());
}

TEST_F(GraphProjectionTest, BuildWithEdgeLabelFilter) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e1(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	graph::Edge e2(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("WORKS_WITH"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "KNOWS");
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_EQ(proj.edgeCount(), 1u);
}

TEST_F(GraphProjectionTest, BuildWithWeightProperty) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {{"distance", graph::PropertyValue(42.5)}});
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "distance");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 42.5);
}

TEST_F(GraphProjectionTest, BuildWithMissingWeightPropertyFallsBackToOne) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	// No 'distance' property set
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "distance");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 1.0);
}

TEST_F(GraphProjectionTest, GetNeighborsNonExistentNode) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	const auto &out = proj.getOutNeighbors(999);
	EXPECT_TRUE(out.empty());
	const auto &in = proj.getInNeighbors(999);
	EXPECT_TRUE(in.empty());
}

TEST_F(GraphProjectionTest, BuildWithDeletedNodes) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n3(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	// Delete n2 to create a gap (covers !node.isActive() branch)
	dataManager->deleteNode(n2);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_TRUE(proj.getNodeIds().contains(n1.getId()));
	EXPECT_FALSE(proj.getNodeIds().contains(n2.getId()));
	EXPECT_TRUE(proj.getNodeIds().contains(n3.getId()));
}

TEST_F(GraphProjectionTest, BuildWithIntegerWeightProperty) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	// Use INTEGER weight instead of DOUBLE (covers INTEGER branch in weight resolution)
	dataManager->addEdgeProperties(e.getId(), {{"hops", graph::PropertyValue(static_cast<int64_t>(7))}});
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "hops");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 7.0);
}

TEST_F(GraphProjectionTest, BuildWithProjectionSpecSupportsMultiLabelsTypesAndOrientation) {
	graph::Node fn(0, dataManager->getOrCreateTokenId("Function"));
	graph::Node method(0, dataManager->getOrCreateTokenId("Method"));
	graph::Node file(0, dataManager->getOrCreateTokenId("File"));
	graph::Node other(0, dataManager->getOrCreateTokenId("External"));
	dataManager->addNode(fn);
	dataManager->addNode(method);
	dataManager->addNode(file);
	dataManager->addNode(other);

	graph::Edge calls(0, fn.getId(), method.getId(), dataManager->getOrCreateTokenId("CALLS"));
	graph::Edge imports(0, method.getId(), file.getId(), dataManager->getOrCreateTokenId("IMPORTS"));
	graph::Edge skipped(0, other.getId(), fn.getId(), dataManager->getOrCreateTokenId("CALLS"));
	dataManager->addEdge(calls);
	dataManager->addEdge(imports);
	dataManager->addEdge(skipped);
	dataManager->addEdgeProperties(imports.getId(), {{"score", graph::PropertyValue(2.5)}});
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Function", "Method", "File"};
	graph::query::algorithm::RelationshipProjectionSpec callsSpec;
	callsSpec.type = "CALLS";
	callsSpec.weight.kind = graph::query::algorithm::ProjectionWeightKind::GPWK_CONSTANT;
	callsSpec.weight.constantWeight = 8.0;
	graph::query::algorithm::RelationshipProjectionSpec importsSpec;
	importsSpec.type = "IMPORTS";
	importsSpec.orientation = graph::query::algorithm::ProjectionOrientation::GPO_REVERSE;
	importsSpec.weight.kind = graph::query::algorithm::ProjectionWeightKind::GPWK_PROPERTY;
	importsSpec.weight.propertyName = "score";
	importsSpec.weight.defaultWeight = 1.0;
	spec.relationships = {callsSpec, importsSpec};

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, spec);
	EXPECT_EQ(proj.nodeCount(), 3u);
	EXPECT_EQ(proj.edgeCount(), 2u);
	EXPECT_TRUE(proj.isWeighted());
	EXPECT_FALSE(proj.getNodeIds().contains(other.getId()));

	const auto &fnOut = proj.getOutNeighbors(fn.getId());
	ASSERT_EQ(fnOut.size(), 1u);
	EXPECT_EQ(fnOut[0].targetId, method.getId());
	EXPECT_DOUBLE_EQ(fnOut[0].weight, 8.0);

	const auto &fileOut = proj.getOutNeighbors(file.getId());
	ASSERT_EQ(fileOut.size(), 1u);
	EXPECT_EQ(fileOut[0].targetId, method.getId());
	EXPECT_DOUBLE_EQ(fileOut[0].weight, 2.5);
}

TEST_F(GraphProjectionTest, UndirectedProjectionDoesNotDoubleCountSyntheticReverseInCsr) {
	graph::Node a(0, dataManager->getOrCreateTokenId("Thing"));
	graph::Node b(0, dataManager->getOrCreateTokenId("Thing"));
	dataManager->addNode(a);
	dataManager->addNode(b);
	graph::Edge e(0, a.getId(), b.getId(), dataManager->getOrCreateTokenId("LINKS"));
	dataManager->addEdge(e);
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Thing"};
	graph::query::algorithm::RelationshipProjectionSpec links;
	links.type = "LINKS";
	links.orientation = graph::query::algorithm::ProjectionOrientation::GPO_UNDIRECTED;
	spec.relationships = {links};

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, spec);
	EXPECT_EQ(proj.edgeCount(), 2u);
	EXPECT_EQ(proj.getOutNeighbors(a.getId()).size(), 1u);
	EXPECT_EQ(proj.getOutNeighbors(b.getId()).size(), 1u);

	auto csr = graph::query::algorithm::CsrProjection::build(proj);
	EXPECT_EQ(csr->edgeCount(), 2u);
	EXPECT_FALSE(csr->isWeighted());
	EXPECT_NE(csr->indexOf(a.getId()), std::numeric_limits<size_t>::max());
	EXPECT_EQ(csr->indexOf(999999), std::numeric_limits<size_t>::max());
}

TEST_F(GraphProjectionTest, BuildWithEmptyRelationshipSpecsUsesDefaultOrientation) {
	graph::Node a(0, dataManager->getOrCreateTokenId("Thing"));
	graph::Node b(0, dataManager->getOrCreateTokenId("Thing"));
	dataManager->addNode(a);
	dataManager->addNode(b);
	graph::Edge e(0, a.getId(), b.getId(), dataManager->getOrCreateTokenId("LINKS"));
	dataManager->addEdge(e);
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Thing"};
	spec.defaultOrientation = graph::query::algorithm::ProjectionOrientation::GPO_REVERSE;

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, spec);
	EXPECT_EQ(proj.edgeCount(), 1u);
	EXPECT_TRUE(proj.getOutNeighbors(a.getId()).empty());
	ASSERT_EQ(proj.getOutNeighbors(b.getId()).size(), 1u);
	EXPECT_EQ(proj.getOutNeighbors(b.getId())[0].targetId, a.getId());
}

TEST_F(GraphProjectionTest, UndirectedSelfLoopAddsSingleProjectionArc) {
	graph::Node a(0, dataManager->getOrCreateTokenId("Thing"));
	dataManager->addNode(a);
	graph::Edge e(0, a.getId(), a.getId(), dataManager->getOrCreateTokenId("LOOPS"));
	dataManager->addEdge(e);
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Thing"};
	graph::query::algorithm::RelationshipProjectionSpec loop;
	loop.type = "LOOPS";
	loop.orientation = graph::query::algorithm::ProjectionOrientation::GPO_UNDIRECTED;
	spec.relationships = {loop};

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, spec);
	EXPECT_EQ(proj.edgeCount(), 1u);
	ASSERT_EQ(proj.getOutNeighbors(a.getId()).size(), 1u);
	EXPECT_FALSE(proj.getOutNeighbors(a.getId())[0].syntheticReverse);
}

TEST_F(GraphProjectionTest, ProjectionSpecValidatesDirectWeightConfiguration) {
	graph::Node a(0, dataManager->getOrCreateTokenId("Thing"));
	graph::Node b(0, dataManager->getOrCreateTokenId("Thing"));
	dataManager->addNode(a);
	dataManager->addNode(b);
	graph::Edge e(0, a.getId(), b.getId(), dataManager->getOrCreateTokenId("LINKS"));
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {{"name", graph::PropertyValue("not_numeric")}});
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Thing"};
	graph::query::algorithm::RelationshipProjectionSpec links;
	links.type = "LINKS";
	links.weight.kind = graph::query::algorithm::ProjectionWeightKind::GPWK_CONSTANT;
	links.weight.constantWeight = -1.0;
	spec.relationships = {links};
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);

	spec.relationships[0].weight.constantWeight = std::numeric_limits<double>::quiet_NaN();
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);

	spec.relationships[0].weight.kind = graph::query::algorithm::ProjectionWeightKind::GPWK_PROPERTY;
	spec.relationships[0].weight.propertyName.clear();
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);

	spec.relationships[0].weight.propertyName = "name";
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);

	spec.relationships[0].weight.propertyName = "badWeight";
	dataManager->addEdgeProperties(e.getId(), {{"badWeight", graph::PropertyValue(-1.0)}});
	flushAndReopen();
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);

	spec.relationships[0].weight.propertyName = "nanWeight";
	dataManager->addEdgeProperties(e.getId(), {{"nanWeight", graph::PropertyValue(std::numeric_limits<double>::quiet_NaN())}});
	flushAndReopen();
	EXPECT_THROW(graph::query::algorithm::GraphProjection::build(dataManager, spec), std::runtime_error);
}

TEST_F(GraphProjectionTest, ProjectionSpecNullWeightPropertyUsesDefaultWeight) {
	graph::Node a(0, dataManager->getOrCreateTokenId("Thing"));
	graph::Node b(0, dataManager->getOrCreateTokenId("Thing"));
	dataManager->addNode(a);
	dataManager->addNode(b);
	graph::Edge e(0, a.getId(), b.getId(), dataManager->getOrCreateTokenId("LINKS"));
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {{"nullable", graph::PropertyValue()}});
	flushAndReopen();

	graph::query::algorithm::ProjectionSpec spec;
	spec.nodeLabels = {"Thing"};
	graph::query::algorithm::RelationshipProjectionSpec links;
	links.type = "LINKS";
	links.weight.kind = graph::query::algorithm::ProjectionWeightKind::GPWK_PROPERTY;
	links.weight.propertyName = "nullable";
	links.weight.defaultWeight = 2.0;
	spec.relationships = {links};

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, spec);
	ASSERT_EQ(proj.getOutNeighbors(a.getId()).size(), 1u);
	EXPECT_DOUBLE_EQ(proj.getOutNeighbors(a.getId())[0].weight, 2.0);
}

// ============================================================================
// GraphProjectionManager Tests
// ============================================================================

TEST(GraphProjectionManagerTest, CreateAndGetProjection) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_TRUE(pm.exists("test"));
	auto retrieved = pm.getProjection("test");
	EXPECT_EQ(retrieved.get(), proj.get());
}

TEST(GraphProjectionManagerTest, DuplicateNameThrows) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_THROW(pm.createProjection("test", proj), std::runtime_error);
}

TEST(GraphProjectionManagerTest, CreateRejectsInvalidInputs) {
	graph::query::algorithm::GraphProjectionManager pm;
	graph::query::algorithm::ProjectionSpec emptyName;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();

	EXPECT_THROW(pm.createProjection(emptyName, proj, 0, std::chrono::nanoseconds{0}), std::runtime_error);

	graph::query::algorithm::ProjectionSpec named;
	named.name = "null_projection";
	EXPECT_THROW(pm.createProjection(named, nullptr, 0, std::chrono::nanoseconds{0}), std::runtime_error);
}

TEST(GraphProjectionManagerTest, GetNonExistentThrows) {
	graph::query::algorithm::GraphProjectionManager pm;
	EXPECT_THROW(pm.getProjection("missing"), std::runtime_error);
}

TEST(GraphProjectionManagerTest, DropProjection) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_TRUE(pm.dropProjection("test"));
	EXPECT_FALSE(pm.exists("test"));
	EXPECT_FALSE(pm.dropProjection("test")); // Already dropped
}

TEST(GraphProjectionManagerTest, DescribesListsAndMarksStaleByRevision) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	graph::query::algorithm::ProjectionSpec spec;
	spec.name = "catalog";
	spec.nodeLabels = {"Person"};
	graph::query::algorithm::RelationshipProjectionSpec relationship;
	relationship.type = "KNOWS";
	relationship.orientation = graph::query::algorithm::ProjectionOrientation::GPO_UNDIRECTED;
	spec.relationships = {relationship};

	pm.createProjection(spec, proj, 7, std::chrono::milliseconds(3));
	auto descriptor = pm.describe("catalog", 7);
	ASSERT_TRUE(descriptor.has_value());
	EXPECT_EQ(descriptor->name, "catalog");
	EXPECT_EQ(descriptor->sourceRevision, 7u);
	EXPECT_EQ(descriptor->currentRevision, 7u);
	EXPECT_FALSE(descriptor->stale);
	EXPECT_EQ(descriptor->buildMillis, 3);
	EXPECT_GT(descriptor->memoryBytes, 0u);
	ASSERT_EQ(descriptor->spec.relationships.size(), 1u);
	EXPECT_EQ(descriptor->spec.relationships[0].type, "KNOWS");

	auto stale = pm.describe("catalog", 8);
	ASSERT_TRUE(stale.has_value());
	EXPECT_TRUE(stale->stale);
	EXPECT_EQ(stale->currentRevision, 8u);

	auto list = pm.list(8);
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(list[0].name, "catalog");
	EXPECT_TRUE(list[0].stale);
}

TEST(GraphProjectionManagerTest, ListReturnsDescriptorsSortedByName) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto first = std::make_shared<graph::query::algorithm::GraphProjection>();
	auto second = std::make_shared<graph::query::algorithm::GraphProjection>();

	graph::query::algorithm::ProjectionSpec zSpec;
	zSpec.name = "zeta";
	graph::query::algorithm::ProjectionSpec aSpec;
	aSpec.name = "alpha";
	pm.createProjection(std::move(zSpec), first, 1, std::chrono::nanoseconds{0});
	pm.createProjection(std::move(aSpec), second, 1, std::chrono::nanoseconds{0});

	auto list = pm.list(1);
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].name, "alpha");
	EXPECT_EQ(list[1].name, "zeta");
}

TEST(GraphProjectionManagerTest, DropAllClearsProjectionAndCsrDescriptors) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);
	auto csr = pm.getOrBuildCsr("test");
	ASSERT_NE(csr, nullptr);
	EXPECT_EQ(pm.getOrBuildCsr("test").get(), csr.get());

	auto descriptor = pm.describe("test", 0);
	ASSERT_TRUE(descriptor.has_value());
	EXPECT_TRUE(descriptor->hasCsr);
	EXPECT_GT(descriptor->csrMemoryBytes, 0u);

	EXPECT_EQ(pm.dropAll(), 1u);
	EXPECT_FALSE(pm.exists("test"));
	EXPECT_FALSE(pm.describe("test", 0).has_value());
	EXPECT_EQ(pm.dropAll(), 0u);
}

TEST(GraphProjectionManagerTest, CsrBuildForMissingProjectionThrows) {
	graph::query::algorithm::GraphProjectionManager pm;
	EXPECT_THROW(pm.getOrBuildCsr("missing"), std::runtime_error);
}

TEST(GdsGraphCatalogDetailTest, SaturatesLargeCatalogIntegers) {
	namespace detail = graph::query::execution::operators::detail;
	constexpr auto int64Max = (std::numeric_limits<int64_t>::max)();
	constexpr auto sizeMax = (std::numeric_limits<size_t>::max)();
	constexpr auto expectedSizeMax = sizeMax > static_cast<size_t>(int64Max)
		? int64Max
		: static_cast<int64_t>(sizeMax);
	EXPECT_EQ(detail::toCatalogInteger((std::numeric_limits<size_t>::max)()),
			  expectedSizeMax);
	EXPECT_EQ(detail::toCatalogInteger((std::numeric_limits<uint64_t>::max)()),
			  int64Max);
	EXPECT_EQ(detail::toCatalogInteger(static_cast<int64_t>(-7)), -7);
}

TEST(GdsGraphCatalogDetailTest, SerializesDefaultReverseAndPropertyWeightSchema) {
	namespace algorithm = graph::query::algorithm;
	namespace detail = graph::query::execution::operators::detail;

	algorithm::GraphProjectionDescriptor descriptor;
	descriptor.name = "default_schema";
	descriptor.spec.name = descriptor.name;
	descriptor.spec.nodeLabels = {"Person"};
	descriptor.spec.defaultOrientation = algorithm::ProjectionOrientation::GPO_REVERSE;
	descriptor.sourceRevision = 1;
	descriptor.currentRevision = 2;
	descriptor.stale = true;

	auto record = detail::descriptorRecord(descriptor);
	const auto relationships = record.getValue("relationships")->getList();
	ASSERT_EQ(relationships.size(), 1u);
	const auto &defaultRelationship = relationships[0].getMap();
	EXPECT_EQ(defaultRelationship.at("orientation").toString(), "REVERSE");
	EXPECT_EQ(defaultRelationship.at("weightKind").toString(), "NONE");
	EXPECT_TRUE(std::get<bool>(record.getValue("stale")->getVariant()));

	algorithm::RelationshipProjectionSpec propertyWeighted;
	propertyWeighted.type = "CALLS";
	propertyWeighted.weight.kind = algorithm::ProjectionWeightKind::GPWK_PROPERTY;
	propertyWeighted.weight.propertyName = "score";
	propertyWeighted.weight.defaultWeight = 0.5;
	descriptor.spec.relationships = {propertyWeighted};

	auto schema = detail::schemaRecord(descriptor);
	const auto propertyRelationships = schema.getValue("relationships")->getList();
	ASSERT_EQ(propertyRelationships.size(), 1u);
	const auto &propertyRelationship = propertyRelationships[0].getMap();
	EXPECT_EQ(propertyRelationship.at("weightKind").toString(), "PROPERTY");
	EXPECT_EQ(propertyRelationship.at("weightProperty").toString(), "score");
	EXPECT_DOUBLE_EQ(std::get<double>(propertyRelationship.at("defaultWeight").getVariant()), 0.5);
}
