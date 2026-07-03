#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <utility>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/planner/NodeTopKScanPlanner.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::planner;

namespace fs = std::filesystem;

namespace {

std::shared_ptr<expressions::Expression> property(std::string variable, std::string key) {
	return std::make_shared<expressions::VariableReferenceExpression>(std::move(variable), std::move(key));
}

class ProjectWithExtraChildren final : public LogicalProject {
public:
	ProjectWithExtraChildren(std::vector<LogicalProjectItem> items) :
		LogicalProject(nullptr, std::move(items), false) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

class LimitWithExtraChildren final : public LogicalLimit {
public:
	explicit LimitWithExtraChildren(int64_t limit) : LogicalLimit(nullptr, limit) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

class SortWithExtraChildren final : public LogicalSort {
public:
	explicit SortWithExtraChildren(std::vector<LogicalSortItem> items) :
		LogicalSort(nullptr, std::move(items)) {}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {nullptr, nullptr}; }
};

std::unique_ptr<LogicalProject> makeTopKProject(std::unique_ptr<LogicalNodeScan> scan = nullptr,
                                                bool ascending = false,
                                                int64_t limit = 10) {
	if (!scan) {
		scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	}
	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(property("u", "score"), ascending);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));
	auto limited = std::make_unique<LogicalLimit>(std::move(sort), limit);
	std::vector<LogicalProjectItem> items;
	items.emplace_back(property("u", "id"), "id");
	return std::make_unique<LogicalProject>(std::move(limited), std::move(items), false);
}

} // namespace

TEST(NodeTopKScanPlannerTest, AcceptsSimpleProjectedNodePropertyTopK) {
	auto project = makeTopKProject();

	auto plan = tryBuildNodeTopKScanPlan(*project);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->config.variable, "u");
	EXPECT_EQ(plan->config.labels, (std::vector<std::string>{"User"}));
	EXPECT_EQ(plan->sortProperty, "score");
	EXPECT_FALSE(plan->ascending);
	EXPECT_EQ(plan->limit, 10);
	EXPECT_EQ(plan->requirements.materialization, execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES);
	EXPECT_EQ(plan->requirements.requiredProperties, (std::vector<std::string>{"score", "id"}));
	ASSERT_EQ(plan->projections.size(), 1U);
	EXPECT_EQ(plan->projections[0].property, "id");
	EXPECT_EQ(plan->projections[0].alias, "id");
}

TEST(NodeTopKScanPlannerTest, RejectsUnsupportedShapes) {
	auto distinctProject = makeTopKProject();
	auto child = distinctProject->detachChild(0);
	std::vector<LogicalProjectItem> items;
	items.emplace_back(property("u", "id"), "id");
	LogicalProject distinct(std::move(child), std::move(items), true);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(distinct).has_value());

	auto badProject = makeTopKProject();
	auto badLimit = static_cast<LogicalLimit *>(badProject->getChildren()[0]);
	auto badSort = static_cast<LogicalSort *>(badLimit->getChildren()[0]);
	badSort->setChild(0, std::make_unique<LogicalNodeScan>("other", std::vector<std::string>{"User"}));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(*badProject).has_value());

	auto variableScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("u"), false);
	auto sort = std::make_unique<LogicalSort>(std::move(variableScan), std::move(sortItems));
	auto limited = std::make_unique<LogicalLimit>(std::move(sort), 5);
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("u", "id"), "id");
	LogicalProject variableSort(std::move(limited), std::move(projectItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(variableSort).has_value());
}

TEST(NodeTopKScanPlannerTest, AddsResidualPredicatesAndUsesIndexes) {
	auto uuid = boost::uuids::random_generator()();
	auto path = fs::temp_directory_path() / ("test_topk_planner_" + boost::uuids::to_string(uuid) + ".dat");
	auto db = std::make_unique<Database>(path.string());
	db->open();
	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_country_topk", "node", "User", "country"));

	std::vector<std::pair<std::string, PropertyValue>> predicates = {{"country", PropertyValue("CN")}};
	auto indexedScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, predicates);
	auto indexedProject = makeTopKProject(std::move(indexedScan));
	auto indexedPlan = tryBuildNodeTopKScanPlan(*indexedProject, indexManager);
	ASSERT_TRUE(indexedPlan.has_value());
	EXPECT_EQ(indexedPlan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(indexedPlan->predicates.empty());

	std::vector<std::pair<std::string, PropertyValue>> residual = {{"country", PropertyValue("US")}};
	auto residualScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, residual);
	auto residualProject = makeTopKProject(std::move(residualScan));
	auto residualPlan = tryBuildNodeTopKScanPlan(*residualProject);
	ASSERT_TRUE(residualPlan.has_value());
	ASSERT_EQ(residualPlan->predicates.size(), 1U);
	EXPECT_EQ(residualPlan->predicates[0].propertyKey, "country");
	EXPECT_EQ(residualPlan->requirements.requiredProperties, (std::vector<std::string>{"score", "id", "country"}));

	db->close();
	db.reset();
	std::error_code ec;
	fs::remove(path, ec);
}

