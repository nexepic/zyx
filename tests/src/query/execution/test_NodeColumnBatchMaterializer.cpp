#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/NodeColumnBatchMaterializer.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

class NodeColumnBatchMaterializerTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_node_column_batch_materializer_" + boost::uuids::to_string(uuid) + ".dat");
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

	int64_t addPerson(const std::unordered_map<std::string, PropertyValue> &props = {}) {
		Node node(0, dm->getOrCreateTokenId("Person"));
		dm->addNode(node);
		if (!props.empty()) {
			dm->addNodeProperties(node.getId(), props);
		}
		return node.getId();
	}
};

TEST_F(NodeColumnBatchMaterializerTest, MaterializesOnlySelectedRows) {
	const int64_t first = addPerson();
	const int64_t second = addPerson();
	const int64_t third = addPerson();
	NodeColumnBatch batch;
	batch.nodeIds = {first, second, third};
	batch.selected = {1, 0, 1};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 2U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getId(), first);
	EXPECT_EQ(records[1].getNode("n")->getId(), third);
}

TEST_F(NodeColumnBatchMaterializerTest, ReusesSelectedPropertyColumns) {
	const int64_t first = addPerson({{"age", PropertyValue(int64_t{10})}, {"name", PropertyValue("disk-first")}});
	const int64_t second = addPerson({{"age", PropertyValue(int64_t{20})}, {"name", PropertyValue("disk-second")}});
	NodeColumnBatch batch;
	batch.nodeIds = {first, second};
	batch.selected = {1, 1};
	batch.propertyColumns["age"] = {PropertyValue(int64_t{41}), PropertyValue(int64_t{42})};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 2U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getProperty("age"), PropertyValue(int64_t{41}));
	EXPECT_EQ(records[1].getNode("n")->getProperty("age"), PropertyValue(int64_t{42}));
	EXPECT_FALSE(records[0].getNode("n")->hasProperty("name"));
	EXPECT_FALSE(records[1].getNode("n")->hasProperty("name"));
}

TEST_F(NodeColumnBatchMaterializerTest, FullNodeModeReusesMaterializedNodesWithProperties) {
	const int64_t first = addPerson({{"age", PropertyValue(int64_t{10})}});
	const int64_t second = addPerson({{"age", PropertyValue(int64_t{20})}});
	Node materializedFirst = dm->getNode(first);
	materializedFirst.setProperties({{"age", PropertyValue(int64_t{101})}, {"name", PropertyValue("selected-first")}});
	Node materializedSecond = dm->getNode(second);
	materializedSecond.setProperties({{"age", PropertyValue(int64_t{202})}, {"name", PropertyValue("selected-second")}});

	NodeColumnBatch batch;
	batch.nodeIds = {first, second};
	batch.selected = {1, 1};
	batch.materializedNodes = {materializedFirst, materializedSecond};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_FULL_NODE;
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 2U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getProperty("age"), PropertyValue(int64_t{101}));
	EXPECT_EQ(records[0].getNode("n")->getProperty("name"), PropertyValue("selected-first"));
	EXPECT_EQ(records[1].getNode("n")->getProperty("age"), PropertyValue(int64_t{202}));
	EXPECT_EQ(records[1].getNode("n")->getProperty("name"), PropertyValue("selected-second"));
}

TEST_F(NodeColumnBatchMaterializerTest, EmptySelectionVectorMeansAllRowsSelected) {
	const int64_t first = addPerson();
	const int64_t second = addPerson();
	NodeColumnBatch batch;
	batch.nodeIds = {first, second};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 2U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getId(), first);
	EXPECT_EQ(records[1].getNode("n")->getId(), second);
}

TEST_F(NodeColumnBatchMaterializerTest, FullNodeFallbackPreservesNodeIdAndPropertiesWhenMaterializedNodesExhausted) {
	const int64_t first = addPerson({{"age", PropertyValue(int64_t{10})}});
	const int64_t second = addPerson({{"age", PropertyValue(int64_t{20})}, {"name", PropertyValue("from-disk")}});
	Node materializedFirst = dm->getNode(first);
	materializedFirst.setProperties({{"age", PropertyValue(int64_t{101})}});

	NodeColumnBatch batch;
	batch.nodeIds = {first, second};
	batch.selected = {1, 1};
	batch.materializedNodes = {materializedFirst};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_FULL_NODE;
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 2U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getId(), first);
	EXPECT_EQ(records[0].getNode("n")->getProperty("age"), PropertyValue(int64_t{101}));
	EXPECT_EQ(records[1].getNode("n")->getId(), second);
	EXPECT_EQ(records[1].getNode("n")->getProperty("age"), PropertyValue(int64_t{20}));
	EXPECT_EQ(records[1].getNode("n")->getProperty("name"), PropertyValue("from-disk"));
}

TEST_F(NodeColumnBatchMaterializerTest, PropertyMaterializationIgnoresMissingColumnSlots) {
	const int64_t first = addPerson({{"age", PropertyValue(int64_t{10})}});
	const int64_t second = addPerson({{"age", PropertyValue(int64_t{20})}});
	const int64_t third = addPerson({{"age", PropertyValue(int64_t{30})}});

	NodeColumnBatch batch;
	batch.nodeIds = {first, second, third};
	batch.selected = {1, 1, 1};
	batch.propertyColumns["age"] = {PropertyValue(int64_t{101}), std::nullopt};

	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);

	ASSERT_EQ(records.size(), 3U);
	ASSERT_TRUE(records[0].getNode("n").has_value());
	ASSERT_TRUE(records[1].getNode("n").has_value());
	ASSERT_TRUE(records[2].getNode("n").has_value());
	EXPECT_EQ(records[0].getNode("n")->getProperty("age"), PropertyValue(int64_t{101}));
	EXPECT_FALSE(records[1].getNode("n")->hasProperty("age"));
	EXPECT_FALSE(records[2].getNode("n")->hasProperty("age"));
}

TEST_F(NodeColumnBatchMaterializerTest, RecordsPerfTraceWhenEnabled) {
	const int64_t first = addPerson();
	NodeColumnBatch batch;
	batch.nodeIds = {first};
	batch.selected = {1};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto records = materializeNodeRecords(batch, "n", *dm, requirements);
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_EQ(records.size(), 1U);
	EXPECT_TRUE(snapshot.contains("node_scan.materialize"));
}
