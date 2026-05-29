#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeBatchLoader.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

class NodeBatchLoaderTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_node_batch_loader_" + boost::uuids::to_string(uuid) + ".dat");
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove(testFilePath, ec);
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
	}

	int64_t addLabeledNode(const std::vector<std::string> &labels,
	                       const std::unordered_map<std::string, PropertyValue> &props = {}) {
		Node node(0, dm->getOrCreateTokenId(labels.front()));
		for (size_t i = 1; i < labels.size(); ++i) {
			node.addLabelId(dm->getOrCreateTokenId(labels[i]));
		}
		dm->addNode(node);
		if (!props.empty()) {
			dm->addNodeProperties(node.getId(), props);
		}
		return node.getId();
	}

	int64_t addPerson(const std::unordered_map<std::string, PropertyValue> &props = {}) {
		return addLabeledNode({"Person"}, props);
	}
};

TEST_F(NodeBatchLoaderTest, IdOnlyModeDoesNotLoadPropertiesOrNodes) {
	const int64_t id = addPerson({{"age", PropertyValue(int64_t{42})}});
	NodeScanConfig config;
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.countOnly = true;

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.nodeIds[0], id);
	ASSERT_EQ(batch.selected.size(), 1U);
	EXPECT_EQ(batch.selected[0], 1);
	EXPECT_EQ(batch.selectedCount(), 1U);
	EXPECT_TRUE(batch.propertyColumns.empty());
	EXPECT_TRUE(batch.materializedNodes.empty());
}

TEST_F(NodeBatchLoaderTest, IdOnlyModeIgnoresMistakenRequiredProperties) {
	const int64_t id = addPerson({{"age", PropertyValue(int64_t{42})}});
	NodeScanConfig config;
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.nodeIds[0], id);
	ASSERT_EQ(batch.selected.size(), 1U);
	EXPECT_EQ(batch.selected[0], 1);
	EXPECT_EQ(batch.selectedCount(), 1U);
	EXPECT_TRUE(batch.propertyColumns.empty());
	EXPECT_TRUE(batch.materializedNodes.empty());
}

TEST_F(NodeBatchLoaderTest, SelectedPropertyModeLoadsOnlyRequestedColumns) {
	const int64_t id = addPerson({{"age", PropertyValue(int64_t{42})}, {"name", PropertyValue("Alice")}});
	NodeScanConfig config;
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	EXPECT_FALSE(batch.hasPropertyColumn("name"));
	ASSERT_EQ(batch.propertyColumns["age"].size(), 1U);
	ASSERT_TRUE(batch.propertyColumns["age"][0].has_value());
	EXPECT_EQ(batch.propertyColumns["age"][0].value(), PropertyValue(int64_t{42}));
	EXPECT_TRUE(batch.materializedNodes.empty());
}

TEST_F(NodeBatchLoaderTest, MissingSelectedPropertyIsNullopt) {
	const int64_t id = addPerson({{"name", PropertyValue("Alice")}});
	NodeScanConfig config;
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	ASSERT_EQ(batch.propertyColumns["age"].size(), 1U);
	EXPECT_FALSE(batch.propertyColumns["age"][0].has_value());
}

TEST_F(NodeBatchLoaderTest, SelectedPropertyModeEmitsLoadPropertiesProfilePhase) {
	const int64_t first = addPerson({{"age", PropertyValue(int64_t{42})}});
	const int64_t second = addPerson({{"age", PropertyValue(int64_t{7})}});
	NodeScanConfig config;
	config.variable = "n";
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(std::vector<int64_t>{first, second}, 0, 2, config, requirements);

	EXPECT_EQ(batch.size(), 2U);
	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.load_properties"));
	EXPECT_EQ(snapshot.at("node_scan.load_properties").calls, 1U);
	EXPECT_TRUE(snapshot.contains("node_scan.load_property_entities"));
	EXPECT_TRUE(snapshot.contains("node_scan.extract_property_columns"));
}

TEST_F(NodeBatchLoaderTest, DenseCleanBatchUsesBulkNodeLoad) {
	static constexpr size_t kNodeCount = 4100;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson());
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_FULL_NODE;

	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selectedCount(), candidates.size());
	EXPECT_EQ(batch.materializedNodes.size(), candidates.size());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.bulk_load_nodes"));
	EXPECT_EQ(snapshot.at("node_scan.bulk_load_nodes").calls, 1U);
}

TEST_F(NodeBatchLoaderTest, DenseCleanBatchMarksDeletedBulkRowsUnselected) {
	static constexpr size_t kNodeCount = 4100;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson());
	}
	Node deleted = dm->getNode(candidates[7]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selected[7], 0);
	EXPECT_EQ(batch.selectedCount(), candidates.size() - 1);
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.bulk_load_nodes"));
}

