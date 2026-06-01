#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeGroupCountFastPathOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeGroupCountFastPathOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
				("test_node_group_count_fast_path_" + boost::uuids::to_string(uuid) + ".dat");
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
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

	static std::unordered_map<std::string, int64_t> readStringGroups(const RecordBatch &batch) {
		std::unordered_map<std::string, int64_t> groups;
		for (const auto &record : batch) {
			auto country = record.getValue("country");
			auto count = record.getValue("count");
			if (!country.has_value() || !count.has_value()) {
				ADD_FAILURE() << "group-count record is missing an output value";
				continue;
			}
			const std::string key = country->getType() == PropertyType::NULL_TYPE
					? std::string{"<null>"}
					: std::get<std::string>(country->getVariant());
			groups[key] = std::get<int64_t>(count->getVariant());
		}
		return groups;
	}
};

TEST_F(NodeGroupCountFastPathOperatorTest, CountsRowsByPropertyFromMetadataColumns) {
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"country", PropertyValue(i % 2 == 0 ? "CN" : "US")}});
	}
	addPerson();
	addLabeledNode({"Animal"}, {{"country", PropertyValue("CN")}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeGroupCountFastPathOperator op(dm, im, config, requirements, {}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 3U);
	EXPECT_EQ(groups["CN"], 150);
	EXPECT_EQ(groups["US"], 150);
	EXPECT_EQ(groups["<null>"], 1);
	EXPECT_FALSE(op.next().has_value());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.group_count"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeGroupCountFastPathOperatorTest, AppliesFallbackPredicatesWhenNeeded) {
	addPerson({{"country", PropertyValue("CN")}, {"age", PropertyValue(int64_t{40})}});
	addPerson({{"country", PropertyValue("CN")}, {"age", PropertyValue(int64_t{20})}});
	addPerson({{"country", PropertyValue("US")}, {"age", PropertyValue(int64_t{35})}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country", "age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_GE;
	predicate.value = PropertyValue(int64_t{30});

	NodeGroupCountFastPathOperator op(dm, im, config, requirements, {predicate}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 2U);
	EXPECT_EQ(groups["CN"], 1);
	EXPECT_EQ(groups["US"], 1);
}
