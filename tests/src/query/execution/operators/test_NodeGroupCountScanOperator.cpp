#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeGroupCountScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeGroupCountScanOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
				("test_node_group_count_scan_path_" + boost::uuids::to_string(uuid) + ".dat");
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

	static std::string typedGroupKey(const PropertyValue &value) {
		switch (value.getType()) {
			case PropertyType::NULL_TYPE:
				return "null";
			case PropertyType::BOOLEAN:
				return std::get<bool>(value.getVariant()) ? "bool:true" : "bool:false";
			case PropertyType::INTEGER:
				return "int:" + std::to_string(std::get<int64_t>(value.getVariant()));
			case PropertyType::DOUBLE:
				return "double:" + std::to_string(std::get<double>(value.getVariant()));
			case PropertyType::STRING:
				return "string:" + std::get<std::string>(value.getVariant());
			case PropertyType::DATE:
				return "date:" + std::to_string(std::get<TemporalDate>(value.getVariant()).epochDays);
			case PropertyType::DATETIME:
				return "datetime:" + std::to_string(std::get<TemporalDateTime>(value.getVariant()).epochMillis);
			case PropertyType::DURATION: {
				const auto duration = std::get<TemporalDuration>(value.getVariant());
				return "duration:" + std::to_string(duration.months) + ":" + std::to_string(duration.days) + ":" +
					   std::to_string(duration.nanos);
			}
			case PropertyType::LIST:
				return "list";
			case PropertyType::MAP:
				return "map";
			case PropertyType::COMPOSITE:
			case PropertyType::UNKNOWN:
			default:
				return "other";
		}
	}

	static std::unordered_map<std::string, int64_t> readTypedGroups(const RecordBatch &batch) {
		std::unordered_map<std::string, int64_t> groups;
		for (const auto &record : batch) {
			auto value = record.getValue("bucket");
			auto count = record.getValue("count");
			if (!value.has_value() || !count.has_value()) {
				ADD_FAILURE() << "group-count record is missing an output value";
				continue;
			}
			groups[typedGroupKey(*value)] = std::get<int64_t>(count->getVariant());
		}
		return groups;
	}
};

TEST_F(NodeGroupCountScanOperatorTest, CountsRowsByPropertyFromMetadataValueStream) {
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

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "country", "country", "count");
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
	EXPECT_TRUE(snapshot.contains("node_scan.load_property_entities"));
	EXPECT_FALSE(snapshot.contains("node_scan.extract_property_columns"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeGroupCountScanOperatorTest, MetadataPathGroupsRowsWithoutPropertyEntitiesAsNull) {
	for (int64_t i = 0; i < 4; ++i) {
		addPerson();
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 1U);
	EXPECT_EQ(groups["<null>"], 4);
}

TEST_F(NodeGroupCountScanOperatorTest, MetadataPathWithAllValuesAvoidsNullBucket) {
	for (int64_t i = 0; i < 160; ++i) {
		addPerson({{"country", PropertyValue(i % 2 == 0 ? "CN" : "US")}});
	}
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(false);

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"country"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 2U);
	EXPECT_EQ(groups["CN"], 80);
	EXPECT_EQ(groups["US"], 80);
}

