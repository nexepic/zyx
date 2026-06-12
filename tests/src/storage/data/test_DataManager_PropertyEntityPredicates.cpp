#include "DataManagerTestFixture.hpp"

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
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

	constexpr size_t kParallelPropertyReadSegmentsForDecisionTests =
			(graph::concurrent::kDefaultMemoryScanBytesPerWorker / TOTAL_SEGMENT_SIZE) + 1;

	int64_t addNodeWithPropertyEntity(const std::shared_ptr<DataManager> &dataManager,
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
	const int64_t propertyId = addNodeWithPropertyEntity(dataManager, {{"nothing", PropertyValue(std::monostate{})},
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
						sawListFallback =
								value.fallbackValue != nullptr && *value.fallbackValue == PropertyValue(listValue);
					} else if (key == "meta") {
						sawMapFallback =
								value.fallbackValue != nullptr && *value.fallbackValue == PropertyValue(mapValue);
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
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues(
					  {}, {}, 1, "age", [](size_t, const auto &) {}, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues(
					  {propertyId}, {}, 1, "age", [](size_t, const auto &) {}, nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues(
					  {propertyId}, {0}, 0, "age", [](size_t, const auto &) {}, nullptr),
			  0U);

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

TEST_F(DataManagerTest, BulkVisitPropertyEntityScalarValuesPartitionedScansIndependentChunks) {
	Node owner = createTestNode(dataManager, "PartitionedScalarOwner");
	dataManager->addNode(owner);

	const size_t propertyCount =
			static_cast<size_t>(PROPERTIES_PER_SEGMENT) * kParallelPropertyReadSegmentsForDecisionTests;
	std::vector<int64_t> propertyIds;
	std::vector<size_t> rows;
	propertyIds.reserve(propertyCount);
	rows.reserve(propertyCount);
	for (size_t i = 0; i < propertyCount; ++i) {
		auto property =
				createTestProperty(owner.getId(), Node::typeId, {{"rank", PropertyValue(static_cast<int64_t>(i))}});
		dataManager->addPropertyEntity(property);
		propertyIds.push_back(property.getId());
		rows.push_back(i);
	}

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(4);
	size_t initializedPartitions = 0;
	std::vector<size_t> partitionVisits;
	std::vector<int64_t> partitionSums;
	const size_t visited = dataManager->bulkVisitPropertyEntityScalarValuesPartitioned(
			propertyIds, rows, propertyCount, "rank",
			[&](size_t partitionCount) {
				initializedPartitions = partitionCount;
				partitionVisits.assign(partitionCount, 0);
				partitionSums.assign(partitionCount, 0);
			},
			[&](size_t partition, size_t, const PropertyEntityScalarValue &value) {
				if (partition >= partitionVisits.size() || value.type != PropertyType::INTEGER) {
					return;
				}
				++partitionVisits[partition];
				partitionSums[partition] += value.intValue;
			},
			&pool);

	EXPECT_GT(initializedPartitions, 1U);
	EXPECT_EQ(visited, propertyCount);
	size_t totalVisits = 0;
	int64_t totalSum = 0;
	for (size_t partition = 0; partition < initializedPartitions; ++partition) {
		totalVisits += partitionVisits[partition];
		totalSum += partitionSums[partition];
	}
	EXPECT_EQ(totalVisits, propertyCount);
	EXPECT_EQ(totalSum, static_cast<int64_t>((propertyCount - 1) * propertyCount / 2));
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicateSpecsEvaluateAllComparisonOperators) {
	const auto dateValue = TemporalDate::fromYMD(2026, 6, 2);
	const auto durationValue = TemporalDuration::fromComponents(0, 1, 0, 0, 0, 0, 0);
	std::vector<PropertyValue> listValue{PropertyValue(int64_t{1}), PropertyValue("two")};
	PropertyValue::MapType mapValue;
	mapValue.emplace("inner", PropertyValue(int64_t{9}));
	const int64_t propertyId = addNodeWithPropertyEntity(dataManager, {{"flag", PropertyValue(true)},
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
	EXPECT_EQ(count({pred("age", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{40})),
					 pred("age", PropertyEntityPredicateOp::PEP_LE, PropertyValue(int64_t{45}))}),
			  2U);
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicateSpecsByOwnerTypeParallelMatchesSequential) {
	const size_t propertyCount =
			static_cast<size_t>(PROPERTIES_PER_SEGMENT) * kParallelPropertyReadSegmentsForDecisionTests;
	size_t expectedLoaded = 0;
	size_t expectedMatched = 0;
	for (size_t i = 0; i < propertyCount; ++i) {
		Node node = createTestNode(dataManager, "ParallelPredicateCountNode");
		dataManager->addNode(node);
		auto property = createTestProperty(node.getId(), Node::typeId,
										   {{"rank", PropertyValue(static_cast<int64_t>(i % 10))},
											{"bucket", PropertyValue(static_cast<int64_t>(i % 3))}});
		dataManager->addPropertyEntity(property);
		++expectedLoaded;
		if (i % 10 >= 8 && i % 3 == 1) {
			++expectedMatched;
		}
	}

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{8})),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))};
	const auto sequential =
			dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Node, predicates, nullptr);

	graph::concurrent::ThreadPool pool(4);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto parallel =
			dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Node, predicates, &pool);
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(sequential.loadedCount, expectedLoaded);
	EXPECT_EQ(sequential.matchedCount, expectedMatched);
	EXPECT_EQ(parallel.loadedCount, sequential.loadedCount);
	EXPECT_EQ(parallel.matchedCount, sequential.matchedCount);
	EXPECT_TRUE(snapshot.contains("property_predicate_count.parallel"));
	EXPECT_TRUE(snapshot.contains("property_predicate_count.parallel.task"));
	EXPECT_TRUE(snapshot.contains("property_predicate_count.parallel.merge"));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsHonorRowsAndOptions) {
	const int64_t firstId = addNodeWithPropertyEntity(
			dataManager, {{"age", PropertyValue(int64_t{42})}, {"name", PropertyValue("milo")}});
	const int64_t secondId = addNodeWithPropertyEntity(
			dataManager, {{"age", PropertyValue(int64_t{18})}, {"name", PropertyValue("zoe")}});
	ASSERT_NE(firstId, 0);
	ASSERT_NE(secondId, 0);

	simulateSave();
	dataManager->clearCache();

	std::vector<PropertyEntityPredicate> predicates{
			pred("age", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{40})),
			pred("age", PropertyEntityPredicateOp::PEP_LE, PropertyValue(int64_t{50})),
			pred("name", PropertyEntityPredicateOp::PEP_NE, PropertyValue("zoe"))};
	auto result = dataManager->bulkMatchPropertyEntityPredicateSpecs({secondId, firstId, 0, firstId}, {2, 0, 1, 3}, 4,
																	 predicates, nullptr);
	std::sort(result.loadedRows.begin(), result.loadedRows.end());
	std::sort(result.matchedRows.begin(), result.matchedRows.end());
	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{0U, 2U, 3U}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{0U, 3U}));
	EXPECT_EQ(result.loadedCount, 3U);
	EXPECT_EQ(result.matchedCount, 2U);

	PropertyEntityPredicateMatchOptions noRows;
	noRows.collectLoadedRows = false;
	noRows.collectMatchedRows = false;
	auto countOnly = dataManager->bulkMatchPropertyEntityPredicateSpecs({firstId, secondId}, {0, 1}, 2, predicates,
																		nullptr, noRows);
	EXPECT_TRUE(countOnly.loadedRows.empty());
	EXPECT_TRUE(countOnly.matchedRows.empty());
	EXPECT_EQ(countOnly.loadedCount, 2U);
	EXPECT_EQ(countOnly.matchedCount, 1U);
}