TEST(NodeTopKScanPlannerTest, PhysicalPlanConverterUsesTopKScanPath) {
	auto uuid = boost::uuids::random_generator()();
	auto path = fs::temp_directory_path() / ("test_topk_converter_" + boost::uuids::to_string(uuid) + ".dat");
	auto db = std::make_unique<Database>(path.string());
	db->open();
	auto dm = db->getStorage()->getDataManager();
	auto im = db->getQueryEngine()->getIndexManager();

	PhysicalPlanConverter converter(dm, im);
	auto logical = makeTopKProject();
	auto physical = converter.convert(logical.get());

	ASSERT_NE(physical, nullptr);
	EXPECT_NE(dynamic_cast<execution::operators::NodeTopKScanOperator *>(physical.get()), nullptr);

	db->close();
	db.reset();
	std::error_code ec;
	fs::remove(path, ec);
}

TEST(NodeTopKScanPlannerTest, RejectsAdditionalUnsupportedShapes) {
	std::vector<LogicalProjectItem> emptyItems;
	auto emptyItemsProject = std::make_unique<LogicalProject>(
		std::make_unique<LogicalLimit>(std::make_unique<LogicalSingleRow>(), 1), std::move(emptyItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(*emptyItemsProject).has_value());

	std::vector<LogicalProjectItem> nonSortItems;
	nonSortItems.emplace_back(property("u", "id"), "id");
	LogicalProject nonSortChild(
		std::make_unique<LogicalLimit>(std::make_unique<LogicalSingleRow>(), 1), std::move(nonSortItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(nonSortChild).has_value());

	auto scanForMultiSort = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	std::vector<LogicalSortItem> multiSortItems;
	multiSortItems.emplace_back(property("u", "score"), false);
	multiSortItems.emplace_back(property("u", "id"), true);
	auto multiSort = std::make_unique<LogicalSort>(std::move(scanForMultiSort), std::move(multiSortItems));
	auto multiLimit = std::make_unique<LogicalLimit>(std::move(multiSort), 3);
	std::vector<LogicalProjectItem> multiProjectItems;
	multiProjectItems.emplace_back(property("u", "id"), "id");
	LogicalProject multiSortProject(std::move(multiLimit), std::move(multiProjectItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(multiSortProject).has_value());

	auto singleRowSort = std::make_unique<LogicalSort>(
		std::make_unique<LogicalSingleRow>(), std::vector<LogicalSortItem>{LogicalSortItem(property("u", "score"), false)});
	auto singleRowLimit = std::make_unique<LogicalLimit>(std::move(singleRowSort), 3);
	std::vector<LogicalProjectItem> singleRowProjectItems;
	singleRowProjectItems.emplace_back(property("u", "id"), "id");
	LogicalProject singleRowProject(std::move(singleRowLimit), std::move(singleRowProjectItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(singleRowProject).has_value());

	auto literalSortScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	std::vector<LogicalSortItem> literalSortItems;
	literalSortItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{1}), false);
	auto literalSort = std::make_unique<LogicalSort>(std::move(literalSortScan), std::move(literalSortItems));
	auto literalLimit = std::make_unique<LogicalLimit>(std::move(literalSort), 3);
	std::vector<LogicalProjectItem> literalProjectItems;
	literalProjectItems.emplace_back(property("u", "id"), "id");
	LogicalProject literalSortProject(std::move(literalLimit), std::move(literalProjectItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(literalSortProject).has_value());

	auto badProjection = makeTopKProject();
	badProjection->detachChild(0);
	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(property("u", "score"), false);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));
	auto limit = std::make_unique<LogicalLimit>(std::move(sort), 3);
	std::vector<LogicalProjectItem> badItems;
	badItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("u"), "u");
	LogicalProject badProject(std::move(limit), std::move(badItems));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(badProject).has_value());
}