TEST_F(NodeGroupCountScanOperatorTest, MetadataPathGroupsTypedScalarAndFallbackValues) {
	addPerson();
	addPerson({{"bucket", PropertyValue(false)}});
	addPerson({{"bucket", PropertyValue(true)}});
	addPerson({{"bucket", PropertyValue(int64_t{7})}});
	addPerson({{"bucket", PropertyValue(int64_t{7})}});
	addPerson({{"bucket", PropertyValue(1.5)}});
	addPerson({{"bucket", PropertyValue("CN")}});
	addPerson({{"bucket", PropertyValue(TemporalDate{42})}});
	addPerson({{"bucket", PropertyValue(TemporalDateTime{84})}});
	addPerson({{"bucket", PropertyValue(TemporalDuration{1, 2, 3})}});
	addPerson({{"bucket", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{1})})}});
	addPerson({{"bucket", PropertyValue(PropertyValue::MapType{{"k", PropertyValue(int64_t{1})}})}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"bucket"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "bucket", "bucket", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readTypedGroups(*batch);
	EXPECT_EQ(groups["null"], 1);
	EXPECT_EQ(groups["bool:false"], 1);
	EXPECT_EQ(groups["bool:true"], 1);
	EXPECT_EQ(groups["int:7"], 2);
	EXPECT_EQ(groups["double:1.500000"], 1);
	EXPECT_EQ(groups["string:CN"], 1);
	EXPECT_EQ(groups["date:42"], 1);
	EXPECT_EQ(groups["datetime:84"], 1);
	EXPECT_EQ(groups["duration:1:2:3"], 1);
	EXPECT_EQ(groups["list"], 1);
	EXPECT_EQ(groups["map"], 1);
}

TEST_F(NodeGroupCountScanOperatorTest, AppliesFallbackPredicatesWhenNeeded) {
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

	NodeGroupCountScanOperator op(dm, im, config, requirements, {predicate}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 2U);
	EXPECT_EQ(groups["CN"], 1);
	EXPECT_EQ(groups["US"], 1);
}

TEST_F(NodeGroupCountScanOperatorTest, FallbackPredicateGroupsMissingPropertyAsNull) {
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"age", PropertyValue(i)}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age"};
	requirements.countOnly = true;

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_GE;
	predicate.value = PropertyValue(int64_t{0});

	NodeGroupCountScanOperator op(dm, im, config, requirements, {predicate}, "missing", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 1U);
	EXPECT_EQ(groups["<null>"], 300);
}

TEST_F(NodeGroupCountScanOperatorTest, CountsBlobBackedAndMissingGroupsFromMetadataFallback) {
	const std::string cn(600, 'c');
	const std::string us(600, 'u');
	for (int64_t i = 0; i < 160; ++i) {
		if (i == 159) {
			addPerson();
		} else {
			addPerson({{"payload", PropertyValue(i % 2 == 0 ? cn : us)}});
		}
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"payload"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "payload", "country", "count");
	op.open();
	auto batch = op.next();
	const auto snapshot = debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(batch.has_value());
	std::unordered_map<std::string, int64_t> groups;
	for (const auto &record : *batch) {
		auto value = record.getValue("country");
		auto count = record.getValue("count");
		ASSERT_TRUE(value.has_value());
		ASSERT_TRUE(count.has_value());
		groups[value->getType() == PropertyType::NULL_TYPE ? "<null>" : std::get<std::string>(value->getVariant())] =
				std::get<int64_t>(count->getVariant());
	}
	EXPECT_EQ(groups[cn], 80);
	EXPECT_EQ(groups[us], 79);
	EXPECT_EQ(groups["<null>"], 1);
	EXPECT_TRUE(snapshot.contains("node_scan.load_properties"));
}

TEST_F(NodeGroupCountScanOperatorTest, BlobFallbackMissingGroupPropertyCountsAsNull) {
	const std::string payload(600, 'p');
	for (int64_t i = 0; i < 8; ++i) {
		addPerson({{"payload", PropertyValue(payload)}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"missing"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "missing", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 1U);
	EXPECT_EQ(groups["<null>"], 8);
}

TEST_F(NodeGroupCountScanOperatorTest, BlobFallbackWithDifferentGroupPropertyCountsAsNull) {
	const std::string payload(600, 'p');
	for (int64_t i = 0; i < 8; ++i) {
		addPerson({{"payload", PropertyValue(payload)}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "n";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"payload"};
	requirements.countOnly = true;

	NodeGroupCountScanOperator op(dm, im, config, requirements, {}, "country", "country", "count");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	auto groups = readStringGroups(*batch);
	EXPECT_EQ(groups.size(), 1U);
	EXPECT_EQ(groups["<null>"], 8);
}