TEST_F(NodeBatchLoaderTest, SelectedPropertyCleanBatchUsesMetadataLoad) {
	static constexpr size_t kNodeCount = 300;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson({{"age", PropertyValue(static_cast<int64_t>(i))}}));
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selectedCount(), candidates.size());
	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	ASSERT_EQ(batch.propertyColumns["age"].size(), candidates.size());
	EXPECT_EQ(batch.propertyColumns["age"][0], std::optional<PropertyValue>(PropertyValue(int64_t{0})));
	EXPECT_EQ(batch.propertyColumns["age"][kNodeCount - 1],
	          std::optional<PropertyValue>(PropertyValue(static_cast<int64_t>(kNodeCount - 1))));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.bulk_load_nodes"));
}

TEST_F(NodeBatchLoaderTest, MetadataLoadFiltersPersistedInactiveRows) {
	static constexpr size_t kNodeCount = 300;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson({{"age", PropertyValue(static_cast<int64_t>(i))}}));
	}
	db->getStorage()->flush();
	Node deleted = dm->getNode(candidates[1]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selected[0], 1);
	EXPECT_EQ(batch.selected[1], 0);
	EXPECT_EQ(batch.selectedCount(), candidates.size() - 1);
	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	EXPECT_FALSE(batch.propertyColumns["age"][1].has_value());
}

TEST_F(NodeBatchLoaderTest, MetadataLoadAppliesResidualLabelFilter) {
	static constexpr size_t kNodeCount = 300;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson({{"age", PropertyValue(static_cast<int64_t>(i))}}));
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"MissingLabel"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};

	NodeBatchLoader loader(dm);
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selectedCount(), 0U);
	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	for (const auto &value : batch.propertyColumns["age"]) {
		EXPECT_FALSE(value.has_value());
	}
}

TEST_F(NodeBatchLoaderTest, SelectedPropertyCountOnlyCleanBatchUsesMetadataLoad) {
	static constexpr size_t kNodeCount = 300;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson({{"age", PropertyValue(static_cast<int64_t>(i))}}));
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	requirements.countOnly = true;

	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	ASSERT_TRUE(batch.hasPropertyColumn("age"));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.bulk_load_nodes"));
}

TEST_F(NodeBatchLoaderTest, DirtyBatchFallsBackToPerNodeLoad) {
	static constexpr size_t kNodeCount = 4100;
	std::vector<int64_t> candidates;
	candidates.reserve(kNodeCount);
	for (size_t i = 0; i < kNodeCount; ++i) {
		candidates.push_back(addPerson());
	}
	db->getStorage()->flush();
	addPerson();

	NodeScanConfig config;
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selectedCount(), candidates.size());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("node_scan.bulk_load_nodes"));
	ASSERT_TRUE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_EQ(snapshot.at("node_scan.load_nodes").calls, candidates.size());
}

TEST_F(NodeBatchLoaderTest, InactiveNodeIsNotSelected) {
	const int64_t id = addPerson();
	Node node = dm->getNode(id);
	dm->deleteNode(node);

	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	ASSERT_EQ(batch.selected.size(), 1U);
	EXPECT_EQ(batch.selected[0], 0);
	EXPECT_EQ(batch.selectedCount(), 0U);
}

TEST_F(NodeBatchLoaderTest, ActiveCheckCanBeSkippedForExistingNode) {
	const int64_t id = addPerson();
	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.needsActiveCheck = false;

	NodeBatchLoader loader(dm);
	auto batch = loader.load({id}, 0, 1, config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.selected[0], 1);
	EXPECT_EQ(batch.selectedCount(), 1U);
}