TEST(NodeTopKScanPlannerTest, BuildsRangeAndCompositeIndexPlans) {
	auto uuid = boost::uuids::random_generator()();
	auto path = fs::temp_directory_path() / ("test_topk_index_shapes_" + boost::uuids::to_string(uuid) + ".dat");
	auto db = std::make_unique<Database>(path.string());
	db->open();
	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_age_topk", "node", "User", "age"));
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_country_age_topk", "node", "User", {"country", "age"}));

	auto rangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	range.minInclusive = true;
	range.maxInclusive = true;
	rangeScan->setRangePredicates({range});
	auto rangeProject = makeTopKProject(std::move(rangeScan));
	auto rangePlan = tryBuildNodeTopKScanPlan(*rangeProject, indexManager);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->predicates.empty());

	auto compositeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	compositeScan->setCompositeEquality(std::move(composite));
	auto compositeProject = makeTopKProject(std::move(compositeScan));
	auto compositePlan = tryBuildNodeTopKScanPlan(*compositeProject, indexManager);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->predicates.empty());

	db->close();
	db.reset();
	std::error_code ec;
	fs::remove(path, ec);
}

TEST(NodeTopKScanPlannerTest, AddsResidualRangeAndCompositePredicates) {
	auto closedRangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate closedRange;
	closedRange.key = "age";
	closedRange.minValue = PropertyValue(int64_t{18});
	closedRange.maxValue = PropertyValue(int64_t{65});
	closedRangeScan->setRangePredicates({closedRange});
	auto closedRangeProject = makeTopKProject(std::move(closedRangeScan));
	auto closedRangePlan = tryBuildNodeTopKScanPlan(*closedRangeProject);
	ASSERT_TRUE(closedRangePlan.has_value());
	ASSERT_EQ(closedRangePlan->predicates.size(), 1U);
	EXPECT_EQ(closedRangePlan->predicates[0].op, execution::VectorPredicateOp::VPO_RANGE_CLOSED);

	auto lowerRangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate lowerRange;
	lowerRange.key = "age";
	lowerRange.minValue = PropertyValue(int64_t{18});
	lowerRange.minInclusive = true;
	lowerRangeScan->setRangePredicates({lowerRange});
	auto lowerRangeProject = makeTopKProject(std::move(lowerRangeScan));
	auto lowerRangePlan = tryBuildNodeTopKScanPlan(*lowerRangeProject);
	ASSERT_TRUE(lowerRangePlan.has_value());
	ASSERT_EQ(lowerRangePlan->predicates.size(), 1U);
	EXPECT_EQ(lowerRangePlan->predicates[0].op, execution::VectorPredicateOp::VPO_GE);

	auto upperRangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate upperRange;
	upperRange.key = "age";
	upperRange.maxValue = PropertyValue(int64_t{65});
	upperRange.maxInclusive = false;
	upperRangeScan->setRangePredicates({upperRange});
	auto upperRangeProject = makeTopKProject(std::move(upperRangeScan));
	auto upperRangePlan = tryBuildNodeTopKScanPlan(*upperRangeProject);
	ASSERT_TRUE(upperRangePlan.has_value());
	ASSERT_EQ(upperRangePlan->predicates.size(), 1U);
	EXPECT_EQ(upperRangePlan->predicates[0].op, execution::VectorPredicateOp::VPO_LT);

	auto emptyRangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate emptyRange;
	emptyRange.key = "age";
	emptyRangeScan->setRangePredicates({emptyRange});
	auto emptyRangeProject = makeTopKProject(std::move(emptyRangeScan));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(*emptyRangeProject).has_value());

	auto compositeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	compositeScan->setCompositeEquality(std::move(composite));
	auto compositeProject = makeTopKProject(std::move(compositeScan));
	auto compositePlan = tryBuildNodeTopKScanPlan(*compositeProject);
	ASSERT_TRUE(compositePlan.has_value());
	ASSERT_EQ(compositePlan->predicates.size(), 2U);
	EXPECT_EQ(compositePlan->predicates[0].propertyKey, "country");
	EXPECT_EQ(compositePlan->predicates[1].propertyKey, "age");

	auto malformedScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	CompositeEqualityPredicate malformed;
	malformed.keys = {"country", "age"};
	malformed.values = {PropertyValue("CN")};
	malformedScan->setCompositeEquality(std::move(malformed));
	auto malformedProject = makeTopKProject(std::move(malformedScan));
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(*malformedProject).has_value());
}

