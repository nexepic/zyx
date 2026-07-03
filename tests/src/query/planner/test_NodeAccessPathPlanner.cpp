#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/planner/NodeAccessPathPlanner.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;

namespace fs = std::filesystem;

namespace {

LogicalNodeScan makeScan() {
	return LogicalNodeScan("n", {"Person"});
}

RangePredicate makeClosedRange() {
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	return range;
}

fs::path makeDbPath(const std::string &prefix) {
	return fs::temp_directory_path() /
	       (prefix + "_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".zyx");
}

} // namespace

TEST(NodeAccessPathPlannerTest, ExpressionHelpersRecognizeVariablesAndProperties) {
	auto variable = std::make_shared<expressions::VariableReferenceExpression>("n");
	auto property = std::make_shared<expressions::VariableReferenceExpression>("n", "age");
	auto otherProperty = std::make_shared<expressions::VariableReferenceExpression>("m", "age");
	auto literal = std::make_shared<expressions::LiteralExpression>(int64_t{1});

	EXPECT_TRUE(isNodeVariableReference(nullptr, "n"));
	EXPECT_TRUE(isNodeVariableReference(variable, "n"));
	EXPECT_FALSE(isNodeVariableReference(variable, "m"));
	EXPECT_FALSE(isNodeVariableReference(property, "n"));
	EXPECT_FALSE(isNodeVariableReference(literal, "n"));

	ASSERT_NE(asNodePropertyAccess(property, "n"), nullptr);
	EXPECT_EQ(asNodePropertyAccess(nullptr, "n"), nullptr);
	EXPECT_EQ(asNodePropertyAccess(otherProperty, "n"), nullptr);
	EXPECT_EQ(asNodePropertyAccess(variable, "n"), nullptr);
}

TEST(NodeAccessPathPlannerTest, AppendResidualPredicatesDeduplicatesRequiredProperties) {
	LogicalNodeScan scan("n", {"Person"}, {{"age", PropertyValue(int64_t{42})}});
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "city"};
	composite.values = {PropertyValue("CN"), PropertyValue("Shanghai")};
	scan.setCompositeEquality(std::move(composite));
	scan.setRangePredicates({makeClosedRange()});

	execution::NodeScanRequirements requirements;
	addRequiredNodeProperty(requirements, "age");
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	execution::NodeScanConfig config;
	config.variable = "n";
	config.labels = {"Person"};

	ASSERT_TRUE(appendResidualNodePredicates(scan, config, requirements, predicates));

	EXPECT_EQ(requirements.requiredProperties,
	          (std::vector<std::string>{"age", "country", "city"}));
	ASSERT_EQ(predicates.size(), 4U);
	EXPECT_EQ(predicates[0].propertyKey, "age");
	EXPECT_EQ(predicates[0].op, execution::VectorPredicateOp::VPO_EQ);
	EXPECT_EQ(predicates[1].propertyKey, "age");
	EXPECT_EQ(predicates[1].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);
	EXPECT_EQ(predicates[2].propertyKey, "country");
	EXPECT_EQ(predicates[3].propertyKey, "city");
}

TEST(NodeAccessPathPlannerTest, PreferredConfigAndFallbackPreserveLabelsAndClearIndexFields) {
	auto scan = makeScan();
	scan.setPropertyPredicates({{"age", PropertyValue(int64_t{42})}});
	scan.setPreferredScanType(execution::ScanType::PROPERTY_SCAN);

	auto decision = chooseNodeAccessPathDecision(scan, nullptr);
	ASSERT_EQ(decision.candidates.size(), 1U);
	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_PROPERTY_INDEX);
	EXPECT_TRUE(decision.selected.preferred);
	EXPECT_TRUE(decision.selectedSupportsDirectCandidateLookup());
	EXPECT_FALSE(decision.selectedRequiresConservativeFallback());

	auto config = chooseNodeAccessPathConfig(scan, nullptr);
	ASSERT_EQ(config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_EQ(config.indexKey, "age");

	fallbackToLabelOrFullScan(config);
	EXPECT_EQ(config.type, execution::ScanType::LABEL_SCAN);
	EXPECT_EQ(config.labels, (std::vector<std::string>{"Person"}));
	EXPECT_TRUE(config.indexKey.empty());
	EXPECT_TRUE(config.compositeKeys.empty());

	execution::NodeScanConfig unlabeledConfig;
	unlabeledConfig.type = execution::ScanType::RANGE_SCAN;
	unlabeledConfig.indexKey = "age";
	unlabeledConfig.rangeMin = PropertyValue(int64_t{1});
	unlabeledConfig.rangeMax = PropertyValue(int64_t{9});
	fallbackToLabelOrFullScan(unlabeledConfig);
	EXPECT_EQ(unlabeledConfig.type, execution::ScanType::FULL_SCAN);
	EXPECT_TRUE(unlabeledConfig.indexKey.empty());
}

