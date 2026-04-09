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
