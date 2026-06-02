#include "DataManagerTestFixture.hpp"

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/StorageHeaders.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "src/storage/data/DataManagerPropertyEntityScanDetail.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int64_t addNodeWithPropertyEntity(
		const std::shared_ptr<DataManager> &dataManager,
		const std::unordered_map<std::string, PropertyValue> &properties) {
	Node node;
	node.setLabelId(dataManager->getOrCreateTokenId("PropertyEntityPredicateNode"));
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), properties);
	const Node stored = dataManager->getNode(node.getId());
	return stored.hasPropertyEntity() ? stored.getPropertyEntityId() : int64_t{0};
}

PropertyEntityPredicate pred(const std::string &key, PropertyEntityPredicateOp op, PropertyValue value) {
	PropertyEntityPredicate predicate;
	predicate.key = key;
	predicate.op = op;
	predicate.value = std::move(value);
	return predicate;
}

PropertyEntityPredicate rangePred(const std::string &key, PropertyValue lower, PropertyValue upper) {
	auto predicate = pred(key, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, std::move(lower));
	predicate.upperValue = std::move(upper);
	return predicate;
}

} // namespace

TEST_F(DataManagerTest, BulkVisitPropertyEntityScalarValuesReportsTypedAndFallbackValues) {
	std::vector<PropertyValue> listValue{PropertyValue(int64_t{1}), PropertyValue("two")};
	PropertyValue::MapType mapValue;
	mapValue.emplace("inner", PropertyValue(int64_t{9}));
	const auto dateValue = TemporalDate::fromYMD(2026, 6, 2);
	const auto dateTimeValue = TemporalDateTime::fromComponents(2026, 6, 2, 12, 34, 56, 789);
	const auto durationValue = TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6);
	const int64_t propertyId = addNodeWithPropertyEntity(
			dataManager,
			{{"nothing", PropertyValue(std::monostate{})},
			 {"flag", PropertyValue(true)},
			 {"age", PropertyValue(int64_t{42})},
			 {"score", PropertyValue(9.5)},
			 {"name", PropertyValue("alice")},
			 {"date", PropertyValue(dateValue)},
			 {"datetime", PropertyValue(dateTimeValue)},
			 {"duration", PropertyValue(durationValue)},
			 {"items", PropertyValue(listValue)},
			 {"meta", PropertyValue(mapValue)}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::map<std::string, PropertyType> seenTypes;
	std::string seenName;
	bool seenFlag = false;
	int64_t seenAge = 0;
	double seenScore = 0.0;
	int64_t seenDateDays = 0;
	int64_t seenDateTimeMillis = 0;
	TemporalDuration seenDuration{};
	bool sawListFallback = false;
	bool sawMapFallback = false;
	auto visitKey = [&](const std::string &key) {
		return dataManager->bulkVisitPropertyEntityScalarValues(
				{propertyId, propertyId}, {0, 1}, 2, key,
				[&](size_t row, const PropertyEntityScalarValue &value) {
					if (row != 0) {
						return;
					}
					seenTypes[key] = value.type;
					if (key == "flag") {
						seenFlag = value.boolValue;
					} else if (key == "age") {
						seenAge = value.intValue;
					} else if (key == "score") {
						seenScore = value.doubleValue;
					} else if (key == "name") {
						seenName.assign(value.stringValue);
					} else if (key == "date") {
						seenDateDays = value.intValue;
					} else if (key == "datetime") {
						seenDateTimeMillis = value.intValue;
					} else if (key == "duration") {
						seenDuration = value.durationValue;
					} else if (key == "items") {
						sawListFallback = value.fallbackValue != nullptr && *value.fallbackValue == PropertyValue(listValue);
					} else if (key == "meta") {
						sawMapFallback = value.fallbackValue != nullptr && *value.fallbackValue == PropertyValue(mapValue);
					}
				},
				nullptr);
	};

	EXPECT_EQ(visitKey("nothing"), 2U);
	EXPECT_EQ(visitKey("flag"), 2U);
	EXPECT_EQ(visitKey("age"), 2U);
	EXPECT_EQ(visitKey("score"), 2U);
	EXPECT_EQ(visitKey("name"), 2U);
	EXPECT_EQ(visitKey("date"), 2U);
	EXPECT_EQ(visitKey("datetime"), 2U);
	EXPECT_EQ(visitKey("duration"), 2U);
	EXPECT_EQ(visitKey("items"), 2U);
	EXPECT_EQ(visitKey("meta"), 2U);
	EXPECT_EQ(visitKey("missing"), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({}, {}, 1, "age", [](size_t, const auto &) {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {}, 1, "age", [](size_t, const auto &) {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 0, "age", [](size_t, const auto &) {}, nullptr), 0U);

	EXPECT_EQ(seenTypes.at("nothing"), PropertyType::NULL_TYPE);
	EXPECT_TRUE(seenFlag);
	EXPECT_EQ(seenAge, 42);
	EXPECT_EQ(seenScore, 9.5);
	EXPECT_EQ(seenName, "alice");
	EXPECT_EQ(seenTypes.at("date"), PropertyType::DATE);
	EXPECT_EQ(seenDateDays, dateValue.epochDays);
	EXPECT_EQ(seenTypes.at("datetime"), PropertyType::DATETIME);
	EXPECT_EQ(seenDateTimeMillis, dateTimeValue.epochMillis);
	EXPECT_EQ(seenDuration.months, durationValue.months);
	EXPECT_EQ(seenDuration.days, durationValue.days);
	EXPECT_EQ(seenDuration.nanos, durationValue.nanos);
	EXPECT_TRUE(sawListFallback);
	EXPECT_TRUE(sawMapFallback);
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicateSpecsEvaluateAllComparisonOperators) {
	const auto dateValue = TemporalDate::fromYMD(2026, 6, 2);
	const auto durationValue = TemporalDuration::fromComponents(0, 1, 0, 0, 0, 0, 0);
	std::vector<PropertyValue> listValue{PropertyValue(int64_t{1}), PropertyValue("two")};
	PropertyValue::MapType mapValue;
	mapValue.emplace("inner", PropertyValue(int64_t{9}));
	const int64_t propertyId = addNodeWithPropertyEntity(
			dataManager,
			{{"flag", PropertyValue(true)},
			 {"age", PropertyValue(int64_t{42})},
			 {"score", PropertyValue(9.5)},
			 {"name", PropertyValue("milo")},
			 {"date", PropertyValue(dateValue)},
			 {"duration", PropertyValue(durationValue)},
			 {"items", PropertyValue(listValue)},
			 {"meta", PropertyValue(mapValue)}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	auto count = [&](std::vector<PropertyEntityPredicate> predicates) {
		return dataManager->bulkCountPropertyEntityPredicateSpecs({propertyId, 0, propertyId}, predicates, nullptr);
	};

	EXPECT_EQ(count({pred("flag", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(true))}), 2U);
	EXPECT_EQ(count({pred("flag", PropertyEntityPredicateOp::PEP_NE, PropertyValue(false))}), 2U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_LT, PropertyValue(int64_t{43}))}), 2U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_LE, PropertyValue(int64_t{42}))}), 2U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_GT, PropertyValue(int64_t{41}))}), 2U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{42}))}), 2U);
	EXPECT_EQ(count({rangePred("age", PropertyValue(int64_t{40}), PropertyValue(int64_t{42}))}), 2U);
	EXPECT_EQ(count({pred("score", PropertyEntityPredicateOp::PEP_GT, PropertyValue(9.0))}), 2U);
	EXPECT_EQ(count({rangePred("score", PropertyValue(9.5), PropertyValue(10.0))}), 2U);
	EXPECT_EQ(count({pred("name", PropertyEntityPredicateOp::PEP_GT, PropertyValue("a"))}), 2U);
	EXPECT_EQ(count({pred("name", PropertyEntityPredicateOp::PEP_LE, PropertyValue("milo"))}), 2U);
	EXPECT_EQ(count({rangePred("name", PropertyValue("aa"), PropertyValue("zz"))}), 2U);
	EXPECT_EQ(count({pred("date", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(dateValue))}), 2U);
	EXPECT_EQ(count({pred("duration", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(durationValue))}), 2U);
	EXPECT_EQ(count({pred("items", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(listValue))}), 2U);
	EXPECT_EQ(count({pred("meta", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(mapValue))}), 2U);

	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_NE, PropertyValue(int64_t{42}))}), 0U);
	EXPECT_EQ(count({pred("name", PropertyEntityPredicateOp::PEP_LT, PropertyValue("a"))}), 0U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_RANGE_CLOSED, PropertyValue(int64_t{40}))}), 0U);
	EXPECT_EQ(count({rangePred("name", PropertyValue("n"), PropertyValue("z"))}), 0U);
	EXPECT_EQ(count({pred("missing", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))}), 0U);
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{40})), pred("age", PropertyEntityPredicateOp::PEP_LE, PropertyValue(int64_t{45}))}), 2U);
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsHonorRowsAndOptions) {
	const int64_t firstId = addNodeWithPropertyEntity(
			dataManager,
			{{"age", PropertyValue(int64_t{42})}, {"name", PropertyValue("milo")}});
	const int64_t secondId = addNodeWithPropertyEntity(
			dataManager,
			{{"age", PropertyValue(int64_t{18})}, {"name", PropertyValue("zoe")}});
	ASSERT_NE(firstId, 0);
	ASSERT_NE(secondId, 0);

	simulateSave();
	dataManager->clearCache();

	std::vector<PropertyEntityPredicate> predicates{
			pred("age", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{40})),
			pred("age", PropertyEntityPredicateOp::PEP_LE, PropertyValue(int64_t{50})),
			pred("name", PropertyEntityPredicateOp::PEP_NE, PropertyValue("zoe"))};
	auto result = dataManager->bulkMatchPropertyEntityPredicateSpecs(
			{secondId, firstId, 0, firstId}, {2, 0, 1, 3}, 4, predicates, nullptr);
	std::sort(result.loadedRows.begin(), result.loadedRows.end());
	std::sort(result.matchedRows.begin(), result.matchedRows.end());
	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{0U, 2U, 3U}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{0U, 3U}));
	EXPECT_EQ(result.loadedCount, 3U);
	EXPECT_EQ(result.matchedCount, 2U);

	PropertyEntityPredicateMatchOptions noRows;
	noRows.collectLoadedRows = false;
	noRows.collectMatchedRows = false;
	auto countOnly = dataManager->bulkMatchPropertyEntityPredicateSpecs(
			{firstId, secondId}, {0, 1}, 2, predicates, nullptr, noRows);
	EXPECT_TRUE(countOnly.loadedRows.empty());
	EXPECT_TRUE(countOnly.matchedRows.empty());
	EXPECT_EQ(countOnly.loadedCount, 2U);
	EXPECT_EQ(countOnly.matchedCount, 1U);
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"name", PropertyValue("node-" + std::to_string(i))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));
	ASSERT_NE(propertyIds.front(), 0);
	ASSERT_NE(propertyIds.back(), 0);

	simulateSave();
	dataManager->clearCache();

	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
										   propertyIds[PROPERTIES_PER_SEGMENT]};
	const std::vector<size_t> rows{2, 0, 1};
	std::map<size_t, int64_t> valuesByRow;
	const size_t visited = dataManager->bulkVisitPropertyEntityValues(
			selectedIds, rows, 3, "rank",
			[&](size_t row, const PropertyValue &value) {
				valuesByRow[row] = std::get<int64_t>(value.getVariant());
			},
			nullptr);

	EXPECT_EQ(visited, 3U);
	EXPECT_EQ(valuesByRow[0], 0);
	EXPECT_EQ(valuesByRow[1], static_cast<int64_t>(PROPERTIES_PER_SEGMENT));
	EXPECT_EQ(valuesByRow[2], static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 1));
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityScalarValuesScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"name", PropertyValue("node-" + std::to_string(i))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));

	simulateSave();
	dataManager->clearCache();

	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT], propertyIds[1],
										   propertyIds[PROPERTIES_PER_SEGMENT + 1]};
	const std::vector<size_t> rows{1, 0, 2};
	std::map<size_t, int64_t> valuesByRow;
	const size_t visited = dataManager->bulkVisitPropertyEntityScalarValues(
			selectedIds, rows, 3, "rank",
			[&](size_t row, const PropertyEntityScalarValue &value) {
				ASSERT_EQ(value.type, PropertyType::INTEGER);
				valuesByRow[row] = value.intValue;
			},
			nullptr);

	EXPECT_EQ(visited, 3U);
	EXPECT_EQ(valuesByRow[0], 1);
	EXPECT_EQ(valuesByRow[1], static_cast<int64_t>(PROPERTIES_PER_SEGMENT));
	EXPECT_EQ(valuesByRow[2], static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 1));
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesRejectsInvalidReferences) {
	const int64_t propertyId = addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(int64_t{7})}});
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	auto valueVisitor = [](size_t, const PropertyValue &) {};
	auto scalarVisitor = [](size_t, const PropertyEntityScalarValue &) {};
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({}, {}, 1, "rank", valueVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({propertyId}, {}, 1, "rank", valueVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({propertyId}, {0}, 0, "rank", valueVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({propertyId}, {0}, 1, "", valueVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({propertyId}, {0}, 1, "rank", {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({0, propertyId}, {0, 9}, 1, "rank", valueVisitor, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({propertyId + 10000}, {0}, 1, "rank", valueVisitor, nullptr),
			  0U);

	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({}, {}, 1, "rank", scalarVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {}, 1, "rank", scalarVisitor, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 0, "rank", scalarVisitor, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 1, "", scalarVisitor, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 1, "rank", {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({0, propertyId}, {0, 9}, 1, "rank", scalarVisitor,
															   nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId + 10000}, {0}, 1, "rank", scalarVisitor,
															   nullptr),
			  0U);
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesParallelReadsCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"name", PropertyValue("node-" + std::to_string(i))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(2);
	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
										   propertyIds[PROPERTIES_PER_SEGMENT]};
	auto loaded = dataManager->bulkLoadPropertyEntities(selectedIds, &pool);

	ASSERT_EQ(loaded.size(), 3U);
	EXPECT_EQ(std::get<int64_t>(loaded.at(propertyIds[0]).getPropertyValues().at("rank").getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(loaded.at(propertyIds[PROPERTIES_PER_SEGMENT]).getPropertyValues().at("rank").getVariant()),
			  static_cast<int64_t>(PROPERTIES_PER_SEGMENT));
	EXPECT_EQ(std::get<int64_t>(
					  loaded.at(propertyIds[PROPERTIES_PER_SEGMENT + 1]).getPropertyValues().at("rank").getVariant()),
			  static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 1));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsParallelScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(2);
	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
										   propertyIds[PROPERTIES_PER_SEGMENT]};
	const std::vector<size_t> rows{2, 0, 1};
	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{0})),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{0}))};
	auto result = dataManager->bulkMatchPropertyEntityPredicateSpecs(selectedIds, rows, 3, predicates, &pool);
	std::sort(result.loadedRows.begin(), result.loadedRows.end());
	std::sort(result.matchedRows.begin(), result.matchedRows.end());

	EXPECT_EQ(result.loadedCount, 3U);
	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{0U, 1U, 2U}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_EQ(result.matchedCount, 2U);
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicateSpecsParallelScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(2);
	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
										   propertyIds[PROPERTIES_PER_SEGMENT]};
	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{0})),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{0}))};

	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs(selectedIds, predicates, &pool), 2U);
}

