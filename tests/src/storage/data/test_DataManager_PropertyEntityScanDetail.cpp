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

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "DataManagerTestFixture.hpp"
#include "graph/storage/SegmentTracker.hpp"

namespace graph::storage {
namespace {

std::string serializePropertyValueBytes(const PropertyValue &value) {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::serialize<PropertyValue>(stream, value);
	return stream.str();
}

PredicateSpecExpectation predicateSpec(const std::string &key, const PropertyValue &value,
									   PropertyEntityPredicateOp op, const PropertyValue *upperValue = nullptr) {
	PredicateSpecExpectation spec;
	spec.key = &key;
	spec.value = &value;
	spec.upperValue = upperValue;
	spec.op = op;
	return spec;
}

std::string makeRawPropertyRecordForOwner(int64_t propertyId, int64_t ownerId, uint32_t entityType, bool active,
										  const std::vector<std::pair<std::string, std::string>> &entries) {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::writePOD(stream, propertyId);
	utils::Serializer::writePOD(stream, ownerId);
	utils::Serializer::writePOD(stream, entityType);
	utils::Serializer::writePOD(stream, active);
	utils::Serializer::writePOD(stream, static_cast<uint32_t>(entries.size()));
	for (const auto &[key, valueBytes] : entries) {
		utils::Serializer::serialize<std::string>(stream, key);
		stream.write(valueBytes.data(), static_cast<std::streamsize>(valueBytes.size()));
	}
	std::string record = stream.str();
	record.resize(Property::TOTAL_PROPERTY_SIZE, '\0');
	return record;
}

std::string makeRawPropertyRecord(const std::vector<std::pair<std::string, std::string>> &entries) {
	return makeRawPropertyRecordForOwner(99, 7, Node::typeId, true, entries);
}

std::shared_ptr<DataManager> makeDataManagerWithoutPositionalRead() {
	auto file = std::make_shared<std::fstream>();
	static auto header = std::make_shared<FileHeader>();
	auto tracker = std::make_shared<SegmentTracker>(nullptr, *header);
	IDAllocators allocators;
	return std::make_shared<DataManager>(file, 0, *header, std::move(allocators), tracker, nullptr);
}

struct AlwaysTruePropertyEntityMatcher {
	std::optional<bool> operator()(const char *) const { return true; }
};

std::string makeRawPropertyRecordWithMalformedKey() {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::writePOD(stream, int64_t{99});
	utils::Serializer::writePOD(stream, int64_t{7});
	utils::Serializer::writePOD(stream, Node::typeId);
	utils::Serializer::writePOD(stream, true);
	utils::Serializer::writePOD(stream, uint32_t{1});
	utils::Serializer::writePOD(stream, uint32_t{Property::TOTAL_PROPERTY_SIZE});
	std::string record = stream.str();
	record.resize(Property::TOTAL_PROPERTY_SIZE, '\0');
	return record;
}

std::string malformedStringValueBytes() {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::writePOD(stream, PropertyType::STRING);
	utils::Serializer::writePOD(stream, uint32_t{Property::TOTAL_PROPERTY_SIZE});
	return stream.str();
}

std::string truncatedDurationValueBytes(size_t componentCount) {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::writePOD(stream, PropertyType::DURATION);
	for (size_t i = 0; i < componentCount; ++i) {
		utils::Serializer::writePOD(stream, int64_t{static_cast<int64_t>(i + 1)});
	}
	return stream.str();
}

} // namespace

TEST(PropertyEntityScanDetailTest, ReadsAndSkipsSerializedScalarAndFallbackValues) {
	const std::vector<PropertyValue> scalarValues{
			PropertyValue(std::monostate{}),
			PropertyValue(true),
			PropertyValue(int64_t{42}),
			PropertyValue(4.25),
			PropertyValue("milo"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 12, 0, 0, 0)),
			PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)),
	};
	for (const auto &expected: scalarValues) {
		const std::string bytes = serializePropertyValueBytes(expected);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		auto decoded = readSerializedPropertyValue(cursor, end);
		ASSERT_TRUE(decoded.has_value());
		EXPECT_EQ(*decoded, expected);
		EXPECT_EQ(cursor, end);
	}

	std::vector<PropertyValue> listValue{PropertyValue(int64_t{1}), PropertyValue("two")};
	const PropertyValue listProperty(std::move(listValue));
	std::string listBytes = serializePropertyValueBytes(listProperty);
	const char *readCursor = listBytes.data();
	const char *readEnd = readCursor + listBytes.size();
	auto decodedList = readSerializedPropertyValue(readCursor, readEnd);
	ASSERT_TRUE(decodedList.has_value());
	EXPECT_EQ(*decodedList, listProperty);
	EXPECT_EQ(readCursor, readEnd);

	const char *skipCursor = listBytes.data();
	const char *skipEnd = skipCursor + listBytes.size();
	EXPECT_TRUE(skipPropertyValue(skipCursor, skipEnd));
	EXPECT_EQ(skipCursor, skipEnd);
}