TEST_F(DataManagerTest, BulkPropertyEntityDirectOrderedRowsPreserveRowsAndColumns) {
	std::vector<int64_t> propertyIds;
	for (int64_t rank = 0; rank < 3; ++rank) {
		propertyIds.push_back(addNodeWithPropertyEntity(
				dataManager, {{"rank", PropertyValue(rank)}, {"name", PropertyValue("node-" + std::to_string(rank))}}));
	}
	ASSERT_TRUE(std::is_sorted(propertyIds.begin(), propertyIds.end()));
	ASSERT_NE(propertyIds.front(), 0);

	simulateSave();
	dataManager->clearCache();

	const std::vector<size_t> rows{1, 2, 3};
	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{1}))};
	auto match = dataManager->bulkMatchPropertyEntityPredicateSpecs(propertyIds, rows, 4, predicates, nullptr);
	EXPECT_EQ(match.loadedRows, rows);
	EXPECT_EQ(match.matchedRows, (std::vector<size_t>{2U, 3U}));
	EXPECT_EQ(match.loadedCount, 3U);
	EXPECT_EQ(match.matchedCount, 2U);

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns["rank"] = std::vector<std::optional<PropertyValue>>(4, std::nullopt);
	auto loadedColumnRows =
			dataManager->bulkLoadPropertyEntityColumns(propertyIds, rows, 4, {"rank"}, columns, nullptr);
	EXPECT_EQ(loadedColumnRows, rows);
	EXPECT_FALSE(columns["rank"][0].has_value());
	for (size_t row = 1; row < columns["rank"].size(); ++row) {
		ASSERT_TRUE(columns["rank"][row].has_value());
		EXPECT_EQ(std::get<int64_t>(columns["rank"][row]->getVariant()), static_cast<int64_t>(row - 1));
	}

	std::vector<int64_t> valueRanks(4, -1);
	const size_t valueVisits = dataManager->bulkVisitPropertyEntityValues(
			propertyIds, rows, 4, "rank",
			[&](size_t row, const PropertyValue &value) { valueRanks[row] = std::get<int64_t>(value.getVariant()); },
			nullptr);
	EXPECT_EQ(valueVisits, 3U);
	EXPECT_EQ(valueRanks, (std::vector<int64_t>{-1, 0, 1, 2}));

	std::vector<int64_t> scalarRanks(4, -1);
	const size_t scalarVisits = dataManager->bulkVisitPropertyEntityScalarValues(
			propertyIds, rows, 4, "rank",
			[&](size_t row, const PropertyEntityScalarValue &value) {
				ASSERT_EQ(value.type, PropertyType::INTEGER);
				scalarRanks[row] = value.intValue;
			},
			nullptr);
	EXPECT_EQ(scalarVisits, 3U);
	EXPECT_EQ(scalarRanks, (std::vector<int64_t>{-1, 0, 1, 2}));
}

