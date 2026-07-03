#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class NodeTopKScanOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath =
				fs::temp_directory_path() / ("test_node_topk_scan_path_" + boost::uuids::to_string(uuid) + ".dat");
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

	static std::string readString(const Record &record, const std::string &alias) {
		auto value = record.getValue(alias);
		EXPECT_TRUE(value.has_value());
		return value ? value->toString() : "";
	}
};

TEST_F(NodeTopKScanOperatorTest, ReturnsProjectedRowsInDescendingOrder) {
	static constexpr size_t kNodeCount = 300;
	for (size_t i = 0; i < kNodeCount; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(static_cast<int64_t>(i))}});
	}
	addLabeledNode({"Animal"}, {{"id", PropertyValue("animal")}, {"score", PropertyValue(int64_t{1000})}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 3U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-299");
	EXPECT_EQ(readString((*batch)[1], "id"), "user-298");
	EXPECT_EQ(readString((*batch)[2], "id"), "user-297");
	EXPECT_FALSE(op.next().has_value());
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeTopKScanOperatorTest, SkipsRedundantChecksForLabelIndexCandidates) {
	ASSERT_TRUE(im->createIndex("idx_person_label_topk_operator", "node", "", ""));
	for (int64_t i = 0; i < 300; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))}, {"score", PropertyValue(i)}});
	}
	addLabeledNode({"Animal"}, {{"id", PropertyValue("animal")}, {"score", PropertyValue(int64_t{1000})}});
	db->getStorage()->flush();
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-299");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
}

TEST_F(NodeTopKScanOperatorTest, AppliesResidualPredicateBeforeRanking) {
	for (int64_t i = 0; i < 10; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(i)},
				   {"country", PropertyValue(i >= 5 ? "CN" : "US")}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "country"};

	VectorizedPropertyPredicate predicate;
	predicate.variable = "u";
	predicate.propertyKey = "country";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue("US");

	NodeTopKScanOperator op(dm, im, config, requirements, {predicate}, {{"id", "id"}}, "score", false, 2);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(readString((*batch)[0], "id"), "user-4");
	EXPECT_EQ(readString((*batch)[1], "id"), "user-3");
}

TEST_F(NodeTopKScanOperatorTest, LoadsOnlyRetainedProjectionProperties) {
	for (int64_t i = 0; i < 20; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(i)},
				   {"city", PropertyValue(i == 19 ? "Shanghai" : "Seattle")}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "city", "missing"};

	NodeTopKScanOperator op(dm, im, config, requirements, {},
								{{"city", "city"}, {"score", "score"}, {"missing", "missing"}}, "score", false, 1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "city"), "Shanghai");
	EXPECT_EQ(readString((*batch)[0], "score"), "19");
	EXPECT_EQ(readString((*batch)[0], "missing"), "null");
}