TEST(PropertyEntityScanDetailTest, DirectOrderedRowsValidationRejectsUnsafeInputs) {
	EXPECT_FALSE(canUseDirectOrderedRows({}, {}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({1}, {}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({1}, {0}, 0));
	EXPECT_FALSE(canUseDirectOrderedRows({0}, {0}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({1}, {1}, 1));
	EXPECT_FALSE(canUseDirectOrderedRows({2, 1}, {0, 1}, 2));
	EXPECT_FALSE(canUseDirectOrderedRows({1, 2}, {1, 1}, 3));
	EXPECT_TRUE(canUseDirectOrderedRows({1, 2, 4}, {0, 1, 3}, 4));
}

TEST(PropertyEntityScanDetailTest, PersistedScanHelpersReturnEmptyWithoutPositionalReads) {
	auto dm = makeDataManagerWithoutPositionalRead();
	ASSERT_FALSE(dm->hasPreadSupport());

	size_t directVisits = visitPropertyEntityRowsDirect(*dm, {1}, {0}, [](size_t, const char *) { return size_t{1}; });
	EXPECT_EQ(directVisits, 0U);

	auto count = countPropertyEntityMatches(*dm, {1}, nullptr, AlwaysTruePropertyEntityMatcher{});
	EXPECT_EQ(count.loadedCount, 0U);
	EXPECT_EQ(count.matchedCount, 0U);

	auto match = matchPropertyEntityRows(*dm, {1}, {0}, 1, nullptr, PropertyEntityPredicateMatchOptions{},
										 AlwaysTruePropertyEntityMatcher{});
	EXPECT_EQ(match.loadedCount, 0U);
	EXPECT_TRUE(match.loadedRows.empty());

	const std::vector<int64_t> owners{1};
	EXPECT_TRUE(collectPropertyValuesByOwnerType(*dm, EntityType::Node, "rank",
												 std::span<const int64_t>(owners.data(), owners.size()), nullptr)
						.empty());
	EXPECT_TRUE(collectPropertyValuesByOwnerType(*dm, EntityType::Node, std::vector<std::string>{"rank"},
												 std::span<const int64_t>(owners.data(), owners.size()), nullptr)
						.empty());

	PropertyEntityOwnerPredicateScanOptions options;
	options.beginOwnerId = 1;
	options.endOwnerId = 10;
	EXPECT_TRUE(collectPropertyPredicateOwnerIdsByOwnerType(*dm, EntityType::Node, options, nullptr,
															AlwaysTruePropertyEntityMatcher{})
						.empty());

	auto ownerCount = countPropertyEntityMatchesByOwnerType(*dm, EntityType::Node, nullptr,
														   AlwaysTruePropertyEntityMatcher{});
	EXPECT_EQ(ownerCount.loadedCount, 0U);
	EXPECT_EQ(ownerCount.matchedCount, 0U);
}

TEST_F(DataManagerTest, PersistedScanHelpersShareMatcherCoverageAcrossPositiveReads) {
	Node node = createTestNode(dataManager, "DetailPositiveScanNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"rank", PropertyValue(int64_t{42})}});
	const Node stored = dataManager->getNode(node.getId());
	ASSERT_TRUE(stored.hasPropertyEntity());
	simulateSave();
	dataManager->clearCache();

	const auto matcher = AlwaysTruePropertyEntityMatcher{};
	const int64_t propertyId = stored.getPropertyEntityId();
	auto count = countPropertyEntityMatches(*dataManager, {propertyId}, nullptr, matcher);
	EXPECT_EQ(count.loadedCount, 1U);
	EXPECT_EQ(count.matchedCount, 1U);

	auto match = matchPropertyEntityRows(
			*dataManager, {propertyId}, {0}, 1, nullptr, PropertyEntityPredicateMatchOptions{}, matcher);
	EXPECT_EQ(match.loadedCount, 1U);
	EXPECT_EQ(match.matchedCount, 1U);
	EXPECT_EQ(match.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(match.matchedRows, (std::vector<size_t>{0U}));

	PropertyEntityOwnerPredicateScanOptions options;
	options.beginOwnerId = node.getId();
	options.endOwnerId = node.getId();
	const auto owners =
			collectPropertyPredicateOwnerIdsByOwnerType(*dataManager, EntityType::Node, options, nullptr, matcher);
	EXPECT_EQ(owners, (std::vector<int64_t>{node.getId()}));

	auto ownerCount = countPropertyEntityMatchesByOwnerType(*dataManager, EntityType::Node, nullptr, matcher);
	EXPECT_EQ(ownerCount.loadedCount, 1U);
	EXPECT_EQ(ownerCount.matchedCount, 1U);
}

TEST(PropertyEntityScanDetailTest, SkipsAllInlineScalarValueEncodings) {
	const std::vector<PropertyValue> values{
			PropertyValue(std::monostate{}),
			PropertyValue(true),
			PropertyValue(int64_t{42}),
			PropertyValue(4.25),
			PropertyValue("milo"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 12, 0, 0, 0)),
			PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)),
	};
	for (const auto &value: values) {
		const std::string bytes = serializePropertyValueBytes(value);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_TRUE(skipPropertyValue(cursor, end));
		EXPECT_EQ(cursor, end);
	}
}

TEST(PropertyEntityScanDetailTest, PredicateHelpersEvaluateFallbackAndStringViewOperators) {
	const std::string key = "rank";
	const PropertyValue one(int64_t{1});
	const PropertyValue two(int64_t{2});
	const PropertyValue three(int64_t{3});

	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, one, PropertyEntityPredicateOp::PEP_NE)));
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, three, PropertyEntityPredicateOp::PEP_LT)));
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, one, PropertyEntityPredicateOp::PEP_GT)));
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, one, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &three)));
	EXPECT_FALSE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
												predicateSpec(key, three, PropertyEntityPredicateOp::PEP_RANGE_CLOSED)));

	const PropertyValue expectedString("2");
	EXPECT_FALSE(typedValueSatisfiesPredicate<int64_t>(
			2, predicateSpec(key, expectedString, PropertyEntityPredicateOp::PEP_EQ)));

	const std::string text = "ab";
	SerializedStringView view{text.data(), static_cast<uint32_t>(text.size())};
	const PropertyValue lower("aa");
	const PropertyValue upper("ac");
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, lower, PropertyEntityPredicateOp::PEP_GT)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, lower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
																&upper)));
	const PropertyValue intExpectation(int64_t{7});
	EXPECT_FALSE(stringViewSatisfiesPredicate(view, predicateSpec(key, intExpectation, PropertyEntityPredicateOp::PEP_EQ)));

	const std::string longer = "abc";
	SerializedStringView shorterView{text.data(), static_cast<uint32_t>(text.size())};
	EXPECT_LT(compareStringView(shorterView, longer), 0);
}

TEST(PropertyEntityScanDetailTest, RejectsMalformedSerializedData) {
	const char *emptyCursor = "";
	const char *emptyEnd = emptyCursor;
	uint32_t out = 0;
	EXPECT_FALSE(readRawBytes(emptyCursor, emptyEnd, &out, sizeof(out)));

	std::string shortHeader(sizeof(int64_t), '\0');
	const char *headerCursor = shortHeader.data();
	const char *headerEnd = headerCursor + shortHeader.size();
	PropertyRecordHeader header;
	EXPECT_FALSE(readPropertyRecordHeader(headerCursor, headerEnd, header));

	std::string missingStringSize(sizeof(uint16_t), '\0');
	const char *stringCursor = missingStringSize.data();
	const char *stringEnd = stringCursor + missingStringSize.size();
	std::string decodedString;
	EXPECT_FALSE(readString(stringCursor, stringEnd, decodedString));

	std::string boolBytes = serializePropertyValueBytes(PropertyValue(true));
	boolBytes.resize(sizeof(PropertyType));
	const char *boolCursor = boolBytes.data();
	const char *boolEnd = boolCursor + boolBytes.size();
	EXPECT_FALSE(readSerializedPropertyValue(boolCursor, boolEnd).has_value());

	std::ostringstream listStream(std::ios::binary);
	utils::Serializer::writePOD(listStream, PropertyType::LIST);
	utils::Serializer::writePOD(listStream, uint32_t{1});
	std::string truncatedList = listStream.str();
	const char *listCursor = truncatedList.data();
	const char *listEnd = listCursor + truncatedList.size();
	EXPECT_FALSE(skipPropertyValue(listCursor, listEnd));

	std::string invalidRecord(Property::TOTAL_PROPERTY_SIZE, '\0');
	const char *recordCursor = invalidRecord.data();
	const char *recordEnd = recordCursor + invalidRecord.size();
	EXPECT_FALSE(readActivePropertyRecordHeader(recordCursor, recordEnd).has_value());
	EXPECT_FALSE(readSelectedPropertyValues(invalidRecord.data(), {"missing"}).has_value());
}