TEST_F(DataManagerTest, BulkCollectPropertyValuesByOwnerTypeFiltersOwnerTypeAndIds) {
	Node first = createTestNode(dataManager, "OwnerValueNode");
	dataManager->addNode(first);
	dataManager->addNodeProperties(first.getId(),
								   {{"name", PropertyValue("alice")}, {"rank", PropertyValue(int64_t{1})}});

	Node second = createTestNode(dataManager, "OwnerValueNode");
	dataManager->addNode(second);
	dataManager->addNodeProperties(second.getId(),
								   {{"name", PropertyValue("bob")}, {"rank", PropertyValue(int64_t{2})}});

	Edge edge = createTestEdge(dataManager, first.getId(), second.getId(), "OWNER_VALUE_EDGE");
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(),
								   {{"name", PropertyValue("edge")}, {"rank", PropertyValue(int64_t{9})}});

	simulateSave();
	dataManager->clearCache();

	auto valuesByOwner = [](const std::vector<PropertyEntityOwnerValue> &values) {
		std::map<int64_t, PropertyValue> mapped;
		for (const auto &entry: values) {
			mapped.emplace(entry.ownerId, entry.value);
		}
		return mapped;
	};

	auto nodeNames = valuesByOwner(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Node, "name"));
	ASSERT_EQ(nodeNames.size(), 2U);
	EXPECT_EQ(nodeNames.at(first.getId()), PropertyValue("alice"));
	EXPECT_EQ(nodeNames.at(second.getId()), PropertyValue("bob"));

	auto filteredRanks = valuesByOwner(
			dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Node, "rank", {second.getId()}));
	ASSERT_EQ(filteredRanks.size(), 1U);
	EXPECT_EQ(filteredRanks.at(second.getId()), PropertyValue(int64_t{2}));

	auto unsortedRanks = valuesByOwner(dataManager->bulkCollectPropertyValuesByOwnerType(
			EntityType::Node, "rank", {second.getId(), first.getId(), second.getId()}));
	ASSERT_EQ(unsortedRanks.size(), 2U);
	EXPECT_EQ(unsortedRanks.at(first.getId()), PropertyValue(int64_t{1}));
	EXPECT_EQ(unsortedRanks.at(second.getId()), PropertyValue(int64_t{2}));

	auto edgeNames = valuesByOwner(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Edge, "name"));
	ASSERT_EQ(edgeNames.size(), 1U);
	EXPECT_EQ(edgeNames.at(edge.getId()), PropertyValue("edge"));

	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Node, "missing").empty());
	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Node, "name", std::vector<int64_t>{})
						.empty());
}

TEST_F(DataManagerTest, BulkCollectPropertyPredicateOwnerIdsByOwnerTypeFiltersRangeAndPredicates) {
	Node first = createTestNode(dataManager, "OwnerPredicateNode");
	dataManager->addNode(first);
	dataManager->addNodeProperties(first.getId(),
								   {{"rank", PropertyValue(int64_t{1})}, {"kind", PropertyValue("node")}});

	Node second = createTestNode(dataManager, "OwnerPredicateNode");
	dataManager->addNode(second);
	dataManager->addNodeProperties(second.getId(),
								   {{"rank", PropertyValue(int64_t{3})}, {"kind", PropertyValue("node")}});

	Edge firstEdge = createTestEdge(dataManager, first.getId(), second.getId(), "OWNER_PREDICATE_EDGE");
	dataManager->addEdge(firstEdge);
	dataManager->addEdgeProperties(firstEdge.getId(),
								   {{"rank", PropertyValue(int64_t{3})}, {"kind", PropertyValue("edge")}});

	Edge secondEdge = createTestEdge(dataManager, second.getId(), first.getId(), "OWNER_PREDICATE_EDGE");
	dataManager->addEdge(secondEdge);
	dataManager->addEdgeProperties(secondEdge.getId(),
								   {{"rank", PropertyValue(int64_t{5})}, {"kind", PropertyValue("edge")}});

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> rankAtLeastThree{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{3}))};
	auto nodeOwners = dataManager->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Node, rankAtLeastThree);
	EXPECT_EQ(nodeOwners, (std::vector<int64_t>{second.getId()}));

	const std::vector<PropertyEntityPredicate> boundedNodeRank{
			rangePred("rank", PropertyValue(int64_t{2}), PropertyValue(int64_t{4}))};
	auto boundedNodeOwners =
			dataManager->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Node, boundedNodeRank);
	EXPECT_EQ(boundedNodeOwners, (std::vector<int64_t>{second.getId()}));

	std::vector<PropertyEntityPredicate> edgePredicates = rankAtLeastThree;
	edgePredicates.push_back(pred("kind", PropertyEntityPredicateOp::PEP_EQ, PropertyValue("edge")));
	auto edgeOwners = dataManager->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Edge, edgePredicates);
	EXPECT_EQ(edgeOwners, (std::vector<int64_t>{firstEdge.getId(), secondEdge.getId()}));

	PropertyEntityOwnerPredicateScanOptions range;
	range.beginOwnerId = secondEdge.getId();
	range.endOwnerId = secondEdge.getId();
	auto rangedEdgeOwners =
			dataManager->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Edge, edgePredicates, range);
	EXPECT_EQ(rangedEdgeOwners, (std::vector<int64_t>{secondEdge.getId()}));

	range.beginOwnerId = secondEdge.getId();
	range.endOwnerId = firstEdge.getId();
	EXPECT_TRUE(dataManager->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Edge, edgePredicates, range)
						.empty());
}

