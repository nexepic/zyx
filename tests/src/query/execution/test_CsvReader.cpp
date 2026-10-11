/**
 * @file test_CsvReader.cpp
 * @brief Unit tests for CsvReader RFC 4180-compliant CSV parser.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "graph/query/execution/CsvReader.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"

namespace fs = std::filesystem;
using namespace graph::query::execution;

class CsvReaderTest : public ::testing::Test {
protected:
	std::vector<fs::path> tempFiles;

	std::string createTempFile(const std::string &content) {
		auto uuid = boost::uuids::random_generator()();
		auto path = fs::temp_directory_path() / ("test_csv_reader_" + boost::uuids::to_string(uuid) + ".csv");
		std::ofstream out(path);
		out << content;
		out.close();
		tempFiles.push_back(path);
		return path.string();
	}

	void TearDown() override {
		std::error_code ec;
		for (auto &f : tempFiles) {
			if (fs::exists(f)) fs::remove(f, ec);
		}
	}
};

TEST_F(CsvReaderTest, CannotOpenFile) {
	EXPECT_THROW(CsvReader("/nonexistent/path/file.csv"), std::runtime_error);
}

TEST_F(CsvReaderTest, EmptyFile) {
	auto path = createTempFile("");
	CsvReader reader(path);
	EXPECT_FALSE(reader.hasNext());
}

TEST_F(CsvReaderTest, SingleLineNoNewline) {
	auto path = createTempFile("hello,world");
	CsvReader reader(path);
	ASSERT_TRUE(reader.hasNext());
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "hello");
	EXPECT_EQ(row[1], "world");
	EXPECT_FALSE(reader.hasNext());
}

TEST_F(CsvReaderTest, MultipleRows) {
	auto path = createTempFile("a,b\nc,d\ne,f\n");
	CsvReader reader(path);

	ASSERT_TRUE(reader.hasNext());
	auto row1 = reader.nextRow();
	EXPECT_EQ(row1[0], "a");
	EXPECT_EQ(row1[1], "b");

	ASSERT_TRUE(reader.hasNext());
	auto row2 = reader.nextRow();
	EXPECT_EQ(row2[0], "c");
	EXPECT_EQ(row2[1], "d");

	ASSERT_TRUE(reader.hasNext());
	auto row3 = reader.nextRow();
	EXPECT_EQ(row3[0], "e");
	EXPECT_EQ(row3[1], "f");

	EXPECT_FALSE(reader.hasNext());
}

TEST_F(CsvReaderTest, QuotedFieldWithComma) {
	auto path = createTempFile("\"a,b\",c\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "a,b");
	EXPECT_EQ(row[1], "c");
}

TEST_F(CsvReaderTest, QuotedFieldWithNewline) {
	auto path = createTempFile("\"line1\nline2\",c\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "line1\nline2");
	EXPECT_EQ(row[1], "c");
}

TEST_F(CsvReaderTest, EscapedQuote) {
	auto path = createTempFile("\"say \"\"hello\"\"\",b\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "say \"hello\"");
	EXPECT_EQ(row[1], "b");
}

TEST_F(CsvReaderTest, CustomDelimiter) {
	auto path = createTempFile("a\tb\tc\n");
	CsvReader reader(path, "\t");
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 3u);
	EXPECT_EQ(row[0], "a");
	EXPECT_EQ(row[1], "b");
	EXPECT_EQ(row[2], "c");
}

TEST_F(CsvReaderTest, HasNextIsPeeked) {
	auto path = createTempFile("a,b\n");
	CsvReader reader(path);
	// Call hasNext twice - second should return peeked result
	EXPECT_TRUE(reader.hasNext());
	EXPECT_TRUE(reader.hasNext());
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "a");
}

TEST_F(CsvReaderTest, NextRowWithoutHasNextPeek) {
	auto path = createTempFile("x,y\n");
	CsvReader reader(path);
	// Call nextRow directly without hasNext
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "x");
	EXPECT_EQ(row[1], "y");
}

TEST_F(CsvReaderTest, NextRowOnExhaustedReaderThrows) {
	auto path = createTempFile("a\n");
	CsvReader reader(path);
	reader.nextRow(); // consume the only row
	EXPECT_THROW(reader.nextRow(), std::runtime_error);
}

TEST_F(CsvReaderTest, Reset) {
	auto path = createTempFile("a,b\nc,d\n");
	CsvReader reader(path);
	reader.nextRow();
	reader.nextRow();
	EXPECT_FALSE(reader.hasNext());

	reader.reset();
	ASSERT_TRUE(reader.hasNext());
	auto row = reader.nextRow();
	EXPECT_EQ(row[0], "a");
}

TEST_F(CsvReaderTest, EmptyFieldsPreserved) {
	auto path = createTempFile(",\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "");
	EXPECT_EQ(row[1], "");
}

TEST_F(CsvReaderTest, SingleEmptyLine) {
	auto path = createTempFile("\n");
	CsvReader reader(path);
	// An empty line produces a single empty-string field
	ASSERT_TRUE(reader.hasNext());
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 1u);
	EXPECT_EQ(row[0], "");
}

TEST_F(CsvReaderTest, MultiCharDelimiter) {
	auto path = createTempFile("a::b::c\n");
	CsvReader reader(path, "::");
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 3u);
	EXPECT_EQ(row[0], "a");
	EXPECT_EQ(row[1], "b");
	EXPECT_EQ(row[2], "c");
}

TEST_F(CsvReaderTest, QuotedFieldFollowedByDelimiter) {
	auto path = createTempFile("\"quoted\",unquoted\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "quoted");
	EXPECT_EQ(row[1], "unquoted");
}

TEST_F(CsvReaderTest, QuoteInMiddleOfFieldTreatedAsLiteral) {
	// A quote char mid-field when field is non-empty: not a start-quote
	auto path = createTempFile("ab\"cd,ef\n");
	CsvReader reader(path);
	auto row = reader.nextRow();
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], "ab\"cd");
	EXPECT_EQ(row[1], "ef");
}

TEST_F(CsvReaderTest, RejectsInvalidQuotedRecordsAndResourceBoundExhaustion) {
	auto unterminated = createTempFile("\"unterminated\n");
	CsvReader invalidReader(unterminated);
	EXPECT_THROW(static_cast<void>(invalidReader.hasNext()), std::runtime_error);

	auto oversizedField = createTempFile("abcdef\n");
	CsvReader fieldReader(oversizedField, ",", {.maximum_record_bytes = 16U, .maximum_columns = 2U,
																 .maximum_field_bytes = 4U});
	EXPECT_THROW(static_cast<void>(fieldReader.nextRow()), std::runtime_error);

	auto tooManyColumns = createTempFile("a,b,c\n");
	CsvReader columnReader(tooManyColumns, ",", {.maximum_record_bytes = 16U, .maximum_columns = 2U,
																 .maximum_field_bytes = 8U});
	EXPECT_THROW(static_cast<void>(columnReader.nextRow()), std::runtime_error);
}

TEST_F(CsvReaderTest, RejectsInvalidConstructionArgumentsBeforeOpeningTheFile) {
	const auto path = createTempFile("value\n");
	const CsvReaderLimits valid{.maximum_record_bytes = 16U, .maximum_columns = 2U, .maximum_field_bytes = 8U};

	EXPECT_THROW(CsvReader(path, "", valid), std::invalid_argument);
	EXPECT_THROW(CsvReader(path, "123456789", valid), std::invalid_argument);
	EXPECT_THROW(CsvReader(path, "\"", valid), std::invalid_argument);
	EXPECT_THROW(CsvReader(path, "\n", valid), std::invalid_argument);
	EXPECT_THROW(CsvReader(path, ",", {.maximum_record_bytes = 0U, .maximum_columns = 2U,
											 .maximum_field_bytes = 1U}),
			 std::invalid_argument);
	EXPECT_THROW(CsvReader(path, ",", {.maximum_record_bytes = 2U, .maximum_columns = 0U,
											 .maximum_field_bytes = 1U}),
			 std::invalid_argument);
	EXPECT_THROW(CsvReader(path, ",", {.maximum_record_bytes = 2U, .maximum_columns = 2U,
											 .maximum_field_bytes = 0U}),
			 std::invalid_argument);
	EXPECT_THROW(CsvReader(path, ",", {.maximum_record_bytes = 2U, .maximum_columns = 2U,
											 .maximum_field_bytes = 3U}),
			 std::invalid_argument);
}

TEST_F(CsvReaderTest, HandlesCrLfAndRejectsMalformedQuotedOrOversizedRecords) {
	auto crlf = createTempFile("\"last\"\r\n");
	CsvReader crlfReader(crlf);
	EXPECT_EQ(crlfReader.nextRow(), (std::vector<std::string>{"last"}));

	auto trailingData = createTempFile("\"quoted\"trailing\n");
	CsvReader trailingDataReader(trailingData);
	EXPECT_THROW(static_cast<void>(trailingDataReader.nextRow()), std::runtime_error);

	auto oversizedRecord = createTempFile("12345\n");
	CsvReader recordReader(oversizedRecord, ",", {.maximum_record_bytes = 4U, .maximum_columns = 2U,
												  .maximum_field_bytes = 4U});
	EXPECT_THROW(static_cast<void>(recordReader.nextRow()), std::runtime_error);
}

TEST_F(CsvReaderTest, BoundsPhysicalReadBeforeConsumingAnOversizedLine) {
	auto path = createTempFile(std::string(65536U, 'x') + "\n");
	size_t checkpoints = 0U;
	CsvReader reader(path, ",", {16U, 4U, 16U}, [&] { ++checkpoints; });
	EXPECT_THROW(static_cast<void>(reader.hasNext()), std::runtime_error);
	// The source fails before reaching the next streaming checkpoint.
	EXPECT_EQ(checkpoints, 1U);
}

TEST_F(CsvReaderTest, ExactRecordBoundPreservesCrLfAndMultilineSemantics) {
	for (const auto &contents: {std::string("1234\r\n"), std::string("1234\r"), std::string("1234")}) {
		CsvReader reader(createTempFile(contents), ",", {4U, 2U, 4U});
		ASSERT_TRUE(reader.hasNext());
		EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"1234"}));
		EXPECT_FALSE(reader.hasNext());
	}
	CsvReader multiline(createTempFile("\"a\nb\"\r\n"), ",", {5U, 2U, 5U});
	EXPECT_EQ(multiline.nextRow(), (std::vector<std::string>{"a\nb"}));
	CsvReader oversized(createTempFile("\"a\nbb\"\r\n"), ",", {5U, 2U, 5U});
	EXPECT_THROW(static_cast<void>(oversized.hasNext()), std::runtime_error);
}

TEST_F(CsvReaderTest, CancellationInterruptsLongRecordReadingAndParsing) {
	const auto path = createTempFile(std::string(16384U, 'x') + "\n");
	for (const size_t stopAt: {2U, 11U}) {
		size_t checkpoints = 0U;
		CsvReader reader(path, ",", {32768U, 4U, 32768U}, [&] {
			if (++checkpoints == stopAt) throw std::runtime_error("cancelled");
		});
		EXPECT_THROW(static_cast<void>(reader.hasNext()), std::runtime_error);
		EXPECT_EQ(checkpoints, stopAt);
	}
	CsvReader retry(path, ",", {32768U, 4U, 32768U});
	EXPECT_EQ(retry.nextRow(), (std::vector<std::string>{std::string(16384U, 'x')}));
}

TEST_F(CsvReaderTest, SharesByteAllowanceBetweenRawRecordsAndDecodedFieldSlots) {
	const auto path = createTempFile("abcd\n");
	const size_t exact = 8U + sizeof(std::string);
	CsvReader exactReader(path, ",", {16U, 4U, 16U, exact});
	EXPECT_EQ(exactReader.nextRow(), (std::vector<std::string>{"abcd"}));
	CsvReader shortReader(path, ",", {16U, 4U, 16U, exact - 1U});
	EXPECT_THROW(static_cast<void>(shortReader.hasNext()), std::runtime_error);
	CsvReader slots(createTempFile(",\n"), ",", {16U, 4U, 16U, 2U * sizeof(std::string)});
	EXPECT_THROW(static_cast<void>(slots.hasNext()), std::runtime_error);
	CsvReader raw(createTempFile(std::string(64U, 'x') + "\n"), ",", {128U, 4U, 128U, 64U});
	EXPECT_THROW(static_cast<void>(raw.hasNext()), std::runtime_error);
}

TEST_F(CsvReaderTest, RejectsTransientStringGrowthPeakAndRetriesWithMoreSpace) {
	const std::string value(256U, 'g');
	const auto path = createTempFile(value + "\n");
	const size_t logicalBytes = value.size() * 2U + sizeof(std::string);
	CsvReader constrained(path, ",", {512U, 2U, 512U, logicalBytes});
	try {
		static_cast<void>(constrained.hasNext());
		FAIL() << "old and new string buffers must fit simultaneously during growth";
	} catch (const std::runtime_error &error) {
		EXPECT_NE(std::string(error.what()).find("growth peak"), std::string::npos);
	}
	CsvReader sufficient(path, ",", {512U, 2U, 512U, logicalBytes * 4U});
	EXPECT_EQ(sufficient.nextRow(), (std::vector<std::string>{value}));
}

TEST_F(CsvReaderTest, RejectsTransientFieldVectorGrowthPeakAndRetriesWithMoreSpace) {
	const auto path = createTempFile(",,\n");
	const size_t logicalBytes = 2U + 3U * sizeof(std::string);
	CsvReader constrained(path, ",", {16U, 4U, 16U, logicalBytes});
	try {
		static_cast<void>(constrained.hasNext());
		FAIL() << "old and new field-vector buffers must fit simultaneously during growth";
	} catch (const std::runtime_error &error) {
		EXPECT_NE(std::string(error.what()).find("field-vector growth peak"), std::string::npos);
	}
	CsvReader sufficient(path, ",", {16U, 4U, 16U, logicalBytes * 4U});
	EXPECT_EQ(sufficient.nextRow(), (std::vector<std::string>{"", "", ""}));
}

TEST_F(CsvReaderTest, BufferedAllowanceChangesOnlyAfterConsumingThePeekedRow) {
	CsvReader reader(createTempFile("a\nbbbb\n"));
	ASSERT_TRUE(reader.hasNext());
	EXPECT_THROW(reader.setMaximumBufferedBytes(64U), std::runtime_error);
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"a"}));
	reader.setMaximumBufferedBytes(8U + sizeof(std::string));
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"bbbb"}));
	EXPECT_THROW(reader.setMaximumBufferedBytes(0U), std::runtime_error);
}

TEST_F(CsvReaderTest, BoundedMultilineRecordsPreserveEscapesAndReset) {
	const std::string value = std::string(2048U, 'a') + "\nquoted \"value\"\n" + std::string(2048U, 'b');
	const std::string raw = "\"" + std::string(2048U, 'a') + "\nquoted \"\"value\"\"\n" + std::string(2048U, 'b') + "\"";
	const auto path = createTempFile(raw + "\r\nnext\n");
	const size_t logicalAllowance = raw.size() + value.size() + sizeof(std::string);
	const size_t allowance = logicalAllowance * 8U;
	CsvReader reader(path, ",", {8192U, 2U, 8192U, allowance});
	for (size_t pass = 0U; pass < 2U; ++pass) {
		ASSERT_TRUE(reader.hasNext());
		EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{value}));
		EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"next"}));
		EXPECT_FALSE(reader.hasNext());
		reader.reset();
	}
	CsvReader shortReader(path, ",", {8192U, 2U, 8192U, logicalAllowance});
	EXPECT_THROW(static_cast<void>(shortReader.hasNext()), std::runtime_error);
}

TEST_F(CsvReaderTest, EscapedQuotesShareOneRawRecordBufferWithinTheDecodedAllowance) {
	const std::string decoded(1000U, '"');
	const std::string raw = "\"" + std::string(2000U, '"') + "\"";
	const size_t logicalAllowance = raw.size() + decoded.size() + sizeof(std::string);
	const size_t allowance = logicalAllowance * 4U;
	for (const std::string &ending: {std::string("\n"), std::string("\r\n"), std::string()}) {
		SCOPED_TRACE(ending);
		const auto path = createTempFile(raw + ending);
		CsvReader reader(path, ",", {4096U, 2U, 4096U, allowance});
		ASSERT_TRUE(reader.hasNext());
		EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{decoded}));
		EXPECT_FALSE(reader.hasNext());
		reader.reset();
		EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{decoded}));
		CsvReader insufficient(path, ",", {4096U, 2U, 4096U, logicalAllowance});
		EXPECT_THROW(static_cast<void>(insufficient.hasNext()), std::runtime_error);
	}
	const std::string multilineRaw = "\"" + std::string(2000U, '"') + "\n\"";
	const std::string multilineValue = decoded + "\n";
	CsvReader multiline(createTempFile(multilineRaw + "\r\nnext\n"), ",",
			{4096U, 2U, 4096U, (multilineRaw.size() + multilineValue.size() + sizeof(std::string)) * 4U});
	EXPECT_EQ(multiline.nextRow(), (std::vector<std::string>{multilineValue}));
	EXPECT_EQ(multiline.nextRow(), (std::vector<std::string>{"next"}));
}

TEST_F(CsvReaderTest, EndProbeDoesNotConsumeInputOrDiscardThePeekedLastRow) {
	CsvReader empty(createTempFile(""));
	EXPECT_TRUE(empty.atEnd());
	CsvReader reader(createTempFile("last"));
	EXPECT_FALSE(reader.atEnd());
	EXPECT_FALSE(reader.atEnd());
	ASSERT_TRUE(reader.hasNext());
	EXPECT_FALSE(reader.atEnd());
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"last"}));
	EXPECT_TRUE(reader.atEnd());
	reader.reset();
	EXPECT_FALSE(reader.atEnd());
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"last"}));
	EXPECT_TRUE(reader.atEnd());
}

TEST_F(CsvReaderTest, CancellationOnPrefetchedRowPreservesCachedDataForRetry) {
	bool cancelled = false;
	CsvReader reader(createTempFile("first,value\nsecond,row\n"), ",", {}, [&] {
		if (cancelled) throw std::runtime_error("cancelled cached row");
	});
	ASSERT_TRUE(reader.hasNext());
	cancelled = true;
	EXPECT_THROW(static_cast<void>(reader.hasNext()), std::runtime_error);
	EXPECT_THROW(static_cast<void>(reader.nextRow()), std::runtime_error);
	cancelled = false;
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"first", "value"}));
	EXPECT_EQ(reader.nextRow(), (std::vector<std::string>{"second", "row"}));
	EXPECT_TRUE(reader.atEnd());
}

TEST_F(CsvReaderTest, WideHeapFieldsPreserveCapacityBoundsAcrossVectorRelocation) {
	std::string line;
	std::vector<std::string> expected;
	for (size_t column = 0U; column < 512U; ++column) {
		std::string value(80U, static_cast<char>('a' + column % 26U));
		if (column != 0U) line += ',';
		line += value; expected.push_back(std::move(value));
	}
	line += '\n';
	const auto path = createTempFile(line);
	CsvReader reader(path, ",", {65536U, 512U, 128U, 262144U});
	EXPECT_EQ(reader.nextRow(), expected);
	EXPECT_TRUE(reader.atEnd());
	CsvReader limited(path, ",", {65536U, 512U, 128U, 65536U});
	EXPECT_THROW(static_cast<void>(limited.nextRow()), std::runtime_error);
}

TEST_F(CsvReaderTest, LateWideRecordCancellationDoesNotPublishAndResetCanRetry) {
	std::string line;
	for (size_t column = 0U; column < 1024U; ++column) {
		if (column != 0U) line += ',';
		line += "value";
	}
	line += '\n';
	const auto path = createTempFile(line);
	size_t totalChecks = 0U;
	CsvReader baseline(path, ",", {}, [&] { ++totalChecks; });
	ASSERT_EQ(baseline.nextRow().size(), 1024U);
	ASSERT_GT(totalChecks, 4U);
	size_t checks = 0U;
	bool cancel = true;
	CsvReader reader(path, ",", {}, [&] {
		if (++checks == totalChecks && cancel) throw std::runtime_error("late capacity cancellation");
	});
	EXPECT_THROW(static_cast<void>(reader.hasNext()), std::runtime_error);
	EXPECT_EQ(checks, totalChecks);
	cancel = false; reader.reset();
	ASSERT_TRUE(reader.hasNext());
	const auto result = reader.nextRow();
	ASSERT_EQ(result.size(), 1024U);
	for (const auto &field : result) EXPECT_EQ(field, "value");
	EXPECT_TRUE(reader.atEnd());
}

TEST_F(CsvReaderTest, SmallExternalStringBuffersCannotEscapeTheByteAllowance) {
	const std::string value(23U, 'x');
	const auto path = createTempFile(value + "\n");
	CsvReader limited(path, ",", {64U, 2U, 64U, 80U});
	EXPECT_THROW(static_cast<void>(limited.hasNext()), std::runtime_error);
	CsvReader sufficient(path, ",", {64U, 2U, 64U, 256U});
	EXPECT_EQ(sufficient.nextRow(), (std::vector<std::string>{value}));
	EXPECT_TRUE(sufficient.atEnd());
}

TEST_F(CsvReaderTest, SharedStringAccountingIncludesSmallExternalAndRetainedBuffers) {
	const std::string external(23U, 'x');
	EXPECT_EQ(detail::dynamicStringBufferBytes(external), external.capacity() + 1U);
	std::string retained(128U, 'x');
	retained.resize(1U);
	EXPECT_EQ(detail::dynamicStringBufferBytes(retained), retained.capacity() + 1U);
	EXPECT_GT(detail::dynamicStringBufferBytes(retained), retained.size());
}

TEST_F(CsvReaderTest, RetainedPropertyAccountingPreservesNestedSpareCapacity) {
	std::vector<graph::PropertyValue> list; list.reserve(128U);
	std::string text(128U, 'x'); text.resize(1U);
	list.emplace_back(std::move(text));
	graph::PropertyValue value(std::move(list));
	EXPECT_GE(detail::retainedPropertyValueBytes(value),
		value.getList().capacity() * sizeof(graph::PropertyValue) + 129U);
	graph::PropertyValue::Bytes bytes; bytes.reserve(256U); bytes.push_back(std::byte{1});
	graph::PropertyValue binary(std::move(bytes));
	EXPECT_GE(detail::retainedPropertyValueBytes(binary), 256U);
}

TEST_F(CsvReaderTest, RetainedPropertyAccountingObservesCopiedContainerCapacity) {
	std::vector<graph::PropertyValue> elements;
	elements.reserve(512U);
	elements.emplace_back(int64_t{7});
	const graph::PropertyValue original(std::move(elements));
	const graph::PropertyValue copied(original);
	EXPECT_EQ(original, copied);
	EXPECT_GE(detail::retainedPropertyValueBytes(original),
		original.getList().capacity() * sizeof(graph::PropertyValue));
	EXPECT_EQ(detail::retainedPropertyValueBytes(copied), std::max(
		graph::property_utils::getPropertyValueSize(copied),
		copied.getList().capacity() * sizeof(graph::PropertyValue)));
}

TEST_F(CsvReaderTest, RetainedPropertyMapAccountsForEmptyReservedBucketsAndNestedSlots) {
	graph::PropertyValue::MapType map;
	map.reserve(1024U);
	EXPECT_GE(detail::propertyMapHeapBufferBytes(map), map.bucket_count() * sizeof(void *));
	std::vector<graph::PropertyValue> values;
	values.reserve(256U);
	values.emplace_back(int64_t{7});
	map.emplace("payload", graph::PropertyValue(std::move(values)));
	EXPECT_GE(detail::propertyMapHeapBufferBytes(map), map.bucket_count() * sizeof(void *) +
		map.at("payload").getList().capacity() * sizeof(graph::PropertyValue));
	graph::PropertyValue wrapped(std::move(map));
	EXPECT_EQ(detail::propertyHeapBufferBytes(wrapped),
		detail::propertyMapHeapBufferBytes(std::get<graph::PropertyValue::MapType>(wrapped.getVariant())));
}

TEST_F(CsvReaderTest, GuardedNestedPropertyAccountingCancelsAndRetriesWithoutMutation) {
	graph::PropertyValue::MapType map;
	map.reserve(32U);
	map.emplace("nested", graph::PropertyValue(std::vector<graph::PropertyValue>(4096U, graph::PropertyValue(int64_t{7}))));
	const graph::PropertyValue value(std::move(map));
	const size_t expected = detail::retainedPropertyValueBytes(value);
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::retainedPropertyValueBytes(value, [&] {
		if (++checkpoints == 3U) throw std::runtime_error("accounting cancelled");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 3U);
	EXPECT_EQ(value.getMap().at("nested").getList().size(), 4096U);
	checkpoints = 0U;
	EXPECT_EQ(detail::retainedPropertyValueBytes(value, [&] { ++checkpoints; }), expected);
	EXPECT_GE(checkpoints, 18U);
	const auto estimates = detail::propertyBufferEstimates(value, [] {});
	EXPECT_EQ(estimates.logicalBytes, graph::property_utils::getPropertyValueSize(value));
	EXPECT_EQ(estimates.heapBytes, detail::propertyHeapBufferBytes(value));
}

TEST_F(CsvReaderTest, GuardedRetainedMapAccountingPreservesBothLowerBoundsAndRetries) {
	graph::PropertyValue::MapType map;
	map.reserve(32U);
	map.emplace("nested", graph::PropertyValue(std::vector<graph::PropertyValue>(4096U, graph::PropertyValue(int64_t{7}))));
	const size_t logical = sizeof(map) + std::string("nested").size() + sizeof(graph::PropertyValue) +
		detail::retainedPropertyValueBytes(map.at("nested"));
	const size_t expected = std::max(logical, sizeof(map) + detail::propertyMapHeapBufferBytes(map));
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(detail::retainedPropertyMapBytes(map, [&] {
		if (++checkpoints == 3U) throw std::runtime_error("map accounting cancelled");
	})), std::runtime_error);
	EXPECT_EQ(checkpoints, 3U);
	EXPECT_EQ(map.at("nested").getList().size(), 4096U);
	checkpoints = 0U;
	EXPECT_EQ(detail::retainedPropertyMapBytes(map, [&] { ++checkpoints; }), expected);
	EXPECT_GE(checkpoints, 18U);
}