TEST(PropertyEntityScanDetailTest, RejectsMalformedInputsThroughPublicScanHelpers) {
	std::string shortHeader(sizeof(int64_t), '\0');
	const char *shortCursor = shortHeader.data();
	const char *shortEnd = shortCursor + shortHeader.size();
	EXPECT_FALSE(readActivePropertyRecordHeader(shortCursor, shortEnd).has_value());

	const char *emptyCursor = "";
	const char *emptyEnd = emptyCursor;
	EXPECT_FALSE(readSerializedPropertyValue(emptyCursor, emptyEnd).has_value());

	std::optional<PropertyValue> fallback;
	const char *emptyScalarCursor = "";
	const char *emptyScalarEnd = emptyScalarCursor;
	EXPECT_FALSE(readSerializedPropertyScalarValue(emptyScalarCursor, emptyScalarEnd, fallback).has_value());

	std::string boolBytes = serializePropertyValueBytes(PropertyValue(true));
	boolBytes.resize(sizeof(PropertyType));
	const char *scalarBoolCursor = boolBytes.data();
	const char *scalarBoolEnd = scalarBoolCursor + boolBytes.size();
	EXPECT_FALSE(readSerializedPropertyScalarValue(scalarBoolCursor, scalarBoolEnd, fallback).has_value());

	const char *equalsCursor = "";
	const char *equalsEnd = equalsCursor;
	EXPECT_FALSE(serializedPropertyValueEquals(equalsCursor, equalsEnd, compilePropertyValue(PropertyValue())).has_value());

	std::string invalidRecord(Property::TOTAL_PROPERTY_SIZE, '\0');
	std::vector<PropertyEntityRowRef> refs{{1, 0}};
	EXPECT_FALSE(visitSelectedPropertyValue(invalidRecord.data(), "missing", refs, 0, refs.size(),
											[](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValue(invalidRecord.data(), "missing", refs, 0, refs.size(),
												  [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());

	const std::string key = "rank";
	const PropertyValue value(int64_t{1});
	PredicateExpectation expected{&key, compilePropertyValue(value)};
	EXPECT_FALSE(readPropertyEntityPredicateMatch(invalidRecord.data(), std::vector<PredicateExpectation>{expected})
						 .has_value());
	SinglePredicateExpectation single{&key, compilePropertyValue(value)};
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(invalidRecord.data(), single).has_value());

	std::vector<PredicateSpecExpectation> specs{predicateSpec(key, value, PropertyEntityPredicateOp::PEP_EQ)};
	auto groups = groupPredicateSpecExpectations(specs);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(invalidRecord.data(), groups, specs.size()).has_value());

	const char *predicateCursor = "";
	const char *predicateEnd = predicateCursor;
	EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(predicateCursor, predicateEnd, specs.front()).has_value());
}

TEST(PropertyEntityScanDetailTest, RejectsMalformedRecordValuePayloads) {
	const std::string key = "rank";
	const PropertyValue value(int64_t{1});
	const std::string malformedValue = malformedStringValueBytes();
	const auto requestedRecord = makeRawPropertyRecord({{key, malformedValue}});

	EXPECT_FALSE(readSelectedPropertyValues(requestedRecord.data(), {key}).has_value());

	std::vector<PropertyEntityRowRef> refs{{99, 0}};
	std::vector<std::optional<PropertyValue>> column(1);
	std::unordered_map<std::string, size_t> requested{{key, 0}};
	std::vector<std::vector<std::optional<PropertyValue>> *> columns{&column};
	EXPECT_FALSE(readSelectedPropertyColumns(requestedRecord.data(), requested, columns, refs, 0, refs.size()));
	EXPECT_FALSE(visitSelectedPropertyValue(requestedRecord.data(), key, refs, 0, refs.size(),
											[](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValue(requestedRecord.data(), key, refs, 0, refs.size(),
												  [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());

	const PropertyValue expectedString("x");
	PredicateExpectation equality{&key, compilePropertyValue(expectedString)};
	EXPECT_FALSE(readPropertyEntityPredicateMatch(requestedRecord.data(), std::vector<PredicateExpectation>{equality})
						 .has_value());
	SinglePredicateExpectation single{&key, compilePropertyValue(value)};
	SinglePredicateExpectation stringSingle{&key, compilePropertyValue(expectedString)};
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(requestedRecord.data(), stringSingle).has_value());
	std::vector<PredicateSpecExpectation> specs{predicateSpec(key, value, PropertyEntityPredicateOp::PEP_EQ)};
	auto groups = groupPredicateSpecExpectations(specs);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(requestedRecord.data(), groups, specs.size()).has_value());

	const auto skippedRecord = makeRawPropertyRecord({{"other", malformedValue}});
	EXPECT_FALSE(visitSelectedPropertyValue(skippedRecord.data(), key, refs, 0, refs.size(),
											[](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValue(skippedRecord.data(), key, refs, 0, refs.size(),
												  [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());
	EXPECT_FALSE(readPropertyEntityPredicateMatch(skippedRecord.data(), std::vector<PredicateExpectation>{equality})
						 .has_value());
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(skippedRecord.data(), single).has_value());
	EXPECT_FALSE(readPropertyEntityPredicateMatch(skippedRecord.data(), groups, specs.size()).has_value());

	std::ostringstream mapOnly(std::ios::binary);
	utils::Serializer::writePOD(mapOnly, PropertyType::MAP);
	std::string truncatedMap = mapOnly.str();
	const char *mapCursor = truncatedMap.data();
	const char *mapEnd = mapCursor + truncatedMap.size();
	EXPECT_FALSE(skipPropertyValue(mapCursor, mapEnd));

	std::ostringstream listOnly(std::ios::binary);
	utils::Serializer::writePOD(listOnly, PropertyType::LIST);
	std::string truncatedList = listOnly.str();
	const char *scalarCursor = truncatedList.data();
	const char *scalarEnd = scalarCursor + truncatedList.size();
	std::optional<PropertyValue> fallback;
	EXPECT_FALSE(readSerializedPropertyScalarValue(scalarCursor, scalarEnd, fallback).has_value());

	const char *predicateCursor = malformedValue.data();
	const char *predicateEnd = predicateCursor + malformedValue.size();
	EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(predicateCursor, predicateEnd, specs.front()).has_value());
}

TEST(PropertyEntityScanDetailTest, RejectsTruncatedTypedValues) {
	const std::vector<PropertyValue> values{
			PropertyValue(true),
			PropertyValue(int64_t{42}),
			PropertyValue(4.25),
			PropertyValue("milo"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 12, 0, 0, 0)),
			PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)),
	};
	for (const auto &value: values) {
		std::string bytes = serializePropertyValueBytes(value);
		ASSERT_GT(bytes.size(), sizeof(PropertyType));
		bytes.pop_back();

		const char *valueCursor = bytes.data();
		const char *valueEnd = valueCursor + bytes.size();
		EXPECT_FALSE(readSerializedPropertyValue(valueCursor, valueEnd).has_value());

		const char *scalarCursor = bytes.data();
		const char *scalarEnd = scalarCursor + bytes.size();
		std::optional<PropertyValue> fallback;
		EXPECT_FALSE(readSerializedPropertyScalarValue(scalarCursor, scalarEnd, fallback).has_value());

		const char *equalsCursor = bytes.data();
		const char *equalsEnd = equalsCursor + bytes.size();
		EXPECT_FALSE(serializedPropertyValueEquals(equalsCursor, equalsEnd, compilePropertyValue(value)).has_value());
	}
}

TEST(PropertyEntityScanDetailTest, ReadsSelectedColumnsVisitorsAndPredicateMatches) {
	Property property;
	property.setId(77);
	property.getMutableMetadata().entityId = 10;
	property.getMutableMetadata().entityType = Node::typeId;
	property.getMutableMetadata().isActive = true;
	property.setProperties({{"rank", PropertyValue(int64_t{42})}, {"name", PropertyValue("milo")},
							{"flag", PropertyValue(true)}});
	auto buffer = utils::FixedSizeSerializer::serializeToBuffer(property, Property::TOTAL_PROPERTY_SIZE);
	const char *raw = buffer.data();

	auto selected = readSelectedPropertyValues(raw, {"rank", "missing"});
	ASSERT_TRUE(selected.has_value());
	ASSERT_TRUE(selected->contains("rank"));
	EXPECT_EQ(std::get<int64_t>(selected->at("rank").getVariant()), 42);
	EXPECT_FALSE(selected->contains("missing"));

	std::vector<PropertyEntityRowRef> refs{{77, 0}, {77, 1}};
	std::vector<std::optional<PropertyValue>> rankColumn(2);
	std::unordered_map<std::string, size_t> requested{{"rank", 0}};
	std::vector<std::vector<std::optional<PropertyValue>> *> columns{&rankColumn};
	EXPECT_TRUE(readSelectedPropertyColumns(raw, requested, columns, refs, 0, refs.size()));
	ASSERT_TRUE(rankColumn[0].has_value());
	ASSERT_TRUE(rankColumn[1].has_value());
	EXPECT_EQ(std::get<int64_t>(rankColumn[0]->getVariant()), 42);
	EXPECT_EQ(std::get<int64_t>(rankColumn[1]->getVariant()), 42);

	std::vector<std::optional<PropertyValue>> singleRankColumn(1);
	std::vector<PropertyEntityRowRef> singleRef{{77, 0}};
	std::vector<std::vector<std::optional<PropertyValue>> *> singleColumns{&singleRankColumn};
	EXPECT_TRUE(readSelectedPropertyColumns(raw, requested, singleColumns, singleRef, 0, singleRef.size()));
	ASSERT_TRUE(singleRankColumn[0].has_value());
	EXPECT_EQ(std::get<int64_t>(singleRankColumn[0]->getVariant()), 42);

	std::vector<size_t> visitedRows;
	auto visited = visitSelectedPropertyValue(raw, "name", refs, 0, refs.size(),
											  [&](size_t row, const PropertyValue &value) {
												  visitedRows.push_back(row);
												  EXPECT_EQ(std::get<std::string>(value.getVariant()), "milo");
											  });
	ASSERT_TRUE(visited.has_value());
	EXPECT_EQ(*visited, 2U);
	EXPECT_EQ(visitedRows, (std::vector<size_t>{0U, 1U}));
	ASSERT_TRUE(visitSelectedPropertyValue(raw, "absent", refs, 0, refs.size(), [](size_t, const PropertyValue &) {})
						.has_value());

	auto scalarVisited = visitSelectedPropertyScalarValue(raw, "rank", refs, 0, 1,
														  [](size_t row, const PropertyEntityScalarValue &value) {
															  EXPECT_EQ(row, 0U);
															  EXPECT_EQ(value.type, PropertyType::INTEGER);
															  EXPECT_EQ(value.intValue, 42);
														  });
	ASSERT_TRUE(scalarVisited.has_value());
	EXPECT_EQ(*scalarVisited, 1U);
	auto scalarAbsent = visitSelectedPropertyScalarValue(raw, "absent", refs, 0, refs.size(),
														 [](size_t, const PropertyEntityScalarValue &) {});
	ASSERT_TRUE(scalarAbsent.has_value());
	EXPECT_EQ(*scalarAbsent, 0U);

	const std::string rankKey = "rank";
	const PropertyValue rankValue(int64_t{42});
	PredicateExpectation equality{&rankKey, compilePropertyValue(rankValue)};
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{equality}).value());
	SinglePredicateExpectation single{&rankKey, compilePropertyValue(rankValue)};
	EXPECT_TRUE(readPropertyEntitySinglePredicateMatch(raw, single).value());

	const std::string nameKey = "name";
	const PropertyValue lowerName("a");
	const PropertyValue upperName("z");
	std::vector<PredicateSpecExpectation> specs{
			predicateSpec(nameKey, lowerName, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &upperName),
			predicateSpec(nameKey, upperName, PropertyEntityPredicateOp::PEP_NE)};
	const auto groups = groupPredicateSpecExpectations(specs);
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw, groups, specs.size()).value());

	const std::string flagBytes = serializePropertyValueBytes(PropertyValue(true));
	const char *flagCursor = flagBytes.data();
	const char *flagEnd = flagCursor + flagBytes.size();
	EXPECT_TRUE(serializedPropertyValueEquals(flagCursor, flagEnd, compilePropertyValue(PropertyValue(true))).value());
}