TEST_F(DataManagerTest, PropertyEntityMatcherHandlesUnsortedDuplicatesAndParallelWork) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager,
				{{"rank", PropertyValue(static_cast<int64_t>(i))}, {"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));
	simulateSave();
	dataManager->clearCache();

	const std::string rankKey = "rank";
	const PropertyValue lower(int64_t{0});
	const PropertyValue upper(static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 2));
	std::vector<PropertyEntityPredicate> predicates{rangePred(rankKey, lower, upper)};
	std::vector<PredicateSpecExpectation> specs{
			{&predicates.front().key, &predicates.front().value, &predicates.front().upperValue.value(),
			 predicates.front().op}};
	const auto groups = groupPredicateSpecExpectations(specs);
	auto matcher = [&](const char *buf) {
		return readPropertyEntityPredicateMatch(buf, groups, specs.size());
	};

	const std::vector<int64_t> unsortedWithDuplicates{
			0, propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0], propertyIds[PROPERTIES_PER_SEGMENT + 1]};
	EXPECT_EQ(countPropertyEntityMatches(*dataManager, unsortedWithDuplicates, nullptr, matcher), 3U);
	graph::concurrent::ThreadPool pool(2);
	EXPECT_EQ(countPropertyEntityMatches(*dataManager, unsortedWithDuplicates, &pool, matcher), 3U);
	EXPECT_EQ(countPropertyEntityMatches(*dataManager, std::vector<int64_t>{0, 0}, nullptr, matcher), 0U);
	EXPECT_EQ(countPropertyEntityMatches(*dataManager, std::vector<int64_t>{propertyIds.back() + 10000}, nullptr, matcher),
			  0U);
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsRejectsInvalidInputs) {
	const int64_t propertyId =
			addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(int64_t{7})}});
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{7}))};
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates({propertyId}, {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs({propertyId}, {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates({}, {{"rank", PropertyValue(int64_t{7})}}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs({}, predicates, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs({0, 0}, predicates, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs({propertyId + 10000}, predicates, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({}, {}, 1, predicates, nullptr).loadedCount, 0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({propertyId}, {}, 1, predicates, nullptr).loadedCount,
			  0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({propertyId}, {0}, 0, predicates, nullptr).loadedCount,
			  0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({propertyId}, {0}, 1, {}, nullptr).loadedCount, 0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({0, propertyId}, {0, 2}, 1, predicates, nullptr)
					  .loadedCount,
			  0U);
	EXPECT_EQ(dataManager->bulkMatchPropertyEntityPredicateSpecs({propertyId + 10000}, {0}, 1, predicates, nullptr)
					  .loadedCount,
			  0U);
}
