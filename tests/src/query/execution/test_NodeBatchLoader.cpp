#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/execution/NodeBatchLoader.hpp"
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