TEST(PropertyEntityScanDetailTest, SkipsCompositeValuesAndRejectsMalformedContainerPayloads) {
	{
		const std::string bytes = serializePropertyValueBytes(PropertyValue(PropertyValue::MapType{
				{"rank", PropertyValue(int64_t{9})}, {"name", PropertyValue("milo")}}));
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_TRUE(skipPropertyValue(cursor, end));
		EXPECT_EQ(cursor, end);
	}
	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::MAP);
		utils::Serializer::writePOD(stream, uint32_t{1});
		std::string truncated = stream.str();
		const char *cursor = truncated.data();
		const char *end = cursor + truncated.size();
		EXPECT_FALSE(skipPropertyValue(cursor, end));
	}
	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::LIST);
		std::string truncated = stream.str();
		const char *cursor = truncated.data();
		const char *end = cursor + truncated.size();
		EXPECT_FALSE(skipPropertyValue(cursor, end));
	}
	{
		std::string integerBytes = serializePropertyValueBytes(PropertyValue(int64_t{7}));
		integerBytes.resize(sizeof(PropertyType) + 1);
		const char *cursor = integerBytes.data();
		const char *end = cursor + integerBytes.size();
		EXPECT_FALSE(skipPropertyValue(cursor, end));
	}
	{
		const char *base = "x";
		const char *cursor = base + 1;
		const char *end = base;
		EXPECT_FALSE(readSerializedPropertyValueFallback(cursor, end).has_value());
	}
}

TEST(PropertyEntityScanDetailTest, ReadsScalarValuesForAllInlineTypesAndFallbackContainers) {
	const std::vector<PropertyValue> inlineValues{
			PropertyValue(std::monostate{}),
			PropertyValue(false),
			PropertyValue(int64_t{17}),
			PropertyValue(1.5),
			PropertyValue("inline"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 13, 14, 15, 16)),
			PropertyValue(TemporalDuration::fromComponents(2, 3, 0, 4, 5, 6, 7)),
	};
	for (const auto &value: inlineValues) {
		const std::string bytes = serializePropertyValueBytes(value);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		std::optional<PropertyValue> fallback;
		auto scalar = readSerializedPropertyScalarValue(cursor, end, fallback);
		ASSERT_TRUE(scalar.has_value());
		EXPECT_EQ(scalar->type, value.getType());
		EXPECT_FALSE(fallback.has_value());
		EXPECT_EQ(cursor, end);
	}

	const PropertyValue listValue(std::vector<PropertyValue>{PropertyValue(int64_t{1}), PropertyValue("two")});
	const std::string listBytes = serializePropertyValueBytes(listValue);
	const char *cursor = listBytes.data();
	const char *end = cursor + listBytes.size();
	std::optional<PropertyValue> fallback;
	auto scalar = readSerializedPropertyScalarValue(cursor, end, fallback);
	ASSERT_TRUE(scalar.has_value());
	ASSERT_TRUE(fallback.has_value());
	EXPECT_EQ(*scalar->fallbackValue, listValue);
	EXPECT_EQ(cursor, end);

	std::string truncatedDuration = serializePropertyValueBytes(
			PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)));
	truncatedDuration.resize(sizeof(PropertyType) + sizeof(int64_t));
	const char *durationCursor = truncatedDuration.data();
	const char *durationEnd = durationCursor + truncatedDuration.size();
	std::optional<PropertyValue> unusedFallback;
	EXPECT_FALSE(readSerializedPropertyScalarValue(durationCursor, durationEnd, unusedFallback).has_value());
}

TEST(PropertyEntityScanDetailTest, SerializedEqualityCoversScalarContainersAndMismatches) {
	const std::vector<PropertyValue> values{
			PropertyValue(std::monostate{}),
			PropertyValue(false),
			PropertyValue(int64_t{17}),
			PropertyValue(1.5),
			PropertyValue("inline"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 13, 14, 15, 16)),
			PropertyValue(TemporalDuration::fromComponents(2, 3, 0, 4, 5, 6, 7)),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{1}), PropertyValue("two")}),
			PropertyValue(PropertyValue::MapType{{"k", PropertyValue("v")}}),
	};
	for (const auto &value: values) {
		const std::string bytes = serializePropertyValueBytes(value);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		const auto matches = serializedPropertyValueEquals(cursor, end, compilePropertyValue(value));
		ASSERT_TRUE(matches.has_value());
		EXPECT_TRUE(*matches);
		EXPECT_EQ(cursor, end);
	}

	const std::string intBytes = serializePropertyValueBytes(PropertyValue(int64_t{17}));
	const char *intCursor = intBytes.data();
	const char *intEnd = intCursor + intBytes.size();
	EXPECT_FALSE(serializedPropertyValueEquals(intCursor, intEnd, compilePropertyValue(PropertyValue("17"))).value());

	const std::string unknownBytes(1, static_cast<char>(PropertyType::UNKNOWN));
	const char *unknownCursor = unknownBytes.data();
	const char *unknownEnd = unknownCursor + unknownBytes.size();
	EXPECT_FALSE(serializedPropertyValueEquals(unknownCursor, unknownEnd,
											  compilePropertyValue(PropertyValue(std::monostate{})))
						 .value());
}