TEST(NodeTopKScanPlannerTest, FallsBackForInvalidPreferredScanConfig) {
	auto propertyScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	propertyScan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	auto propertyProject = makeTopKProject(std::move(propertyScan));
	auto propertyPlan = tryBuildNodeTopKScanPlan(*propertyProject);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->config.type, execution::ScanType::LABEL_SCAN);

	auto unlabeledRangeScan = std::make_unique<LogicalNodeScan>("u");
	unlabeledRangeScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	auto unlabeledRangeProject = makeTopKProject(std::move(unlabeledRangeScan));
	auto unlabeledRangePlan = tryBuildNodeTopKScanPlan(*unlabeledRangeProject);
	ASSERT_TRUE(unlabeledRangePlan.has_value());
	EXPECT_EQ(unlabeledRangePlan->config.type, execution::ScanType::FULL_SCAN);

	auto compositeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	compositeScan->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	auto compositeProject = makeTopKProject(std::move(compositeScan));
	auto compositePlan = tryBuildNodeTopKScanPlan(*compositeProject);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::LABEL_SCAN);
}

TEST(NodeTopKScanPlannerTest, HandlesPreferredScanConfigsWithoutIndexManager) {
	std::vector<std::pair<std::string, PropertyValue>> propertyPredicates = {{"country", PropertyValue("CN")}};
	auto propertyScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, propertyPredicates);
	propertyScan->setPreferredScanType(execution::ScanType::PROPERTY_SCAN);
	auto propertyProject = makeTopKProject(std::move(propertyScan));
	auto propertyPlan = tryBuildNodeTopKScanPlan(*propertyProject);
	ASSERT_TRUE(propertyPlan.has_value());
	EXPECT_EQ(propertyPlan->config.type, execution::ScanType::PROPERTY_SCAN);
	EXPECT_TRUE(propertyPlan->predicates.empty());

	auto rangeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate range;
	range.key = "age";
	range.minValue = PropertyValue(int64_t{18});
	range.maxValue = PropertyValue(int64_t{65});
	rangeScan->setRangePredicates({range});
	rangeScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	auto rangeProject = makeTopKProject(std::move(rangeScan));
	auto rangePlan = tryBuildNodeTopKScanPlan(*rangeProject);
	ASSERT_TRUE(rangePlan.has_value());
	EXPECT_EQ(rangePlan->config.type, execution::ScanType::RANGE_SCAN);
	EXPECT_TRUE(rangePlan->predicates.empty());

	auto compositeScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "age"};
	composite.values = {PropertyValue("CN"), PropertyValue(int64_t{42})};
	compositeScan->setCompositeEquality(std::move(composite));
	compositeScan->setPreferredScanType(execution::ScanType::COMPOSITE_SCAN);
	auto compositeProject = makeTopKProject(std::move(compositeScan));
	auto compositePlan = tryBuildNodeTopKScanPlan(*compositeProject);
	ASSERT_TRUE(compositePlan.has_value());
	EXPECT_EQ(compositePlan->config.type, execution::ScanType::COMPOSITE_SCAN);
	EXPECT_TRUE(compositePlan->predicates.empty());

	auto labelScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	labelScan->setPreferredScanType(execution::ScanType::LABEL_SCAN);
	auto labelProject = makeTopKProject(std::move(labelScan));
	auto labelPlan = tryBuildNodeTopKScanPlan(*labelProject);
	ASSERT_TRUE(labelPlan.has_value());
	EXPECT_EQ(labelPlan->config.type, execution::ScanType::LABEL_SCAN);
}

TEST(NodeTopKScanPlannerTest, HandlesOpenRangeFallbackAndUpperInclusivePredicate) {
	auto minOpenScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate minOnly;
	minOnly.key = "age";
	minOnly.minValue = PropertyValue(int64_t{18});
	minOpenScan->setRangePredicates({minOnly});
	minOpenScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	auto minOpenProject = makeTopKProject(std::move(minOpenScan));
	auto minOpenPlan = tryBuildNodeTopKScanPlan(*minOpenProject);
	ASSERT_TRUE(minOpenPlan.has_value());
	EXPECT_EQ(minOpenPlan->config.type, execution::ScanType::LABEL_SCAN);

	auto maxOpenScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate maxOnly;
	maxOnly.key = "age";
	maxOnly.maxValue = PropertyValue(int64_t{65});
	maxOnly.maxInclusive = true;
	maxOpenScan->setRangePredicates({maxOnly});
	maxOpenScan->setPreferredScanType(execution::ScanType::RANGE_SCAN);
	auto maxOpenProject = makeTopKProject(std::move(maxOpenScan));
	auto maxOpenPlan = tryBuildNodeTopKScanPlan(*maxOpenProject);
	ASSERT_TRUE(maxOpenPlan.has_value());
	ASSERT_EQ(maxOpenPlan->predicates.size(), 1U);
	EXPECT_EQ(maxOpenPlan->predicates[0].op, execution::VectorPredicateOp::VPO_LE);
}

