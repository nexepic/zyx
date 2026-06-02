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

#include <sstream>
#include <string>
#include <vector>

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

std::string makeRawPropertyRecord(const std::vector<std::pair<std::string, std::string>> &entries) {
	std::ostringstream stream(std::ios::binary);
	utils::Serializer::writePOD(stream, int64_t{99});
	utils::Serializer::writePOD(stream, int64_t{7});
	utils::Serializer::writePOD(stream, Node::typeId);
	utils::Serializer::writePOD(stream, true);
	utils::Serializer::writePOD(stream, static_cast<uint32_t>(entries.size()));
	for (const auto &[key, valueBytes] : entries) {
		utils::Serializer::serialize<std::string>(stream, key);
		stream.write(valueBytes.data(), static_cast<std::streamsize>(valueBytes.size()));
	}
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
	EXPECT_TRUE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{expectedRank}).value());
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, std::vector<PredicateExpectation>{expectedMissing}).value());

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

	std::vector<PredicateSpecExpectation> failingSpecs{
			predicateSpec(rankKey, tooLow, PropertyEntityPredicateOp::PEP_GT)};
	auto failingGroups = groupPredicateSpecExpectations(failingSpecs);
	EXPECT_FALSE(readPropertyEntityPredicateMatch(raw, failingGroups, failingSpecs.size()).value());

	const std::string boolBytes = serializePropertyValueBytes(PropertyValue(true));
	const char *boolCursor = boolBytes.data();
	const char *boolEnd = boolCursor + boolBytes.size();
	EXPECT_FALSE(readSerializedPropertyValueSatisfiesPredicate(boolCursor, boolEnd,
															   predicateSpec(rankKey, rankValue,
																			 PropertyEntityPredicateOp::PEP_EQ))
						 .value());
}

} // namespace graph::storage