TEST(PropertyEntityScanDetailTest, PredicateSpecsCoverTypedAndFallbackOperators) {
	const std::string key = "rank";
	const PropertyValue one(int64_t{1});
	const PropertyValue two(int64_t{2});
	const PropertyValue three(int64_t{3});
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, two, PropertyEntityPredicateOp::PEP_EQ)));
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, three, PropertyEntityPredicateOp::PEP_LE)));
	EXPECT_TRUE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{2}),
											   predicateSpec(key, one, PropertyEntityPredicateOp::PEP_GE)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, two, PropertyEntityPredicateOp::PEP_EQ)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, one, PropertyEntityPredicateOp::PEP_NE)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, three, PropertyEntityPredicateOp::PEP_LT)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, two, PropertyEntityPredicateOp::PEP_LE)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, one, PropertyEntityPredicateOp::PEP_GT)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(2, predicateSpec(key, one, PropertyEntityPredicateOp::PEP_GE)));
	EXPECT_TRUE(typedValueSatisfiesPredicate<int64_t>(
			2, predicateSpec(key, one, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &three)));
	EXPECT_FALSE(typedValueSatisfiesPredicate<int64_t>(2,
													  predicateSpec(key, one, PropertyEntityPredicateOp::PEP_RANGE_CLOSED)));
	const PropertyValue upperText("three");
	EXPECT_FALSE(typedValueSatisfiesPredicate<int64_t>(
			2, predicateSpec(key, one, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &upperText)));
	EXPECT_FALSE(typedValueSatisfiesPredicate<int64_t>(
			2, predicateSpec(key, upperText, PropertyEntityPredicateOp::PEP_EQ)));

	const std::string text = "m";
	SerializedStringView view{text.data(), static_cast<uint32_t>(text.size())};
	const PropertyValue same("m");
	const PropertyValue before("a");
	const PropertyValue after("z");
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, same, PropertyEntityPredicateOp::PEP_EQ)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, before, PropertyEntityPredicateOp::PEP_NE)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, after, PropertyEntityPredicateOp::PEP_LT)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, same, PropertyEntityPredicateOp::PEP_LE)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, before, PropertyEntityPredicateOp::PEP_GT)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view, predicateSpec(key, before, PropertyEntityPredicateOp::PEP_GE)));
	EXPECT_TRUE(stringViewSatisfiesPredicate(view,
											 predicateSpec(key, before, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
														   &after)));
	EXPECT_FALSE(
			stringViewSatisfiesPredicate(view, predicateSpec(key, before, PropertyEntityPredicateOp::PEP_RANGE_CLOSED)));
	const PropertyValue upperInt(int64_t{9});
	EXPECT_FALSE(stringViewSatisfiesPredicate(
			view, predicateSpec(key, before, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &upperInt)));
	EXPECT_FALSE(stringViewSatisfiesPredicate(
			view, predicateSpec(key, upperInt, PropertyEntityPredicateOp::PEP_EQ)));

	const PropertyValue earlierDate(TemporalDate{10});
	const PropertyValue laterDate(TemporalDate{30});
	EXPECT_TRUE(typedValueSatisfiesPredicate(TemporalDate{20},
											 predicateSpec(key, earlierDate, PropertyEntityPredicateOp::PEP_GE)));
	EXPECT_TRUE(typedValueSatisfiesPredicate(TemporalDate{20},
											 predicateSpec(key, earlierDate, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
														   &laterDate)));

	const PropertyValue earlierDateTime(TemporalDateTime{100});
	const PropertyValue currentDateTime(TemporalDateTime{200});
	const PropertyValue laterDateTime(TemporalDateTime{300});
	EXPECT_TRUE(typedValueSatisfiesPredicate(
			TemporalDateTime{200}, predicateSpec(key, currentDateTime, PropertyEntityPredicateOp::PEP_EQ)));
	EXPECT_TRUE(typedValueSatisfiesPredicate(
			TemporalDateTime{200},
			predicateSpec(key, earlierDateTime, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &laterDateTime)));

	const PropertyValue shorterDuration(TemporalDuration{0, 1, 0});
	const PropertyValue currentDuration(TemporalDuration{0, 2, 0});
	const PropertyValue longerDuration(TemporalDuration{0, 3, 0});
	EXPECT_TRUE(typedValueSatisfiesPredicate(
			TemporalDuration{0, 2, 0}, predicateSpec(key, currentDuration, PropertyEntityPredicateOp::PEP_EQ)));
	EXPECT_TRUE(typedValueSatisfiesPredicate(
			TemporalDuration{0, 2, 0},
			predicateSpec(key, shorterDuration, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &longerDuration)));
}

TEST(PropertyEntityScanDetailTest, RejectsTruncatedScalarAndContainerHelpers) {
	const std::string key = "rank";
	const PropertyValue value(int64_t{1});
	const auto spec = predicateSpec(key, value, PropertyEntityPredicateOp::PEP_EQ);

	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::LIST);
		utils::Serializer::writePOD(stream, uint32_t{1});
		std::string bytes = stream.str();
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_FALSE(readSerializedPropertyValue(cursor, end).has_value());
	}
	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::MAP);
		utils::Serializer::writePOD(stream, uint32_t{1});
		std::string bytes = stream.str();
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_FALSE(readSerializedPropertyValue(cursor, end).has_value());
	}
	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::STRING);
		utils::Serializer::writePOD(stream, uint32_t{8});
		stream.write("abc", 3);
		std::string bytes = stream.str();
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		SerializedStringView view;
		EXPECT_FALSE(readStringView(cursor, end, view));
		cursor = bytes.data();
		EXPECT_FALSE(serializedPropertyValueEquals(cursor, end, compilePropertyValue(PropertyValue("abc"))).has_value());
		cursor = bytes.data();
		EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(cursor, end, spec).has_value());
	}
	{
		const PropertyValue dateValue(TemporalDate::fromYMD(2026, 6, 2));
		std::string bytes = serializePropertyValueBytes(dateValue);
		bytes.resize(sizeof(PropertyType));
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_FALSE(serializedPropertyValueEquals(cursor, end, compilePropertyValue(dateValue)).has_value());
	}
	{
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, PropertyType::MAP);
		utils::Serializer::writePOD(stream, uint32_t{1});
		utils::Serializer::writePOD(stream, uint32_t{64});
		std::string bytes = stream.str();
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_FALSE(skipPropertyValue(cursor, end));
	}
}