TEST_F(DataManagerTest, BulkCollectAllPropertyPredicateOwnerIdsByOwnerTypeParallelMatchesSequentialRange) {
	Node source = createTestNode(dataManager, "ParallelOwnerPredicateNode");
	Node target = createTestNode(dataManager, "ParallelOwnerPredicateNode");
	dataManager->addNode(source);
	dataManager->addNode(target);

	std::vector<Edge> edges;
	edges.reserve(64);
	for (size_t i = 0; i < 64; ++i) {
		Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "PARALLEL_OWNER_PREDICATE_EDGE");
		dataManager->addEdge(edge);
		edges.push_back(edge);
	}

	const size_t propertyCount =
			static_cast<size_t>(PROPERTIES_PER_SEGMENT) * kParallelPropertyReadSegmentsForDecisionTests;
	for (size_t i = 0; i < propertyCount; ++i) {
		const size_t edgeIndex = i % edges.size();
		auto property = createTestProperty(edges[edgeIndex].getId(), Edge::typeId,
										   {{"rank", PropertyValue(static_cast<int64_t>(i % 10))},
											{"bucket", PropertyValue(static_cast<int64_t>(edgeIndex % 3))}});
		dataManager->addPropertyEntity(property);
	}

	simulateSave();
	dataManager->clearCache();

	PropertyEntityOwnerPredicateScanOptions options;
	options.beginOwnerId = edges[10].getId();
	options.endOwnerId = edges[40].getId();
	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{8})),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))};

	std::vector<int64_t> expected;
	for (size_t i = 10; i <= 40; ++i) {
		if (i % 3 == 1) {
			expected.push_back(edges[i].getId());
		}
	}

	graph::concurrent::ThreadPool pool(4);
	auto sequentialOwners = dataManager->bulkCollectAllPropertyPredicateOwnerIdsByOwnerType(
			EntityType::Edge, predicates, options, nullptr);
	auto parallelOwners = dataManager->bulkCollectAllPropertyPredicateOwnerIdsByOwnerType(EntityType::Edge, predicates,
																						  options, &pool);
	EXPECT_EQ(sequentialOwners, expected);
	EXPECT_EQ(parallelOwners, sequentialOwners);
}

TEST_F(DataManagerTest, BulkPropertyEntityDirectOrderedRowsRejectsUnsafeLayouts) {
	EXPECT_FALSE(canUseDirectOrderedRows({}, {}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({1}, {}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({1}, {0}, 0));
	EXPECT_FALSE(canUseDirectOrderedRows({0}, {0}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({2, 1}, {0, 1}, 2));
	EXPECT_FALSE(canUseDirectOrderedRows({1, 2}, {1, 1}, 2));
	EXPECT_FALSE(canUseDirectOrderedRows({1, 2}, {0, 2}, 2));
	EXPECT_TRUE(canUseDirectOrderedRows({1, 3}, {1, 2}, 3));
}

TEST_F(DataManagerTest, PropertyEntityScanRejectsInvalidInputsAndUnavailableOwnerTypes) {
	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntities(std::vector<int64_t>{}).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues(std::vector<int64_t>{}, {"rank"}).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({1}, std::vector<std::string>{}).empty());
	EXPECT_TRUE(
			dataManager
					->bulkLoadPropertyEntityColumns(std::vector<int64_t>{}, std::vector<size_t>{}, 1, {"rank"}, columns)
					.empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({1}, std::vector<size_t>{}, 1, {"rank"}, columns).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({1}, {0}, 0, {"rank"}, columns).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({1}, {0}, 1, std::vector<std::string>{}, columns).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({1}, {0}, 1, {"rank"}, columns).empty());
	columns["rank"] = {};
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({1}, {0}, 1, {"rank"}, columns).empty());
	columns["rank"] = std::vector<std::optional<PropertyValue>>(1, std::nullopt);
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({0}, {0}, 1, {"rank"}, columns).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({999999}, {0}, 1, {"rank"}, columns).empty());

	auto scalarVisitor = [](size_t, size_t, const PropertyEntityScalarValue &) {};
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned(std::vector<int64_t>{}, std::vector<size_t>{},
																		  1, "rank", {}, scalarVisitor),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned({1}, {}, 1, "rank", {}, scalarVisitor), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned({1}, {0}, 0, "rank", {}, scalarVisitor), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned({1}, {0}, 1, "", {}, scalarVisitor), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned({1}, {0}, 1, "rank", {},
																		  PropertyEntityScalarPartitionVisitor{}),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValuesPartitioned({0}, {0}, 1, "rank", {}, scalarVisitor), 0U);

	Node unsaved = createTestNode(dataManager, "UnsavedPropertyEntityOwner");
	dataManager->addNode(unsaved);
	dataManager->addNodeProperties(unsaved.getId(), {{"rank", PropertyValue(int64_t{1})}});
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
	EXPECT_FALSE(dataManager->canCountAllPropertyPredicatesByOwnerType(EntityType::Node));

	auto expectEmptyCount = [](const PropertyEntityPredicateCountResult &result) {
		EXPECT_EQ(result.loadedCount, 0U);
		EXPECT_EQ(result.matchedCount, 0U);
	};

	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Blob));
	expectEmptyCount(dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(
			EntityType::Node, std::vector<PropertyEntityPredicate>{}));
	expectEmptyCount(dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(
			EntityType::Blob, {pred("rank", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))}));
	expectEmptyCount(dataManager->bulkCountAllPropertyPredicateSpecsByOwnerType(
			EntityType::Node, std::vector<PropertyEntityPredicate>{}));
	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Blob, "rank").empty());
	EXPECT_TRUE(dataManager->bulkCollectPropertyValuesByOwnerType(EntityType::Blob, "rank", {1}).empty());
	EXPECT_TRUE(dataManager
						->bulkCollectPropertyPredicateOwnerIdsByOwnerType(
								EntityType::Blob,
								{pred("rank", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))})
						.empty());
	EXPECT_TRUE(dataManager
						->bulkCollectPropertyPredicateOwnerIdsByOwnerType(EntityType::Node,
																		  std::vector<PropertyEntityPredicate>{})
						.empty());
	EXPECT_TRUE(dataManager
						->bulkCollectAllPropertyPredicateOwnerIdsByOwnerType(EntityType::Node,
																			 std::vector<PropertyEntityPredicate>{})
						.empty());
}