TEST(NodeAccessPathPlannerTest, PreferredConfigCoversRangeCompositeLabelAndFullScan) {
	auto rangeScan = makeScan();
	rangeScan.setRangePredicates({makeClosedRange()});
	rangeScan.setPreferredScanType(execution::ScanType::RANGE_SCAN);
	auto rangeDecision = chooseNodeAccessPathDecision(rangeScan, nullptr);
	EXPECT_EQ(rangeDecision.selected.config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_EQ(rangeDecision.selected.config.indexKey, "age");
	EXPECT_FALSE(rangeDecision.selected.openRange);

	auto compositeScan = makeScan();
	compositeScan.setCompositeEquality({{"country", "city"}, {PropertyValue("CN"), PropertyValue("Shanghai")}});
	compositeScan.setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	auto compositeDecision = chooseNodeAccessPathDecision(compositeScan, nullptr);
	EXPECT_EQ(compositeDecision.selected.config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositeDecision.selectedSupportsDirectCandidateLookup());

	auto labelScan = makeScan();
	labelScan.setPreferredScanType(execution::ScanType::LABEL_SCAN);
	EXPECT_EQ(chooseNodeAccessPathDecision(labelScan, nullptr).selected.config.type, execution::ScanType::LABEL_SCAN);

	auto fullScan = LogicalNodeScan("n");
	fullScan.setPreferredScanType(execution::ScanType::FULL_SCAN);
	EXPECT_EQ(chooseNodeAccessPathDecision(fullScan, nullptr).selected.config.type, execution::ScanType::FULL_SCAN);
}

TEST(NodeAccessPathPlannerTest, InvalidPreferredScanTypeIsReportedAsMalformed) {
	auto scan = makeScan();
	scan.setPreferredScanType(static_cast<execution::ScanType>(255));

	auto decision = chooseNodeAccessPathDecision(scan, nullptr);

	ASSERT_EQ(decision.candidates.size(), 1U);
	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);
	EXPECT_FALSE(decision.selected.valid);
	EXPECT_TRUE(decision.selectedRequiresConservativeFallback());
}

TEST(NodeAccessPathPlannerTest, PreferredDecisionMarksMalformedCandidateForConservativeFallback) {
	auto scan = makeScan();
	scan.setPreferredScanType(execution::ScanType::PROPERTY_SCAN);

	auto decision = chooseNodeAccessPathDecision(scan, nullptr);

	ASSERT_EQ(decision.candidates.size(), 1U);
	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_PROPERTY_INDEX);
	EXPECT_TRUE(decision.selected.preferred);
	EXPECT_FALSE(decision.selected.valid);
	EXPECT_FALSE(decision.selectedSupportsDirectCandidateLookup());
	EXPECT_TRUE(decision.selectedRequiresConservativeFallback());
}

TEST(NodeAccessPathPlannerTest, RejectsUnboundedRangeAndMalformedComposite) {
	auto unboundedScan = makeScan();
	RangePredicate unbounded;
	unbounded.key = "age";
	unboundedScan.setRangePredicates({unbounded});
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	execution::NodeScanConfig config;
	EXPECT_FALSE(appendResidualNodePredicates(unboundedScan, config, requirements, predicates));

	auto malformedScan = makeScan();
	CompositeEqualityPredicate malformed;
	malformed.keys = {"country", "age"};
	malformed.values = {PropertyValue("CN")};
	malformedScan.setCompositeEquality(std::move(malformed));
	EXPECT_FALSE(appendResidualNodePredicates(malformedScan, config, requirements, predicates));
}