TEST(PropertyEntityScanDetailTest, PredicateReadersHandleMissingKeysAndMismatches) {
	Property property;
	property.setId(91);
	property.getMutableMetadata().entityId = 15;
	property.getMutableMetadata().entityType = Node::typeId;
	property.getMutableMetadata().isActive = true;
	property.setProperties({{"rank", PropertyValue(int64_t{42})}, {"name", PropertyValue("milo")}});
	auto buffer = utils::FixedSizeSerializer::serializeToBuffer(property, Property::TOTAL_PROPERTY_SIZE);
	const char *raw = buffer.data();

	const std::string rankKey = "rank";
	const std::string missingKey = "missing";
	const PropertyValue rankValue(int64_t{42});
	const PropertyValue wrongRank(int64_t{7});
	PredicateExpectation expectedRank{&rankKey, compilePropertyValue(rankValue)};
	PredicateExpectation expectedMissing{&missingKey, compilePropertyValue(rankValue)};
	PredicateExpectation expectedWrongRank{&rankKey, compilePropertyValue(wrongRank)};
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{expectedRank}).value());
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{expectedMissing}).value());
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{expectedWrongRank}).value());

	SinglePredicateExpectation wrongSingle{&rankKey, compilePropertyValue(wrongRank)};
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(raw, wrongSingle).value());
	SinglePredicateExpectation missingSingle{&missingKey, compilePropertyValue(rankValue)};
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(raw, missingSingle).value());

	const PropertyValue lower(int64_t{40});
	const PropertyValue upper(int64_t{50});
	const PropertyValue tooLow(int64_t{100});
	std::vector<PredicateSpecExpectation> matchingSpecs{
			predicateSpec(rankKey, lower, PropertyEntityPredicateOp::PEP_GE),
			predicateSpec(rankKey, upper, PropertyEntityPredicateOp::PEP_LE)};
	auto matchingGroups = groupPredicateSpecExpectations(matchingSpecs);
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw, matchingGroups, matchingSpecs.size()).value());
	EXPECT_TRUE(readPropertyEntitySinglePredicateSpecMatch(
						raw, predicateSpec(rankKey, lower, PropertyEntityPredicateOp::PEP_GE))
						.value());
	EXPECT_TRUE(readPropertyEntitySinglePredicateSpecMatch(
						raw, predicateSpec(rankKey, lower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &upper))
						.value());
	EXPECT_FALSE(readPropertyEntitySinglePredicateSpecMatch(
						 raw, predicateSpec(missingKey, lower, PropertyEntityPredicateOp::PEP_GE))
						 .value());

	std::vector<PredicateSpecExpectation> failingSpecs{
			predicateSpec(rankKey, tooLow, PropertyEntityPredicateOp::PEP_GT)};
	auto failingGroups = groupPredicateSpecExpectations(failingSpecs);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, failingGroups, failingSpecs.size()).value());
	EXPECT_FALSE(readPropertyEntitySinglePredicateSpecMatch(raw, failingSpecs.front()).value());
	std::vector<PredicateSpecExpectation> missingSpec{
			predicateSpec(missingKey, lower, PropertyEntityPredicateOp::PEP_GE)};
	auto missingGroups = groupPredicateSpecExpectations(missingSpec);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, missingGroups, missingSpec.size()).value());
	std::string invalidRecord(Property::TOTAL_PROPERTY_SIZE, '\0');
	EXPECT_FALSE(readPropertyEntitySinglePredicateSpecMatch(invalidRecord.data(), missingSpec.front()).has_value());

	const std::string boolBytes = serializePropertyValueBytes(PropertyValue(true));
	const char *boolCursor = boolBytes.data();
	const char *boolEnd = boolCursor + boolBytes.size();
	EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(boolCursor, boolEnd,
															   predicateSpec(rankKey, rankValue,
																			 PropertyEntityPredicateOp::PEP_EQ))
						 .value());

	const PropertyValue dateLower(TemporalDate{40});
	const PropertyValue dateUpper(TemporalDate{50});
	const std::string dateBytes = serializePropertyValueBytes(PropertyValue(TemporalDate{42}));
	const char *dateCursor = dateBytes.data();
	const char *dateEnd = dateCursor + dateBytes.size();
	EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
						dateCursor, dateEnd,
						predicateSpec(rankKey, dateLower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &dateUpper))
						.value());
	EXPECT_EQ(dateCursor, dateEnd);

	const PropertyValue dateTimeLower(TemporalDateTime{40});
	const std::string dateTimeBytes = serializePropertyValueBytes(PropertyValue(TemporalDateTime{42}));
	const char *dateTimeCursor = dateTimeBytes.data();
	const char *dateTimeEnd = dateTimeCursor + dateTimeBytes.size();
	EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
						dateTimeCursor, dateTimeEnd,
						predicateSpec(rankKey, dateTimeLower, PropertyEntityPredicateOp::PEP_GE))
						.value());
	EXPECT_EQ(dateTimeCursor, dateTimeEnd);

	const PropertyValue durationLower(TemporalDuration{0, 1, 0});
	const PropertyValue durationUpper(TemporalDuration{0, 3, 0});
	const std::string durationBytes = serializePropertyValueBytes(PropertyValue(TemporalDuration{0, 2, 0}));
	const char *durationCursor = durationBytes.data();
	const char *durationEnd = durationCursor + durationBytes.size();
	EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
						durationCursor, durationEnd,
						predicateSpec(rankKey, durationLower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
									  &durationUpper))
						.value());
	EXPECT_EQ(durationCursor, durationEnd);
}

TEST(PropertyEntityScanDetailTest, PredicateReadersStopAfterAllRequestedPredicatesMatch) {
	const std::string rankKey = "rank";
	const std::string nameKey = "name";
	const std::string brokenKey = "broken";
	const PropertyValue rankValue(int64_t{42});
	const PropertyValue nameValue("milo");
	const auto raw = makeRawPropertyRecord({
			{rankKey, serializePropertyValueBytes(rankValue)},
			{nameKey, serializePropertyValueBytes(nameValue)},
			{brokenKey, malformedStringValueBytes()},
	});

	std::vector<PredicateExpectation> equalityPredicates{
			PredicateExpectation{&rankKey, compilePropertyValue(rankValue)},
			PredicateExpectation{&nameKey, compilePropertyValue(nameValue)},
	};
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw.data(), equalityPredicates).value());

	const PropertyValue rankLower(int64_t{40});
	const PropertyValue nameUpper("z");
	std::vector<PredicateSpecExpectation> specPredicates{
			predicateSpec(rankKey, rankLower, PropertyEntityPredicateOp::PEP_GE),
			predicateSpec(nameKey, nameUpper, PropertyEntityPredicateOp::PEP_LE),
	};
	const auto groups = groupPredicateSpecExpectations(specPredicates);
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw.data(), groups, specPredicates.size()).value());
}