TEST_F(DataManagerTest, PropertyEntityPartitionedScalarScanSupportsOptionalInitialization) {
	const int64_t propertyId = addNodeWithPropertyEntity(
			dataManager, {{"rank", PropertyValue(int64_t{7})}, {"name", PropertyValue("partitioned")}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::vector<int64_t> ranks(2, -1);
	const auto directVisits = dataManager->bulkVisitPropertyEntityScalarValuesPartitioned(
			{propertyId}, {0}, 2, "rank", {},
			[&](size_t partition, size_t row, const PropertyEntityScalarValue &value) {
				EXPECT_EQ(partition, 0U);
				if (value.type == PropertyType::INTEGER) {
					ranks[row] = value.intValue;
				}
			});
	EXPECT_EQ(directVisits, 1U);
	EXPECT_EQ(ranks[0], 7);

	const auto nonDirectVisits = dataManager->bulkVisitPropertyEntityScalarValuesPartitioned(
			{propertyId, propertyId}, {1, 0}, 2, "rank", {},
			[&](size_t partition, size_t row, const PropertyEntityScalarValue &value) {
				EXPECT_EQ(partition, 0U);
				if (value.type == PropertyType::INTEGER) {
					ranks[row] = value.intValue;
				}
			});
	EXPECT_EQ(nonDirectVisits, 2U);
	EXPECT_EQ(ranks, (std::vector<int64_t>{7, 7}));

	size_t initializedPartitions = 0;
	const auto initializedVisits = dataManager->bulkVisitPropertyEntityScalarValuesPartitioned(
			{propertyId, propertyId}, {1, 0}, 2, "rank",
			[&](size_t partitionCount) { initializedPartitions = partitionCount; },
			[&](size_t partition, size_t row, const PropertyEntityScalarValue &value) {
				EXPECT_EQ(partition, 0U);
				EXPECT_LT(row, ranks.size());
				EXPECT_EQ(value.type, PropertyType::INTEGER);
			});
	EXPECT_EQ(initializedPartitions, 1U);
	EXPECT_EQ(initializedVisits, 2U);
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityColumnsParallelHandlesFallbackRows) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"name", PropertyValue("node-" + std::to_string(i))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(2);
	const std::vector<int64_t> selectedIds{propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
										   propertyIds[PROPERTIES_PER_SEGMENT]};
	const std::vector<size_t> rows{2, 0, 1};
	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns["rank"] = std::vector<std::optional<PropertyValue>>(3, std::nullopt);

	auto loadedRows = dataManager->bulkLoadPropertyEntityColumns(selectedIds, rows, 3, {"rank"}, columns, &pool);
	std::sort(loadedRows.begin(), loadedRows.end());

	EXPECT_EQ(loadedRows, (std::vector<size_t>{0U, 1U, 2U}));
	ASSERT_TRUE(columns["rank"][0].has_value());
	ASSERT_TRUE(columns["rank"][1].has_value());
	ASSERT_TRUE(columns["rank"][2].has_value());
	EXPECT_EQ(std::get<int64_t>(columns["rank"][0]->getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(columns["rank"][1]->getVariant()), static_cast<int64_t>(PROPERTIES_PER_SEGMENT));
	EXPECT_EQ(std::get<int64_t>(columns["rank"][2]->getVariant()), static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 1));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesAndColumnsParallelUseCoalescedReads) {
	Node owner = createTestNode(dataManager, "ParallelPropertyEntityLoader");
	dataManager->addNode(owner);

	const size_t propertyCount =
			static_cast<size_t>(PROPERTIES_PER_SEGMENT) * kParallelPropertyReadSegmentsForDecisionTests;
	std::vector<int64_t> propertyIds;
	std::vector<size_t> rows;
	propertyIds.reserve(propertyCount);
	rows.reserve(propertyCount);
	for (size_t i = 0; i < propertyCount; ++i) {
		auto property = createTestProperty(owner.getId(), Node::typeId,
										   {{"rank", PropertyValue(static_cast<int64_t>(i))},
											{"name", PropertyValue("node-" + std::to_string(i))}});
		dataManager->addPropertyEntity(property);
		propertyIds.push_back(property.getId());
		rows.push_back(propertyCount - i - 1);
	}

	simulateSave();
	dataManager->clearCache();

	graph::concurrent::ThreadPool pool(4);
	const auto values = dataManager->bulkLoadPropertyEntityValues(propertyIds, {"rank", "name"}, &pool);
	ASSERT_EQ(values.size(), propertyCount);
	EXPECT_EQ(values.at(propertyIds.front()).at("rank"), PropertyValue(int64_t{0}));
	EXPECT_EQ(values.at(propertyIds[propertyCount / 2]).at("rank"),
			  PropertyValue(static_cast<int64_t>(propertyCount / 2)));
	EXPECT_EQ(values.at(propertyIds.back()).at("name"), PropertyValue("node-" + std::to_string(propertyCount - 1)));

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns["rank"] = std::vector<std::optional<PropertyValue>>(propertyCount, std::nullopt);
	columns["name"] = std::vector<std::optional<PropertyValue>>(propertyCount, std::nullopt);
	auto loadedRows = dataManager->bulkLoadPropertyEntityColumns(propertyIds, rows, propertyCount,
																 {"rank", "name", "rank"}, columns, &pool);
	std::sort(loadedRows.begin(), loadedRows.end());

	ASSERT_EQ(loadedRows.size(), propertyCount);
	EXPECT_EQ(loadedRows.front(), 0U);
	EXPECT_EQ(loadedRows.back(), propertyCount - 1);
	ASSERT_TRUE(columns["rank"][0].has_value());
	ASSERT_TRUE(columns["rank"][propertyCount - 1].has_value());
	EXPECT_EQ(columns["rank"][0].value(), PropertyValue(static_cast<int64_t>(propertyCount - 1)));
	EXPECT_EQ(columns["rank"][propertyCount - 1].value(), PropertyValue(int64_t{0}));
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"name", PropertyValue("node-" + std::to_string(i))}}));
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
			[&](size_t row, const PropertyValue &value) { valuesByRow[row] = std::get<int64_t>(value.getVariant()); },
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
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"name", PropertyValue("node-" + std::to_string(i))}}));
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
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 1, "", scalarVisitor, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId}, {0}, 1, "rank", {}, nullptr), 0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({0, propertyId}, {0, 9}, 1, "rank", scalarVisitor,
															   nullptr),
			  0U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({propertyId + 10000}, {0}, 1, "rank", scalarVisitor,
															   nullptr),
			  0U);
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesDeduplicatesRowsAndHandlesMissesInFallbackPath) {
	const int64_t firstId = addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(int64_t{7})}});
	const int64_t secondId = addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(int64_t{9})}});
	ASSERT_NE(firstId, 0);
	ASSERT_NE(secondId, 0);
	simulateSave();
	dataManager->clearCache();

	size_t valueVisits = 0;
	auto valueVisitor = [&](size_t, const PropertyValue &) { ++valueVisits; };
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({firstId, secondId}, {0, 0}, 1, "rank", valueVisitor, nullptr),
			  1U);
	EXPECT_EQ(valueVisits, 1U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({secondId, firstId}, {0, 1}, 2, "rank", valueVisitor, nullptr),
			  2U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityValues({secondId + 10000, secondId + 10001}, {0, 0}, 1, "rank",
														 valueVisitor, nullptr),
			  0U);

	size_t scalarVisits = 0;
	auto scalarVisitor = [&](size_t, const PropertyEntityScalarValue &) { ++scalarVisits; };
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({firstId, secondId}, {0, 0}, 1, "rank", scalarVisitor,
															   nullptr),
			  1U);
	EXPECT_EQ(scalarVisits, 1U);
	EXPECT_EQ(dataManager->bulkVisitPropertyEntityScalarValues({secondId + 10000, secondId + 10001}, {0, 0}, 1, "rank",
															   scalarVisitor, nullptr),
			  0U);
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesParallelReadsCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"name", PropertyValue("node-" + std::to_string(i))}}));
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
	EXPECT_EQ(std::get<int64_t>(
					  loaded.at(propertyIds[PROPERTIES_PER_SEGMENT]).getPropertyValues().at("rank").getVariant()),
			  static_cast<int64_t>(PROPERTIES_PER_SEGMENT));
	EXPECT_EQ(std::get<int64_t>(
					  loaded.at(propertyIds[PROPERTIES_PER_SEGMENT + 1]).getPropertyValues().at("rank").getVariant()),
			  static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 1));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsParallelScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
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

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsSequentialScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	std::vector<size_t> rows;
	std::vector<size_t> expectedMatchedRows;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 4);
	rows.reserve(PROPERTIES_PER_SEGMENT + 4);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 4; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"bucket", PropertyValue(static_cast<int64_t>(i % 3))}}));
		rows.push_back(static_cast<size_t>(i));
		if (i >= PROPERTIES_PER_SEGMENT && i % 3 == 0) {
			expectedMatchedRows.push_back(static_cast<size_t>(i));
		}
	}

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			pred("rank", PropertyEntityPredicateOp::PEP_GE,
				 PropertyValue(static_cast<int64_t>(PROPERTIES_PER_SEGMENT))),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{0}))};
	auto result =
			dataManager->bulkMatchPropertyEntityPredicateSpecs(propertyIds, rows, rows.size(), predicates, nullptr);
	std::sort(result.loadedRows.begin(), result.loadedRows.end());
	std::sort(result.matchedRows.begin(), result.matchedRows.end());

	EXPECT_EQ(result.loadedCount, propertyIds.size());
	EXPECT_EQ(result.loadedRows, rows);
	EXPECT_EQ(result.matchedCount, expectedMatchedRows.size());
	EXPECT_EQ(result.matchedRows, expectedMatchedRows);
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicateSpecsParallelScansCoalescedSegments) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(PROPERTIES_PER_SEGMENT + 2);
	for (uint32_t i = 0; i < PROPERTIES_PER_SEGMENT + 2; ++i) {
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
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
		propertyIds.push_back(
				addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(static_cast<int64_t>(i))},
														{"bucket", PropertyValue(static_cast<int64_t>(i % 2))}}));
	}
	ASSERT_EQ(propertyIds.size(), static_cast<size_t>(PROPERTIES_PER_SEGMENT + 2));
	simulateSave();
	dataManager->clearCache();

	const std::string rankKey = "rank";
	const PropertyValue lower(int64_t{0});
	const PropertyValue upper(static_cast<int64_t>(PROPERTIES_PER_SEGMENT + 2));
	std::vector<PropertyEntityPredicate> predicates{rangePred(rankKey, lower, upper)};
	std::vector<PredicateSpecExpectation> specs{{&predicates.front().key, &predicates.front().value,
												 &predicates.front().upperValue.value(), predicates.front().op}};
	const auto groups = groupPredicateSpecExpectations(specs);
	auto matcher = [&](const char *buf) { return readPropertyEntityPredicateMatch(buf, groups, specs.size()); };

	const std::vector<int64_t> unsortedWithDuplicates{0, propertyIds[PROPERTIES_PER_SEGMENT + 1], propertyIds[0],
													  propertyIds[PROPERTIES_PER_SEGMENT + 1]};
	auto serialCount = countPropertyEntityMatches(*dataManager, unsortedWithDuplicates, nullptr, matcher);
	EXPECT_EQ(serialCount.loadedCount, 3U);
	EXPECT_EQ(serialCount.matchedCount, 3U);
	graph::concurrent::ThreadPool pool(2);
	auto parallelCount = countPropertyEntityMatches(*dataManager, unsortedWithDuplicates, &pool, matcher);
	EXPECT_EQ(parallelCount.loadedCount, 3U);
	EXPECT_EQ(parallelCount.matchedCount, 3U);
	auto invalidZeroCount = countPropertyEntityMatches(*dataManager, std::vector<int64_t>{0, 0}, nullptr, matcher);
	EXPECT_EQ(invalidZeroCount.loadedCount, 0U);
	EXPECT_EQ(invalidZeroCount.matchedCount, 0U);
	auto missingCount = countPropertyEntityMatches(*dataManager, std::vector<int64_t>{propertyIds.back() + 10000},
												   nullptr, matcher);
	EXPECT_EQ(missingCount.loadedCount, 0U);
	EXPECT_EQ(missingCount.matchedCount, 0U);
}