TEST(NodeAccessPathPlannerTest, RangeCandidateSourceAbsorbsClosedRangeButKeepsOpenRangeResidual) {
	auto closedScan = makeScan();
	closedScan.setRangePredicates({makeClosedRange()});
	execution::NodeScanConfig closedConfig;
	closedConfig.type = execution::ScanType::RANGE_SCAN;
	closedConfig.indexKey = "age";
	closedConfig.rangeMin = PropertyValue(int64_t{18});
	closedConfig.rangeMax = PropertyValue(int64_t{65});

	execution::NodeScanRequirements closedRequirements;
	std::vector<execution::VectorizedPropertyPredicate> closedPredicates;
	ASSERT_TRUE(appendResidualNodePredicates(closedScan, closedConfig, closedRequirements, closedPredicates));
	EXPECT_TRUE(closedPredicates.empty());

	auto openScan = makeScan();
	RangePredicate open;
	open.key = "age";
	open.maxValue = PropertyValue(int64_t{65});
	openScan.setRangePredicates({open});
	execution::NodeScanConfig openConfig;
	openConfig.type = execution::ScanType::RANGE_SCAN;
	openConfig.indexKey = "age";
	openConfig.rangeMax = PropertyValue(int64_t{65});

	execution::NodeScanRequirements openRequirements;
	std::vector<execution::VectorizedPropertyPredicate> openPredicates;
	ASSERT_TRUE(appendResidualNodePredicates(openScan, openConfig, openRequirements, openPredicates));
	ASSERT_EQ(openPredicates.size(), 1U);
	EXPECT_EQ(openPredicates[0].op, execution::VectorPredicateOp::VPO_LE);
}

TEST(NodeAccessPathPlannerTest, RangeResidualPredicatesPreserveInclusiveAndExclusiveBounds) {
	auto scan = makeScan();
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	range.minInclusive = false;
	range.maxInclusive = false;
	scan.setRangePredicates({range});

	execution::NodeScanConfig mismatchedConfig;
	mismatchedConfig.type = execution::ScanType::RANGE_SCAN;
	mismatchedConfig.indexKey = "age";
	mismatchedConfig.rangeMin = range.minValue;
	mismatchedConfig.rangeMax = range.maxValue;
	mismatchedConfig.minInclusive = true;
	mismatchedConfig.maxInclusive = true;

	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	ASSERT_TRUE(appendResidualNodePredicates(scan, mismatchedConfig, requirements, predicates));

	ASSERT_EQ(predicates.size(), 2U);
	EXPECT_EQ(predicates[0].op, execution::VectorPredicateOp::VPO_GT);
	EXPECT_EQ(predicates[1].op, execution::VectorPredicateOp::VPO_LT);
	EXPECT_EQ(requirements.requiredProperties, (std::vector<std::string>{"age"}));
}

TEST(NodeAccessPathPlannerTest, RangeResidualCoversHalfOpenAndDifferentIndexKey) {
	auto halfOpenScan = makeScan();
	RangePredicate halfOpen;
	halfOpen.key = "age";
	halfOpen.minValue = PropertyValue(int64_t{18});
	halfOpen.maxValue = PropertyValue(int64_t{65});
	halfOpen.minInclusive = true;
	halfOpen.maxInclusive = false;
	halfOpenScan.setRangePredicates({halfOpen});

	execution::NodeScanConfig differentKeyConfig;
	differentKeyConfig.type = execution::ScanType::RANGE_SCAN;
	differentKeyConfig.indexKey = "height";
	differentKeyConfig.rangeMin = halfOpen.minValue;
	differentKeyConfig.rangeMax = halfOpen.maxValue;
	differentKeyConfig.minInclusive = halfOpen.minInclusive;
	differentKeyConfig.maxInclusive = halfOpen.maxInclusive;

	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	ASSERT_TRUE(appendResidualNodePredicates(halfOpenScan, differentKeyConfig, requirements, predicates));

	ASSERT_EQ(predicates.size(), 2U);
	EXPECT_EQ(predicates[0].op, execution::VectorPredicateOp::VPO_GE);
	EXPECT_EQ(predicates[1].op, execution::VectorPredicateOp::VPO_LT);
}