TEST_F(NodeBatchLoaderTest, MissingResidualLabelRejectsCandidate) {
	const int64_t id = addPerson();
	NodeScanConfig config;
	config.labels = {"MissingLabel"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	auto batch = loader.load({id}, 0, 1, config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.selected[0], 0);
	EXPECT_EQ(batch.selectedCount(), 0U);
}

TEST_F(NodeBatchLoaderTest, DenseBatchWithInvalidCandidateFallsBackToPerNodeLoad) {
	static constexpr size_t kCandidateCount = 4096;
	std::vector<int64_t> candidates(kCandidateCount, 0);
	for (size_t i = 1; i < kCandidateCount; ++i) {
		candidates[i] = addPerson();
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.needsLabels = false;

	NodeBatchLoader loader(dm);
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selected[0], 0);
	EXPECT_EQ(batch.selectedCount(), candidates.size() - 1);
}

TEST_F(NodeBatchLoaderTest, DenseCleanBatchFallsBackWhenBulkRangeHasNoRows) {
	static constexpr size_t kCandidateCount = 4096;
	std::vector<int64_t> candidates;
	candidates.reserve(kCandidateCount);
	for (size_t i = 0; i < kCandidateCount; ++i) {
		candidates.push_back(50'000 + static_cast<int64_t>(i));
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	requirements.needsLabels = false;

	NodeBatchLoader loader(dm);
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.size(), candidates.size());
	EXPECT_EQ(batch.selectedCount(), 0U);
}

TEST_F(NodeBatchLoaderTest, MissingCandidateIdIsNotSelected) {
	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	auto batch = loader.load({999999}, 0, 1, config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.nodeIds[0], 999999);
	EXPECT_EQ(batch.selected[0], 0);
	EXPECT_EQ(batch.selectedCount(), 0U);
}

TEST_F(NodeBatchLoaderTest, FullNodeModeMaterializesProperties) {
	const int64_t id = addPerson({{"age", PropertyValue(int64_t{42})}});
	NodeScanConfig config;
	config.labels = {"Person"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_FULL_NODE;

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {id};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	ASSERT_EQ(batch.materializedNodes.size(), 1U);
	EXPECT_EQ(batch.materializedNodes[0].getId(), id);
	const auto &props = batch.materializedNodes[0].getProperties();
	ASSERT_TRUE(props.contains("age"));
	EXPECT_EQ(props.at("age"), PropertyValue(int64_t{42}));
}

TEST_F(NodeBatchLoaderTest, FullNodeModeDoesNotMaterializeMissingCandidate) {
	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_FULL_NODE;

	NodeBatchLoader loader(dm);
	auto batch = loader.load({999999}, 0, 1, config, requirements);

	ASSERT_EQ(batch.nodeIds.size(), 1U);
	EXPECT_EQ(batch.selected[0], 0);
	EXPECT_TRUE(batch.materializedNodes.empty());
}

TEST_F(NodeBatchLoaderTest, ResidualMultiLabelCheckUsesAndSemantics) {
	const int64_t matching = addLabeledNode({"Person", "Employee"});
	const int64_t mismatched = addLabeledNode({"Person"});
	NodeScanConfig config;
	config.labels = {"Person", "Employee"};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {matching, mismatched};
	auto batch = loader.load(candidates, 0, candidates.size(), config, requirements);

	EXPECT_EQ(batch.nodeIds, candidates);
	EXPECT_EQ(batch.selected, (std::vector<uint8_t>{1, 0}));
	EXPECT_EQ(batch.selectedCount(), 1U);
}

TEST_F(NodeBatchLoaderTest, EmptyOrClampedRangeReturnsExpectedRows) {
	const int64_t first = addPerson();
	const int64_t second = addPerson();
	NodeScanConfig config;
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	NodeBatchLoader loader(dm);
	const std::vector<int64_t> candidates = {first, second};

	auto empty = loader.load(candidates, 2, 2, config, requirements);
	EXPECT_TRUE(empty.nodeIds.empty());
	EXPECT_TRUE(empty.selected.empty());

	auto clamped = loader.load(candidates, 1, candidates.size() + 100, config, requirements);
	ASSERT_EQ(clamped.nodeIds.size(), 1U);
	EXPECT_EQ(clamped.nodeIds[0], second);
	EXPECT_EQ(clamped.selectedCount(), 1U);
}

TEST(NodeMetadataBatchTest, RoundTripsFixedNodeMetadataWithoutProperties) {
	Node node(123, 10);
	ASSERT_TRUE(node.addLabelId(20));
	node.setFirstOutEdgeId(7);
	node.setFirstInEdgeId(9);
	node.setPropertyEntityId(42, PropertyStorageType::PROPERTY_ENTITY);
	node.markInactive(false);

	NodeMetadataBatch batch;
	batch.appendDefault();
	batch.setFromNode(0, node);

	EXPECT_EQ(batch.size(), 1U);
	EXPECT_TRUE(batch.isValid(0));
	EXPECT_TRUE(batch.hasLabelId(0, 10));
	EXPECT_TRUE(batch.hasLabelId(0, 20));
	EXPECT_FALSE(batch.hasLabelId(0, 30));

	Node roundTripped = batch.toNode(0);
	EXPECT_EQ(roundTripped.getId(), 123);
	EXPECT_EQ(roundTripped.getFirstOutEdgeId(), 7);
	EXPECT_EQ(roundTripped.getFirstInEdgeId(), 9);
	EXPECT_EQ(roundTripped.getPropertyEntityId(), 42);
	EXPECT_EQ(roundTripped.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_FALSE(roundTripped.isActive());
	EXPECT_TRUE(roundTripped.hasLabelId(10));
	EXPECT_TRUE(roundTripped.hasLabelId(20));
	EXPECT_TRUE(roundTripped.getProperties().empty());
}