TEST_F(DataManagerTest, OwnerTypePredicateCountScansOnlyMatchingPropertyOwners) {
	Node first = createTestNode(dataManager, "OwnerTypeScanNode");
	Node second = createTestNode(dataManager, "OwnerTypeScanNode");
	dataManager->addNode(first);
	dataManager->addNode(second);
	dataManager->addNodeProperties(first.getId(), {{"score", PropertyValue(95.0)}});
	dataManager->addNodeProperties(second.getId(), {{"score", PropertyValue(12.0)}});

	Edge edge = createTestEdge(dataManager, first.getId(), second.getId(), "OWNER_TYPE_EDGE");
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(), {{"score", PropertyValue(99.0)}});

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			pred("score", PropertyEntityPredicateOp::PEP_GE, PropertyValue(90.0))};
	ASSERT_TRUE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
	auto nodeCount =
			dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Node, predicates, nullptr);
	EXPECT_EQ(nodeCount.loadedCount, 2U);
	EXPECT_EQ(nodeCount.matchedCount, 1U);

	auto edgeCount =
			dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Edge, predicates, nullptr);
	EXPECT_EQ(edgeCount.loadedCount, 1U);
	EXPECT_EQ(edgeCount.matchedCount, 1U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Property, predicates, nullptr)
					  .matchedCount,
			  0U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Node, {}, nullptr).matchedCount,
			  0U);
}