TEST_F(NodeTopKScanOperatorTest, RepeatsTopKWithoutResultCache) {
	for (int64_t i = 0; i < 200; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))}, {"score", PropertyValue(i)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKScanOperator first(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	first.open();
	auto firstBatch = first.next();
	ASSERT_TRUE(firstBatch.has_value());
	ASSERT_EQ(firstBatch->size(), 3U);
	EXPECT_EQ(readString((*firstBatch)[0], "id"), "user-199");

	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	NodeTopKScanOperator second(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 3);
	second.open();
	auto secondBatch = second.next();
	ASSERT_TRUE(secondBatch.has_value());
	ASSERT_EQ(secondBatch->size(), 3U);
	EXPECT_EQ(readString((*secondBatch)[0], "id"), "user-199");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.topk_cache"));
}

TEST_F(NodeTopKScanOperatorTest, UsesMetadataSortKeyPathWithoutColumnMaterialization) {
	for (int64_t i = 0; i < 200; ++i) {
		addPerson({{"score", PropertyValue(i)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"score"};

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", false, 2);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(readString((*batch)[0], "score"), "199");
	EXPECT_EQ(readString((*batch)[1], "score"), "198");
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_property_entities"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
}

TEST_F(NodeTopKScanOperatorTest, ReturnsMultipleBatchesWhenLimitExceedsDefaultBatch) {
	const size_t rowCount = PhysicalOperator::DEFAULT_BATCH_SIZE + 5;
	for (size_t i = 0; i < rowCount; ++i) {
		addPerson({{"id", PropertyValue("user-" + std::to_string(i))},
				   {"score", PropertyValue(static_cast<int64_t>(i))}});
	}

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", true,
								static_cast<int64_t>(rowCount));
	op.open();
	auto first = op.next();
	auto second = op.next();

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(first->size(), PhysicalOperator::DEFAULT_BATCH_SIZE);
	EXPECT_EQ(second->size(), 5U);
	EXPECT_EQ(readString((*first)[0], "score"), "0");
	EXPECT_EQ(readString((*second)[4], "score"), std::to_string(rowCount - 1));
	EXPECT_FALSE(op.next().has_value());
	EXPECT_NE(op.toString().find("ASC LIMIT"), std::string::npos);
}

TEST_F(NodeTopKScanOperatorTest, HandlesEmptyProjectionAndEmptyResult) {
	addPerson({{"id", PropertyValue("first")}, {"score", PropertyValue(int64_t{1})}, {"country", PropertyValue("US")}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score", "country"};

	NodeTopKScanOperator noProjection(dm, im, config, requirements, {}, {}, "score", false, 1);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	noProjection.open();
	EXPECT_FALSE(noProjection.next().has_value());
	EXPECT_TRUE(noProjection.getOutputVariables().empty());
	EXPECT_TRUE(debug::PerfTrace::snapshotAndReset().contains("node_scan.topk"));
	debug::PerfTrace::setEnabled(false);

	VectorizedPropertyPredicate predicate;
	predicate.variable = "u";
	predicate.propertyKey = "country";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue("CN");

	NodeTopKScanOperator noMatch(dm, im, config, requirements, {predicate}, {{"id", "id"}}, "score", false, 1);
	noMatch.open();
	EXPECT_FALSE(noMatch.next().has_value());
}

TEST_F(NodeTopKScanOperatorTest, CloseOpenAllowsReExecution) {
	addPerson({{"id", PropertyValue("first")}, {"score", PropertyValue(int64_t{1})}});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, "score", false, 1);
	op.open();
	ASSERT_TRUE(op.next().has_value());
	op.close();

	addPerson({{"id", PropertyValue("second")}, {"score", PropertyValue(int64_t{2})}});
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(readString((*batch)[0], "id"), "second");
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"id"}));
	EXPECT_NE(op.toString().find("NodeTopKScan"), std::string::npos);
}

TEST_F(NodeTopKScanOperatorTest, MetadataSortPathHandlesTemporalBooleanAndFallbackScalars) {
	for (int64_t i = 0; i < 160; ++i) {
		std::vector<PropertyValue> rankList{PropertyValue(i)};
		addPerson({{"flag", PropertyValue(i % 2 == 0)},
		           {"eventDate", PropertyValue(TemporalDate{static_cast<int32_t>(i)})},
		           {"eventTime", PropertyValue(TemporalDateTime{i})},
		           {"duration", PropertyValue(TemporalDuration{0, static_cast<int32_t>(i), 0})},
		           {"rankList", PropertyValue(rankList)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	auto runTopOne = [&](const std::string &property, bool ascending) {
		NodeTopKScanOperator op(dm, im, config, requirements, {}, {{property, property}}, property, ascending, 1);
		op.open();
		auto batch = op.next();
		EXPECT_FALSE(op.next().has_value());
		return batch;
	};

	auto trueBatch = runTopOne("flag", false);
	ASSERT_TRUE(trueBatch.has_value());
	ASSERT_EQ(trueBatch->size(), 1U);
	EXPECT_EQ((*trueBatch)[0].getValue("flag"), std::optional<PropertyValue>(PropertyValue(true)));

	auto dateBatch = runTopOne("eventDate", false);
	ASSERT_TRUE(dateBatch.has_value());
	EXPECT_EQ((*dateBatch)[0].getValue("eventDate"),
	          std::optional<PropertyValue>(PropertyValue(TemporalDate{159})));

	auto dateTimeBatch = runTopOne("eventTime", false);
	ASSERT_TRUE(dateTimeBatch.has_value());
	EXPECT_EQ((*dateTimeBatch)[0].getValue("eventTime"),
	          std::optional<PropertyValue>(PropertyValue(TemporalDateTime{159})));

	auto durationBatch = runTopOne("duration", false);
	ASSERT_TRUE(durationBatch.has_value());
	EXPECT_EQ((*durationBatch)[0].getValue("duration"),
	          std::optional<PropertyValue>(PropertyValue(TemporalDuration{0, 159, 0})));

	auto listBatch = runTopOne("rankList", false);
	ASSERT_TRUE(listBatch.has_value());
	std::vector<PropertyValue> expectedList{PropertyValue(int64_t{159})};
	EXPECT_EQ((*listBatch)[0].getValue("rankList"), std::optional<PropertyValue>(PropertyValue(expectedList)));
}

TEST_F(NodeTopKScanOperatorTest, MetadataSortPathHandlesDoubleStringNullMissingAndBlobValues) {
	for (int64_t i = 0; i < 160; ++i) {
		std::string blobKey(600, static_cast<char>('a' + (i % 26)));
		addPerson({{"ratio", PropertyValue(static_cast<double>(i) + 0.5)},
		           {"name", PropertyValue("name-" + std::to_string(i))},
		           {"nothing", PropertyValue(std::monostate{})},
		           {"blobKey", PropertyValue(blobKey)}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	auto runTopOne = [&](const std::string &property, bool ascending) {
		NodeTopKScanOperator op(dm, im, config, requirements, {}, {{property, property}}, property, ascending, 1);
		op.open();
		return op.next();
	};

	auto ratioBatch = runTopOne("ratio", false);
	ASSERT_TRUE(ratioBatch.has_value());
	EXPECT_EQ((*ratioBatch)[0].getValue("ratio"), std::optional<PropertyValue>(PropertyValue(159.5)));

	auto nameBatch = runTopOne("name", false);
	ASSERT_TRUE(nameBatch.has_value());
	ASSERT_TRUE((*nameBatch)[0].getValue("name").has_value());
	EXPECT_NE((*nameBatch)[0].getValue("name")->toString().find("name-"), std::string::npos);

	auto nullBatch = runTopOne("nothing", false);
	ASSERT_TRUE(nullBatch.has_value());
	EXPECT_EQ((*nullBatch)[0].getValue("nothing"), std::optional<PropertyValue>(PropertyValue()));

	auto missingBatch = runTopOne("missingSort", false);
	ASSERT_TRUE(missingBatch.has_value());
	EXPECT_EQ((*missingBatch)[0].getValue("missingSort"), std::optional<PropertyValue>(PropertyValue()));

	auto blobBatch = runTopOne("blobKey", false);
	ASSERT_TRUE(blobBatch.has_value());
	ASSERT_TRUE((*blobBatch)[0].getValue("blobKey").has_value());
	EXPECT_EQ(std::get<std::string>((*blobBatch)[0].getValue("blobKey")->getVariant()).front(), 'z');
}

TEST_F(NodeTopKScanOperatorTest, MetadataSortPathHandlesInlineDoubleStringNullAndMissingStorage) {
	for (int64_t i = 0; i < 160; ++i) {
		addPerson({{"ratio", PropertyValue(static_cast<double>(i) + 0.5)},
		           {"name", PropertyValue("name-" + std::to_string(i))},
		           {"nothing", PropertyValue(std::monostate{})}});
	}
	for (int64_t i = 0; i < 5; ++i) {
		addPerson();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	auto runTopOne = [&](const std::string &property, bool ascending) {
		NodeTopKScanOperator op(dm, im, config, requirements, {}, {{property, property}}, property, ascending, 1);
		op.open();
		return op.next();
	};

	auto ratioBatch = runTopOne("ratio", false);
	ASSERT_TRUE(ratioBatch.has_value());
	EXPECT_EQ((*ratioBatch)[0].getValue("ratio"), std::optional<PropertyValue>(PropertyValue(159.5)));

	auto nameBatch = runTopOne("name", false);
	ASSERT_TRUE(nameBatch.has_value());
	ASSERT_TRUE((*nameBatch)[0].getValue("name").has_value());
	EXPECT_NE((*nameBatch)[0].getValue("name")->toString().find("name-"), std::string::npos);

	auto nullBatch = runTopOne("nothing", false);
	ASSERT_TRUE(nullBatch.has_value());
	EXPECT_EQ((*nullBatch)[0].getValue("nothing"), std::optional<PropertyValue>(PropertyValue()));

	auto missingBatch = runTopOne("missingSort", false);
	ASSERT_TRUE(missingBatch.has_value());
	EXPECT_EQ((*missingBatch)[0].getValue("missingSort"), std::optional<PropertyValue>(PropertyValue()));
}

TEST_F(NodeTopKScanOperatorTest, MetadataSortPathLoadsProjectedPropertiesFromBlobBackedRows) {
	for (int64_t i = 0; i < 160; ++i) {
		std::string blobKey(600, static_cast<char>('a' + (i % 26)));
		addPerson({{"blobKey", PropertyValue(blobKey)},
		           {"tag", PropertyValue("tag-" + std::to_string(i))}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"tag", "tag"}}, "blobKey", false, 1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ((*batch)[0].getValue("tag"), std::optional<PropertyValue>(PropertyValue("tag-25")));
}

TEST_F(NodeTopKScanOperatorTest, EmptyCandidateSetUsesMetadataPathAndReturnsNoRows) {
	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", false, 5);
	op.open();
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeTopKScanOperatorTest, NextBeforeOpenTreatsCandidateSetAsEmpty) {
	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"Person"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	NodeTopKScanOperator op(dm, im, config, requirements, {}, {{"score", "score"}}, "score", false, 5);
	EXPECT_FALSE(op.next().has_value());
	op.close();
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(NodeTopKScanOperatorTest, NextBeforeOpenHandlesMissingStorage) {
	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	NodeTopKScanOperator op(nullptr, nullptr, config, requirements, {}, {{"score", "score"}}, "score", false, 5);
	EXPECT_FALSE(op.next().has_value());
}