TEST(NodeTopKScanPlannerTest, HandlesIndexSelectionFallbackBranches) {
	auto uuid = boost::uuids::random_generator()();
	auto path = fs::temp_directory_path() / ("test_topk_fallback_shapes_" + boost::uuids::to_string(uuid) + ".dat");
	auto db = std::make_unique<Database>(path.string());
	db->open();
	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("idx_age_topk_fallback", "node", "User", "age"));

	auto compositeNoIndexScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	CompositeEqualityPredicate composite;
	composite.keys = {"country", "city"};
	composite.values = {PropertyValue("CN"), PropertyValue("SH")};
	compositeNoIndexScan->setCompositeEquality(std::move(composite));
	auto compositeNoIndexProject = makeTopKProject(std::move(compositeNoIndexScan));
	auto compositeNoIndexPlan = tryBuildNodeTopKScanPlan(*compositeNoIndexProject, indexManager);
	ASSERT_TRUE(compositeNoIndexPlan.has_value());
	EXPECT_EQ(compositeNoIndexPlan->config.type, execution::ScanType::FULL_SCAN);
	EXPECT_EQ(compositeNoIndexPlan->predicates.size(), 2U);

	std::vector<std::pair<std::string, PropertyValue>> propertyPredicates = {{"country", PropertyValue("CN")}};
	auto unindexedPropertyScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"}, propertyPredicates);
	auto unindexedPropertyProject = makeTopKProject(std::move(unindexedPropertyScan));
	auto unindexedPropertyPlan = tryBuildNodeTopKScanPlan(*unindexedPropertyProject, indexManager);
	ASSERT_TRUE(unindexedPropertyPlan.has_value());
	EXPECT_EQ(unindexedPropertyPlan->config.type, execution::ScanType::FULL_SCAN);
	ASSERT_EQ(unindexedPropertyPlan->predicates.size(), 1U);
	EXPECT_EQ(unindexedPropertyPlan->predicates[0].propertyKey, "country");

	auto openRangeWithIndexScan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});
	RangePredicate openRange;
	openRange.key = "age";
	openRange.minValue = PropertyValue(int64_t{18});
	openRangeWithIndexScan->setRangePredicates({openRange});
	auto openRangeWithIndexProject = makeTopKProject(std::move(openRangeWithIndexScan));
	auto openRangeWithIndexPlan = tryBuildNodeTopKScanPlan(*openRangeWithIndexProject, indexManager);
	ASSERT_TRUE(openRangeWithIndexPlan.has_value());
	EXPECT_EQ(openRangeWithIndexPlan->config.type, execution::ScanType::FULL_SCAN);

	auto unlabeledScan = std::make_unique<LogicalNodeScan>("u");
	auto unlabeledProject = makeTopKProject(std::move(unlabeledScan));
	auto unlabeledPlan = tryBuildNodeTopKScanPlan(*unlabeledProject, indexManager);
	ASSERT_TRUE(unlabeledPlan.has_value());
	EXPECT_EQ(unlabeledPlan->config.type, execution::ScanType::FULL_SCAN);

	db->close();
	db.reset();
	std::error_code ec;
	fs::remove(path, ec);
}

TEST(NodeTopKScanPlannerTest, RejectsNullLogicalChildren) {
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("u", "id"), "id");
	LogicalProject nullProjectChild(nullptr, projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(nullProjectChild).has_value());

	auto nullLimit = std::make_unique<LogicalLimit>(nullptr, 1);
	LogicalProject nullLimitChild(std::move(nullLimit), projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(nullLimitChild).has_value());

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(property("u", "score"), false);
	auto nullSort = std::make_unique<LogicalSort>(nullptr, sortItems);
	auto limit = std::make_unique<LogicalLimit>(std::move(nullSort), 1);
	LogicalProject nullSortChild(std::move(limit), projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(nullSortChild).has_value());
}

TEST(NodeTopKScanPlannerTest, RejectsMalformedLogicalChildArity) {
	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(property("u", "id"), "id");
	ProjectWithExtraChildren extraProjectChildren(projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(extraProjectChildren).has_value());

	auto extraLimit = std::make_unique<LimitWithExtraChildren>(1);
	LogicalProject projectWithBadLimit(std::move(extraLimit), projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(projectWithBadLimit).has_value());

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(property("u", "score"), false);
	auto extraSort = std::make_unique<SortWithExtraChildren>(sortItems);
	auto limit = std::make_unique<LogicalLimit>(std::move(extraSort), 1);
	LogicalProject projectWithBadSort(std::move(limit), projectItems);
	EXPECT_FALSE(tryBuildNodeTopKScanPlan(projectWithBadSort).has_value());
}