TEST(PropertyEntityScanDetailTest, ColumnVisitorsAndOwnerReadersHandleMalformedRecords) {
	const std::string rankKey = "rank";
	const std::string nameKey = "name";
	const PropertyValue rankValue(int64_t{42});
	const PropertyValue nameValue("milo");
	const auto raw = makeRawPropertyRecordForOwner(
			99, 11, Node::typeId, true,
			{{rankKey, serializePropertyValueBytes(rankValue)}, {nameKey, serializePropertyValueBytes(nameValue)}});

	std::vector<PropertyEntityRowRef> refs{{99, 0}};
	std::vector<std::optional<PropertyValue>> rankColumn(2);
	std::unordered_map<std::string, size_t> requested{{rankKey, 0}};
	std::vector<std::vector<std::optional<PropertyValue>> *> columns{&rankColumn};
	EXPECT_TRUE(readSelectedPropertyColumns(raw.data(), requested, columns, refs, 0, refs.size()));
	ASSERT_TRUE(rankColumn[0].has_value());
	EXPECT_EQ(rankColumn[0].value(), rankValue);
	EXPECT_TRUE(readSelectedPropertyColumnsOne(raw.data(), requested, columns, 1));
	ASSERT_TRUE(rankColumn[1].has_value());
	EXPECT_EQ(rankColumn[1].value(), rankValue);

	size_t valueVisitCount = 0;
	EXPECT_EQ(visitSelectedPropertyValueOne(raw.data(), nameKey, 1,
											[&](size_t row, const PropertyValue &value) {
												++valueVisitCount;
												EXPECT_EQ(row, 1U);
												EXPECT_EQ(value, nameValue);
											})
					  .value(),
			  1U);
	EXPECT_EQ(valueVisitCount, 1U);
	EXPECT_EQ(visitSelectedPropertyValueOne(raw.data(), "missing", 1, [](size_t, const PropertyValue &) {}).value(),
			  0U);

	size_t scalarVisitCount = 0;
	EXPECT_EQ(visitSelectedPropertyScalarValueOne(raw.data(), rankKey, 1,
												  [&](size_t row, const PropertyEntityScalarValue &value) {
													  ++scalarVisitCount;
													  EXPECT_EQ(row, 1U);
													  EXPECT_EQ(value.type, PropertyType::INTEGER);
													  EXPECT_EQ(value.intValue, 42);
												  })
					  .value(),
			  1U);
	EXPECT_EQ(scalarVisitCount, 1U);
	EXPECT_EQ(visitSelectedPropertyScalarValueOne(raw.data(), "missing", 1,
												  [](size_t, const PropertyEntityScalarValue &) {})
					  .value(),
			  0U);

	const std::vector<int64_t> allOwners;
	auto ownerValue = readPropertyOwnerValue(raw.data(), EntityType::Node, rankKey, allOwners);
	ASSERT_TRUE(ownerValue.has_value());
	EXPECT_EQ(ownerValue->ownerId, 11);
	EXPECT_EQ(ownerValue->value, rankValue);
	EXPECT_FALSE(readPropertyOwnerValue(raw.data(), EntityType::Edge, rankKey, allOwners).has_value());
	const std::vector<int64_t> otherOwners{99};
	EXPECT_FALSE(readPropertyOwnerValue(raw.data(), EntityType::Node, rankKey, otherOwners).has_value());
	EXPECT_FALSE(readPropertyOwnerValue(raw.data(), EntityType::Node, "missing", allOwners).has_value());

	const std::vector<std::string> noKeys;
	EXPECT_TRUE(readPropertyOwnerKeyValues(raw.data(), EntityType::Node, noKeys, allOwners).empty());
	const std::vector<std::string> keys{rankKey, nameKey, "missing"};
	auto ownerKeyValues = readPropertyOwnerKeyValues(raw.data(), EntityType::Node, keys, allOwners);
	ASSERT_EQ(ownerKeyValues.size(), 2U);
	EXPECT_EQ(ownerKeyValues[0].ownerId, 11);
	EXPECT_EQ(ownerKeyValues[0].key, rankKey);
	EXPECT_EQ(ownerKeyValues[0].value, rankValue);
	EXPECT_EQ(ownerKeyValues[1].key, nameKey);
	EXPECT_EQ(ownerKeyValues[1].value, nameValue);
	EXPECT_TRUE(readPropertyOwnerKeyValues(raw.data(), EntityType::Edge, keys, allOwners).empty());
	EXPECT_TRUE(readPropertyOwnerKeyValues(raw.data(), EntityType::Node, keys, otherOwners).empty());
	const auto ownerWithSkippedKey = makeRawPropertyRecordForOwner(
			100, 11, Node::typeId, true,
			{{"other", serializePropertyValueBytes(PropertyValue(int64_t{7}))},
			 {rankKey, serializePropertyValueBytes(rankValue)}});
	auto skippedOwnerKeyValues = readPropertyOwnerKeyValues(ownerWithSkippedKey.data(), EntityType::Node, keys, allOwners);
	ASSERT_EQ(skippedOwnerKeyValues.size(), 1U);
	EXPECT_EQ(skippedOwnerKeyValues.front().key, rankKey);
	EXPECT_EQ(skippedOwnerKeyValues.front().value, rankValue);

	const std::string invalidRecord(Property::TOTAL_PROPERTY_SIZE, '\0');
	EXPECT_FALSE(readSelectedPropertyColumns(invalidRecord.data(), requested, columns, refs, 0, refs.size()));
	EXPECT_FALSE(readSelectedPropertyColumnsOne(invalidRecord.data(), requested, columns, 0));
	EXPECT_FALSE(visitSelectedPropertyValueOne(invalidRecord.data(), rankKey, 0, [](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValueOne(invalidRecord.data(), rankKey, 0,
													 [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());
	EXPECT_FALSE(readPropertyOwnerValue(invalidRecord.data(), EntityType::Node, rankKey, allOwners).has_value());
	EXPECT_TRUE(readPropertyOwnerKeyValues(invalidRecord.data(), EntityType::Node, keys, allOwners).empty());

	const auto malformedKeyRecord = makeRawPropertyRecordWithMalformedKey();
	EXPECT_FALSE(readSelectedPropertyColumns(malformedKeyRecord.data(), requested, columns, refs, 0, refs.size()));
	EXPECT_FALSE(readSelectedPropertyColumnsOne(malformedKeyRecord.data(), requested, columns, 0));
	EXPECT_FALSE(visitSelectedPropertyValue(malformedKeyRecord.data(), rankKey, refs, 0, refs.size(),
											[](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyValueOne(malformedKeyRecord.data(), rankKey, 0,
											   [](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValueOne(malformedKeyRecord.data(), rankKey, 0,
													 [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());
	EXPECT_FALSE(readPropertyOwnerValue(malformedKeyRecord.data(), EntityType::Node, rankKey, allOwners).has_value());
	EXPECT_TRUE(readPropertyOwnerKeyValues(malformedKeyRecord.data(), EntityType::Node, keys, allOwners).empty());
	PredicateExpectation equality{&rankKey, compilePropertyValue(rankValue)};
	EXPECT_FALSE(readPropertyEntityPredicateMatch(malformedKeyRecord.data(), std::vector<PredicateExpectation>{equality})
						 .has_value());
	SinglePredicateExpectation singleEquality{&rankKey, compilePropertyValue(rankValue)};
	EXPECT_FALSE(readPropertyEntitySinglePredicateMatch(malformedKeyRecord.data(), singleEquality).has_value());
	std::vector<PredicateSpecExpectation> malformedKeySpecs{
			predicateSpec(rankKey, rankValue, PropertyEntityPredicateOp::PEP_EQ)};
	auto malformedKeyGroups = groupPredicateSpecExpectations(malformedKeySpecs);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(malformedKeyRecord.data(), malformedKeyGroups,
												 malformedKeySpecs.size())
						 .has_value());

	const auto malformedRequestedValue =
			makeRawPropertyRecordForOwner(101, 11, Node::typeId, true, {{rankKey, malformedStringValueBytes()}});
	EXPECT_FALSE(readSelectedPropertyColumnsOne(malformedRequestedValue.data(), requested, columns, 0));
	EXPECT_FALSE(visitSelectedPropertyValueOne(malformedRequestedValue.data(), rankKey, 0,
											   [](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(readPropertyOwnerValue(malformedRequestedValue.data(), EntityType::Node, rankKey, allOwners)
						 .has_value());
	EXPECT_TRUE(readPropertyOwnerKeyValues(malformedRequestedValue.data(), EntityType::Node, keys, allOwners).empty());

	const auto malformedSkippedValue =
			makeRawPropertyRecordForOwner(102, 11, Node::typeId, true, {{"other", malformedStringValueBytes()}});
	EXPECT_FALSE(readSelectedPropertyColumns(malformedSkippedValue.data(), requested, columns, refs, 0, refs.size()));
	EXPECT_FALSE(readSelectedPropertyColumnsOne(malformedSkippedValue.data(), requested, columns, 0));
	EXPECT_FALSE(visitSelectedPropertyScalarValue(malformedSkippedValue.data(), rankKey, refs, 0, refs.size(),
												  [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyValueOne(malformedSkippedValue.data(), rankKey, 0,
											   [](size_t, const PropertyValue &) {})
						 .has_value());
	EXPECT_FALSE(visitSelectedPropertyScalarValueOne(malformedSkippedValue.data(), rankKey, 0,
													 [](size_t, const PropertyEntityScalarValue &) {})
						 .has_value());
	EXPECT_FALSE(readPropertyOwnerValue(malformedSkippedValue.data(), EntityType::Node, rankKey, allOwners)
						 .has_value());
	EXPECT_TRUE(readPropertyOwnerKeyValues(malformedSkippedValue.data(), EntityType::Node, keys, allOwners).empty());
}

TEST(PropertyEntityScanDetailTest, PredicateComparisonHelpersCoverBoundaryBranches) {
	const std::string key = "rank";
	const PropertyValue lower(int64_t{10});
	const PropertyValue upper(int64_t{20});
	const PropertyValue textLower("b");
	const PropertyValue textUpper("d");

	EXPECT_FALSE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{5}),
												predicateSpec(key, lower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
															  &upper)));
	EXPECT_FALSE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{25}),
												predicateSpec(key, lower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED,
															  &upper)));
	auto invalidOp = predicateSpec(key, lower, static_cast<PropertyEntityPredicateOp>(999));
	EXPECT_FALSE(propertyValueSatisfiesPredicate(PropertyValue(int64_t{15}), invalidOp));
	EXPECT_FALSE(typedValueSatisfiesPredicate<int64_t>(15, invalidOp));

	const std::string emptyText;
	SerializedStringView emptyView{emptyText.data(), 0};
	EXPECT_EQ(compareStringView(emptyView, ""), 0);
	EXPECT_LT(compareStringView(emptyView, "x"), 0);
	const std::string longerText = "abcd";
	SerializedStringView longerView{longerText.data(), static_cast<uint32_t>(longerText.size())};
	EXPECT_GT(compareStringView(longerView, "abc"), 0);
	EXPECT_FALSE(stringViewSatisfiesPredicate(
			SerializedStringView{longerText.data(), 1},
			predicateSpec(key, textLower, PropertyEntityPredicateOp::PEP_RANGE_CLOSED, &textUpper)));
	EXPECT_FALSE(stringViewSatisfiesPredicate(longerView, predicateSpec(key, textLower,
																		static_cast<PropertyEntityPredicateOp>(999))));

	const std::string stringBytes = serializePropertyValueBytes(PropertyValue("value"));
	const char *stringCursor = stringBytes.data();
	const char *stringEnd = stringCursor + stringBytes.size();
	CompiledPropertyValue missingStringPointer;
	missingStringPointer.type = PropertyType::STRING;
	EXPECT_FALSE(serializedPropertyValueEquals(stringCursor, stringEnd, missingStringPointer).value());

	const PropertyValue listValue(std::vector<PropertyValue>{PropertyValue(int64_t{1})});
	const std::string listBytes = serializePropertyValueBytes(listValue);
	const char *listCursor = listBytes.data();
	const char *listEnd = listCursor + listBytes.size();
	CompiledPropertyValue missingFallbackPointer;
	missingFallbackPointer.type = PropertyType::LIST;
	EXPECT_FALSE(serializedPropertyValueEquals(listCursor, listEnd, missingFallbackPointer).value());

	std::ostringstream truncatedListStream(std::ios::binary);
	utils::Serializer::writePOD(truncatedListStream, PropertyType::LIST);
	utils::Serializer::writePOD(truncatedListStream, uint32_t{1});
	std::string truncatedListBytes = truncatedListStream.str();
	const char *truncatedListCursor = truncatedListBytes.data();
	const char *truncatedListEnd = truncatedListCursor + truncatedListBytes.size();
	EXPECT_FALSE(serializedPropertyValueEquals(truncatedListCursor, truncatedListEnd, compilePropertyValue(listValue))
						 .has_value());

	std::ostringstream unknownStream(std::ios::binary);
	utils::Serializer::writePOD(unknownStream, PropertyType::UNKNOWN);
	std::string unknownBytes = unknownStream.str();
	const char *unknownCursor = unknownBytes.data();
	const char *unknownEnd = unknownCursor + unknownBytes.size();
	CompiledPropertyValue unknownExpectation;
	unknownExpectation.type = PropertyType::UNKNOWN;
	EXPECT_FALSE(serializedPropertyValueEquals(unknownCursor, unknownEnd, unknownExpectation).has_value());
}

TEST(PropertyEntityScanDetailTest, PredicateReadersRejectTruncatedTypedPayloads) {
	const std::string key = "rank";
	const std::vector<PropertyValue> values{
			PropertyValue(true),
			PropertyValue(int64_t{42}),
			PropertyValue(4.25),
			PropertyValue("milo"),
			PropertyValue(TemporalDate::fromYMD(2026, 6, 2)),
			PropertyValue(TemporalDateTime::fromComponents(2026, 6, 2, 12, 0, 0, 0)),
			PropertyValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6)),
	};
	for (const auto &value: values) {
		std::string bytes = serializePropertyValueBytes(value);
		ASSERT_GT(bytes.size(), sizeof(PropertyType));
		bytes.pop_back();
		const auto spec = predicateSpec(key, value, PropertyEntityPredicateOp::PEP_EQ);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(cursor, end, spec).has_value());
	}

	{
		const PropertyValue boolValue(true);
		const std::string bytes = serializePropertyValueBytes(boolValue);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
							cursor, end, predicateSpec(key, boolValue, PropertyEntityPredicateOp::PEP_EQ))
							.value());
	}
	{
		const PropertyValue doubleValue(4.25);
		const std::string bytes = serializePropertyValueBytes(doubleValue);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
							cursor, end, predicateSpec(key, doubleValue, PropertyEntityPredicateOp::PEP_EQ))
							.value());
	}
	{
		const PropertyValue listValue(std::vector<PropertyValue>{PropertyValue(int64_t{1})});
		const std::string bytes = serializePropertyValueBytes(listValue);
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		EXPECT_TRUE(readSerializedPropertyValueSatisfiesPredicate(
							cursor, end, predicateSpec(key, listValue, PropertyEntityPredicateOp::PEP_EQ))
							.value());
		EXPECT_EQ(cursor, end);
	}

	const PropertyValue durationValue(TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6));
	for (size_t componentCount = 0; componentCount < 3; ++componentCount) {
		const std::string bytes = truncatedDurationValueBytes(componentCount);
		const auto spec = predicateSpec(key, durationValue, PropertyEntityPredicateOp::PEP_EQ);
		const char *predicateCursor = bytes.data();
		const char *predicateEnd = predicateCursor + bytes.size();
		EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(predicateCursor, predicateEnd, spec).has_value());

		const char *equalsCursor = bytes.data();
		const char *equalsEnd = equalsCursor + bytes.size();
		EXPECT_FALSE(serializedPropertyValueEquals(equalsCursor, equalsEnd, compilePropertyValue(durationValue))
							 .has_value());
	}

	for (const auto type: {PropertyType::COMPOSITE, PropertyType::UNKNOWN}) {
		std::ostringstream stream(std::ios::binary);
		utils::Serializer::writePOD(stream, type);
		const std::string bytes = stream.str();
		const char *cursor = bytes.data();
		const char *end = cursor + bytes.size();
		std::optional<PropertyValue> fallback;
		EXPECT_FALSE(readSerializedPropertyScalarValue(cursor, end, fallback).has_value());
	}
}

} // namespace graph::storage