TEST_F(DataManagerTest, OwnerTypePredicateCountSupportsCompoundRangePredicates) {
	Node first = createTestNode(dataManager, "OwnerTypeCompoundPredicateNode");
	Node second = createTestNode(dataManager, "OwnerTypeCompoundPredicateNode");
	Node third = createTestNode(dataManager, "OwnerTypeCompoundPredicateNode");
	dataManager->addNode(first);
	dataManager->addNode(second);
	dataManager->addNode(third);
	dataManager->addNodeProperties(first.getId(),
								   {{"score", PropertyValue(95.0)}, {"bucket", PropertyValue(int64_t{1})}});
	dataManager->addNodeProperties(second.getId(),
								   {{"score", PropertyValue(88.0)}, {"bucket", PropertyValue(int64_t{1})}});
	dataManager->addNodeProperties(third.getId(),
								   {{"score", PropertyValue(97.0)}, {"bucket", PropertyValue(int64_t{2})}});

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			rangePred("score", PropertyValue(90.0), PropertyValue(100.0)),
			pred("bucket", PropertyEntityPredicateOp::PEP_EQ, PropertyValue(int64_t{1}))};
	auto count = dataManager->bulkCountPropertyEntityPredicateSpecsByOwnerType(EntityType::Node, predicates, nullptr);
	EXPECT_EQ(count.loadedCount, 3U);
	EXPECT_EQ(count.matchedCount, 1U);
}