TEST(NodeAccessPathPlannerTest, ResidualPredicatesSkipHandledCompositeAndAddSingleSidedRange) {
	auto compositeScan = makeScan();
	compositeScan.setCompositeEquality({{"country", "city"}, {PropertyValue("CN"), PropertyValue("Shanghai")}});
	execution::NodeScanConfig compositeConfig;
	compositeConfig.type = execution::ScanType::COMPOSITE_SCAN;
	compositeConfig.compositeKeys = {"country", "city"};
	compositeConfig.compositeValues = {PropertyValue("CN"), PropertyValue("Shanghai")};
	execution::NodeScanRequirements compositeRequirements;
	std::vector<execution::VectorizedPropertyPredicate> compositePredicates;
	ASSERT_TRUE(appendResidualNodePredicates(
			compositeScan,
			compositeConfig,
			compositeRequirements,
			compositePredicates));
	EXPECT_TRUE(compositePredicates.empty());
	EXPECT_TRUE(compositeRequirements.requiredProperties.empty());

	auto rangeScan = makeScan();
	RangePredicate minOnly;
	minOnly.key = "age";
	minOnly.minValue = PropertyValue(int64_t{18});
	rangeScan.setRangePredicates({minOnly});
	execution::NodeScanRequirements rangeRequirements;
	std::vector<execution::VectorizedPropertyPredicate> rangePredicates;
	ASSERT_TRUE(appendResidualNodePredicates(
			rangeScan,
			execution::NodeScanConfig{},
			rangeRequirements,
			rangePredicates));
	ASSERT_EQ(rangePredicates.size(), 1U);
	EXPECT_EQ(rangePredicates.front().op, execution::VectorPredicateOp::VPO_GE);
}

TEST(NodeAccessPathPlannerTest, CandidateConfigValidationIdentifiesUsableAccessSources) {
	execution::NodeScanConfig invalidProperty;
	invalidProperty.type = execution::ScanType::PROPERTY_SCAN;
	EXPECT_FALSE(hasValidNodeCandidateConfig(invalidProperty));

	execution::NodeScanConfig invalidRange;
	invalidRange.type = execution::ScanType::RANGE_SCAN;
	EXPECT_FALSE(hasValidNodeCandidateConfig(invalidRange));

	execution::NodeScanConfig composite;
	composite.type = execution::ScanType::COMPOSITE_SCAN;
	composite.compositeKeys = {"country", "age"};
	composite.compositeValues = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	EXPECT_TRUE(hasValidNodeCandidateConfig(composite));
	EXPECT_TRUE(isIndexCandidateSource(composite.type));

	composite.compositeValues.pop_back();
	EXPECT_FALSE(hasValidNodeCandidateConfig(composite));

	execution::NodeScanConfig full;
	full.type = execution::ScanType::FULL_SCAN;
	EXPECT_TRUE(hasValidNodeCandidateConfig(full));
	EXPECT_FALSE(isIndexCandidateSource(full.type));

	execution::NodeScanConfig label;
	label.type = execution::ScanType::LABEL_SCAN;
	EXPECT_TRUE(hasValidNodeCandidateConfig(label));
	EXPECT_FALSE(isIndexCandidateSource(label.type));

	execution::NodeScanConfig openRange;
	openRange.type = execution::ScanType::RANGE_SCAN;
	openRange.indexKey = "age";
	openRange.rangeMin = PropertyValue(int64_t{18});
	EXPECT_TRUE(hasOpenRangeBounds(openRange));
	EXPECT_FALSE(hasOpenRangeBounds(label));

	auto invalidType = static_cast<execution::ScanType>(255);
	EXPECT_FALSE(hasValidNodeCandidateConfig(execution::NodeScanConfig{.type = invalidType}));
	EXPECT_FALSE(isIndexCandidateSource(invalidType));
	EXPECT_STREQ(nodeAccessPathKindName(static_cast<NodeAccessPathKind>(255)), "unknown");
}