TEST_F(DataManagerTest, CompleteOwnerTypePredicateCountRejectsBlobBackedProperties) {
	Node first = createTestNode(dataManager, "OwnerTypeBlobPredicateNode");
	Node second = createTestNode(dataManager, "OwnerTypeBlobPredicateNode");
	dataManager->addNode(first);
	dataManager->addNode(second);
	dataManager->addNodeProperties(first.getId(), {{"score", PropertyValue(int64_t{95})}});
	dataManager->addNodeProperties(second.getId(), {{"payload", PropertyValue(std::string(5000, 'x'))}});

	simulateSave();
	dataManager->clearCache();

	const std::vector<PropertyEntityPredicate> predicates{
			pred("score", PropertyEntityPredicateOp::PEP_GE, PropertyValue(int64_t{90}))};
	ASSERT_TRUE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
	EXPECT_FALSE(dataManager->canCountAllPropertyPredicatesByOwnerType(EntityType::Node));
	EXPECT_EQ(dataManager->bulkCountAllPropertyPredicateSpecsByOwnerType(EntityType::Node, predicates, nullptr)
					  .loadedCount,
			  0U);
}

TEST_F(DataManagerTest, OwnerTypePredicateCountRejectsUnsavedOrSnapshotDirtyState) {
	Node saved = createTestNode(dataManager, "OwnerTypeSnapshotNode");
	dataManager->addNode(saved);
	dataManager->addNodeProperties(saved.getId(), {{"score", PropertyValue(95.0)}});
	simulateSave();
	dataManager->clearCache();
	ASSERT_TRUE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));

	Node unsaved = createTestNode(dataManager, "OwnerTypeSnapshotNode");
	dataManager->addNode(unsaved);
	dataManager->addNodeProperties(unsaved.getId(), {{"score", PropertyValue(99.0)}});
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
	simulateSave();
	dataManager->clearCache();

	CommittedSnapshot emptySnapshot;
	dataManager->setCurrentSnapshot(&emptySnapshot);
	EXPECT_TRUE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));

	CommittedSnapshot propertySnapshot;
	propertySnapshot.properties.emplace(1, DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED));
	dataManager->setCurrentSnapshot(&propertySnapshot);
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));

	CommittedSnapshot blobSnapshot;
	blobSnapshot.blobs.emplace(1, DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED));
	dataManager->setCurrentSnapshot(&blobSnapshot);
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));

	CommittedSnapshot nodeSnapshot;
	nodeSnapshot.nodes.emplace(saved.getId(), DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED));
	dataManager->setCurrentSnapshot(&nodeSnapshot);
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));

	CommittedSnapshot edgeSnapshot;
	edgeSnapshot.edges.emplace(1, DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED));
	dataManager->setCurrentSnapshot(&edgeSnapshot);
	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Edge));
	dataManager->clearCurrentSnapshot();
}

TEST_F(DataManagerTest, OwnerTypePredicateCountRejectsInactiveOwnerSegments) {
	Node inactive = createTestNode(dataManager, "OwnerTypeInactiveNode");
	dataManager->addNode(inactive);
	dataManager->addNodeProperties(inactive.getId(), {{"score", PropertyValue(95.0)}});
	simulateSave();
	dataManager->deleteNode(inactive);
	simulateSave();
	dataManager->clearCache();

	EXPECT_FALSE(dataManager->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsRejectsInvalidInputs) {
	const int64_t propertyId = addNodeWithPropertyEntity(dataManager, {{"rank", PropertyValue(int64_t{7})}});
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