TEST(NodeAccessPathPlannerTest, IndexedDecisionPrioritizesCompositeAndKeepsFallbackCandidate) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_indexes");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_person_age", "node", "Person", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_person_country_age", "node", "Person", {"country", "age"}));
	ASSERT_TRUE(indexManager->createIndex("idx_person_label", "node", "Person", ""));

	LogicalNodeScan scan("n", {"Person"}, {{"age", PropertyValue(int64_t{42})}});
	scan.setCompositeEquality({{"country", "age"}, {PropertyValue("CN"), PropertyValue(int64_t{42})}});

	auto decision = chooseNodeAccessPathDecision(scan, indexManager);

	ASSERT_FALSE(decision.candidates.empty());
	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_COMPOSITE_INDEX);
	EXPECT_EQ(decision.selected.config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(decision.selectedSupportsDirectCandidateLookup());
	EXPECT_EQ(decision.candidates.back().kind, NodeAccessPathKind::NAP_FULL_SCAN);

	const auto hasPropertyCandidate = std::any_of(decision.candidates.begin(), decision.candidates.end(), [](const auto &candidate) {
		return candidate.kind == NodeAccessPathKind::NAP_PROPERTY_INDEX && candidate.config.indexKey == "age";
	});
	const auto hasLabelCandidate = std::any_of(decision.candidates.begin(), decision.candidates.end(), [](const auto &candidate) {
		return candidate.kind == NodeAccessPathKind::NAP_LABEL_SCAN;
	});
	EXPECT_TRUE(hasPropertyCandidate);
	EXPECT_TRUE(hasLabelCandidate);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, GlobalPropertyAndRangeIndexesAreValidAccessPaths) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_global_indexes");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_global_age", "node", "", "age"));

	LogicalNodeScan propertyScan("n", {}, {{"age", PropertyValue(int64_t{42})}});
	auto propertyDecision = chooseNodeAccessPathDecision(propertyScan, indexManager);
	EXPECT_EQ(propertyDecision.selected.kind, NodeAccessPathKind::NAP_PROPERTY_INDEX);
	EXPECT_EQ(propertyDecision.selected.config.indexKey, "age");

	LogicalNodeScan rangeScan("n");
	rangeScan.setRangePredicates({makeClosedRange()});
	auto rangeDecision = chooseNodeAccessPathDecision(rangeScan, indexManager);
	EXPECT_EQ(rangeDecision.selected.kind, NodeAccessPathKind::NAP_RANGE_INDEX);
	EXPECT_EQ(rangeDecision.selected.config.indexKey, "age");

	LogicalNodeScan fullScan("n");
	auto fullDecision = chooseNodeAccessPathDecision(fullScan, indexManager);
	EXPECT_EQ(fullDecision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, EmptyPropertyKeyIsNotConsideredAnIndexCandidate) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_empty_key");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_global_age_empty_key_guard", "node", "", "age"));

	LogicalNodeScan scan("n", {}, {{"", PropertyValue(int64_t{42})}});
	auto decision = chooseNodeAccessPathDecision(scan, indexManager);

	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);
	const auto propertyCandidate = std::find_if(decision.candidates.begin(), decision.candidates.end(), [](const auto &candidate) {
		return candidate.kind == NodeAccessPathKind::NAP_PROPERTY_INDEX;
	});
	EXPECT_EQ(propertyCandidate, decision.candidates.end());

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, CompositeAccessPathRequiresCompleteIndexedShape) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_composite_shape");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_country_age_shape", "node", "", {"country", "age"}));

	LogicalNodeScan singleKey("n");
	singleKey.setCompositeEquality({{"country"}, {PropertyValue("CN")}});
	auto singleKeyDecision = chooseNodeAccessPathDecision(singleKey, indexManager);
	EXPECT_EQ(singleKeyDecision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);

	LogicalNodeScan mismatchedValues("n");
	mismatchedValues.setCompositeEquality({{"country", "age"}, {PropertyValue("CN")}});
	auto mismatchedDecision = chooseNodeAccessPathDecision(mismatchedValues, indexManager);
	EXPECT_EQ(mismatchedDecision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);

	LogicalNodeScan unindexedKeys("n");
	unindexedKeys.setCompositeEquality({{"country", "city"}, {PropertyValue("CN"), PropertyValue("Shanghai")}});
	auto unindexedDecision = chooseNodeAccessPathDecision(unindexedKeys, indexManager);
	EXPECT_EQ(unindexedDecision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, EqualCostPropertyCandidatesTieBreakDeterministically) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_equal_cost");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_age", "node", "", "age"));
	ASSERT_TRUE(indexManager->createIndex("idx_code", "node", "", "code"));

	LogicalNodeScan scan("n", {}, {
		{"age", PropertyValue(int64_t{42})},
		{"code", PropertyValue("A")}
	});
	auto decision = chooseNodeAccessPathDecision(scan, indexManager);

	const auto propertyCount = std::count_if(decision.candidates.begin(), decision.candidates.end(), [](const auto &candidate) {
		return candidate.kind == NodeAccessPathKind::NAP_PROPERTY_INDEX;
	});
	EXPECT_EQ(propertyCount, 2);
	EXPECT_TRUE(decision.selected.valid);
	EXPECT_EQ(decision.selected.estimate.source, "index_count");

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, CostedDecisionChoosesMostSelectivePropertyIndexWithoutRuntimeStats) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_costed_indexes");
	Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	const auto personLabel = dm->getOrCreateTokenId("Person");

	for (int64_t id = 1; id <= 10; ++id) {
		Node node(id, personLabel);
		dm->addNode(node);
		dm->addNodeProperties(id, {
			{"bucket", PropertyValue(int64_t{1})},
			{"code", PropertyValue(id == 7 ? "needle" : "common")}
		});
	}

	ASSERT_TRUE(indexManager->createIndex("idx_person_bucket", "node", "Person", "bucket"));
	ASSERT_TRUE(indexManager->createIndex("idx_person_code", "node", "Person", "code"));
	indexManager->resetStats();

	LogicalNodeScan scan("n", {"Person"}, {
		{"bucket", PropertyValue(int64_t{1})},
		{"code", PropertyValue("needle")}
	});

	auto decision = chooseNodeAccessPathDecision(scan, indexManager);

	EXPECT_EQ(decision.selected.kind, NodeAccessPathKind::NAP_PROPERTY_INDEX);
	EXPECT_EQ(decision.selected.config.indexKey, "code");
	ASSERT_TRUE(decision.selectedEstimatedCardinality().has_value());
	EXPECT_EQ(*decision.selectedEstimatedCardinality(), 1);
	EXPECT_TRUE(decision.selected.estimate.exactCardinality);
	EXPECT_EQ(decision.selected.estimate.source, "index_count");
	EXPECT_EQ(indexManager->lookups(), 0u);
	EXPECT_EQ(indexManager->indexHits(), 0u);

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, OpenRangeIndexRequiresExplicitAccessPathOption) {
	const auto dbPath = makeDbPath("test_node_access_path_planner_open_range");
	Database db(dbPath.string());
	db.open();
	auto indexManager = db.getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_person_age", "node", "Person", "age"));

	auto scan = makeScan();
	RangePredicate minOnly;
	minOnly.key = "age";
	minOnly.minValue = PropertyValue(int64_t{18});
	scan.setRangePredicates({minOnly});

	auto conservativeDecision = chooseNodeAccessPathDecision(scan, indexManager);
	EXPECT_EQ(conservativeDecision.selected.kind, NodeAccessPathKind::NAP_FULL_SCAN);
	EXPECT_FALSE(conservativeDecision.selectedSupportsDirectCandidateLookup());

	NodeAccessPathOptions options;
	options.allowOpenRangeIndex = true;
	auto openRangeDecision = chooseNodeAccessPathDecision(scan, indexManager, options);
	EXPECT_EQ(openRangeDecision.selected.kind, NodeAccessPathKind::NAP_RANGE_INDEX);
	EXPECT_TRUE(openRangeDecision.selected.openRange);
	EXPECT_TRUE(openRangeDecision.selectedSupportsDirectCandidateLookup());
	EXPECT_TRUE(openRangeDecision.selectedRequiresConservativeFallback());

	db.close();
	std::error_code ec;
	fs::remove_all(dbPath, ec);
}

TEST(NodeAccessPathPlannerTest, DecisionAccessorsExposeCostAndInvalidSummaries) {
	NodeAccessPathDecision decision;
	decision.selected.estimate.cost = 12.5;
	EXPECT_EQ(decision.selectedEstimatedCost(), 12.5);

	NodeAccessPathCandidate invalidCandidate;
	invalidCandidate.kind = NodeAccessPathKind::NAP_FULL_SCAN;
	invalidCandidate.reason = "disabled";
	invalidCandidate.valid = false;
	invalidCandidate.estimate.cost = 3.25;
	const auto summary = summarizeNodeAccessPath(invalidCandidate);
	EXPECT_FALSE(summary.valid);

	const auto attributes = toAccessPathAttributes(summary);
	const auto validIt = std::find_if(attributes.begin(), attributes.end(), [](const auto &attribute) {
		return attribute.first == "access_path.valid";
	});
	ASSERT_NE(validIt, attributes.end());
	EXPECT_EQ(validIt->second, "false");
}
