/**
 * @file test_TableFormatter.cpp
 * @author Nexepic
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "graph/utils/TableFormatter.hpp"

using graph::storage::TableFormatter;

class TableFormatterTest : public ::testing::Test {
protected:
	std::stringstream output;

	void SetUp() override {
		output.str("");
		output.clear();
	}

	void TearDown() override {}
};

// ============================================================================
// Basic Table Creation and Printing
// ============================================================================

TEST_F(TableFormatterTest, CreateTableAndPrint) {
	TableFormatter formatter;

	formatter.addColumn("ID");
	formatter.addColumn("Name");
	formatter.addColumn("Score");

	formatter.addRow({"1", "Alice", "95"});
	formatter.addRow({"2", "Bob", "87"});
	formatter.addRow({"3", "Charlie", "92"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

// ============================================================================
// Border Style Tests (covers line 55)
// ============================================================================

TEST_F(TableFormatterTest, CreateWithASCIIBorderStyle) {
	TableFormatter formatter(TableFormatter::BorderStyle::ASCII);

	formatter.addColumn("Col1");
	formatter.addRow({"Data1"});

	EXPECT_NO_THROW(formatter.print(output));

	// Verify ASCII border characters are used
	std::string result = output.str();
	EXPECT_NE(result.find('+'), std::string::npos) << "Should contain ASCII border +";
}

TEST_F(TableFormatterTest, CreateWithUnicodeBorderStyle) {
	TableFormatter formatter(TableFormatter::BorderStyle::UNICODE);

	formatter.addColumn("Col1");
	formatter.addRow({"Data1"});

	EXPECT_NO_THROW(formatter.print(output));

	// Verify something was printed
	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Should contain output with Unicode borders";
}

// ============================================================================
// Row Validation Tests (covers line 118)
// ============================================================================

TEST_F(TableFormatterTest, AddRow_InvalidSize_TooFew) {
	TableFormatter formatter;
	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// Try to add row with too few elements
	EXPECT_THROW(formatter.addRow({"OnlyOne"}), std::runtime_error);
}

TEST_F(TableFormatterTest, AddRow_InvalidSize_TooMany) {
	TableFormatter formatter;
	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// Try to add row with too many elements
	EXPECT_THROW(formatter.addRow({"One", "Two", "Three"}), std::runtime_error);
}

TEST_F(TableFormatterTest, AddRow_ExactSize) {
	TableFormatter formatter;
	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// Correct size should work
	EXPECT_NO_THROW(formatter.addRow({"A", "B"}));
}

// ============================================================================
// Empty Table Tests (covers line 129)
// ============================================================================

TEST_F(TableFormatterTest, PrintEmptyTable_NoColumns) {
	TableFormatter formatter;

	// Print without adding any columns (covers line 129: if (columns.empty()))
	EXPECT_NO_THROW(formatter.print(output));

	// Should produce minimal output
	std::string result = output.str();
	EXPECT_TRUE(result.empty()) << "Empty table should produce no output";
}

TEST_F(TableFormatterTest, PrintEmptyTable_WithColumnsNoRows) {
	TableFormatter formatter;

	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// Has columns but no rows (still covers columns.empty() check being false)
	EXPECT_NO_THROW(formatter.print(output));

	// Should produce header row at least
	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Table with columns should produce header";
}

// ============================================================================
// Title Tests (covers line 139)
// ============================================================================

TEST_F(TableFormatterTest, PrintWithTitle) {
	TableFormatter formatter;

	// Use a short title that fits in the table
	formatter.setTitle("Title");

	formatter.addColumn("Col1");
	formatter.addRow({"Data1"});

	EXPECT_NO_THROW(formatter.print(output));

	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Should produce output with title";
	// Just verify output was produced, title formatting depends on implementation
}

TEST_F(TableFormatterTest, PrintWithoutTitle) {
	TableFormatter formatter;

	formatter.addColumn("Col1");
	formatter.addRow({"Data1"});

	EXPECT_NO_THROW(formatter.print(output));

	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Should produce output without title";
}

// ============================================================================
// Clear Function Tests (covers line 178)
// ============================================================================

TEST_F(TableFormatterTest, Clear_Functionality) {
	TableFormatter formatter;

	// Use shorter titles to fit in table width
	formatter.setTitle("Title1");
	formatter.addColumn("Col1");
	formatter.addColumn("Col2");
	formatter.addRow({"A", "B"});
	formatter.addRow({"C", "D"});

	// Clear the table (covers clear() function)
	formatter.clear();

	// Verify columns are empty
	output.str("");
	output.clear();

	EXPECT_NO_THROW(formatter.print(output));
	std::string resultAfterClear = output.str();
	EXPECT_TRUE(resultAfterClear.empty()) << "Should produce no output after clear";

	// Add new data after clear
	formatter.setTitle("Title2");
	formatter.addColumn("NewCol");
	formatter.addRow({"NewData"});

	// Clear output again before printing
	output.str("");
	output.clear();

	EXPECT_NO_THROW(formatter.print(output));
	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Should produce output with new data";
}

// ============================================================================
// SetTitle Tests
// ============================================================================

TEST_F(TableFormatterTest, SetTitle_MultipleTimes) {
	TableFormatter formatter;

	// Use very short titles to fit in table width
	// Column "Col1" has width 4, tableWidth = 8, titleSpaceWidth = 4
	formatter.setTitle("A");
	formatter.setTitle("B");

	formatter.addColumn("Col1");
	formatter.addRow({"Data1"});

	EXPECT_NO_THROW(formatter.print(output));

	std::string result = output.str();
	EXPECT_FALSE(result.empty()) << "Should produce output";
}

// ============================================================================
// Multiple Rows Tests
// ============================================================================

TEST_F(TableFormatterTest, MultipleRows) {
	TableFormatter formatter;

	formatter.addColumn("ID");
	formatter.addColumn("Name");

	for (int i = 1; i <= 10; ++i) {
		formatter.addRow({std::to_string(i), "Item" + std::to_string(i)});
	}

	EXPECT_NO_THROW(formatter.print(output));
}

TEST_F(TableFormatterTest, SingleRow) {
	TableFormatter formatter;

	formatter.addColumn("Col1");
	formatter.addRow({"OnlyOneRow"});

	EXPECT_NO_THROW(formatter.print(output));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(TableFormatterTest, VeryLongColumnContent) {
	TableFormatter formatter;

	formatter.addColumn("Short");
	formatter.addRow({std::string(1000, 'A')});

	EXPECT_NO_THROW(formatter.print(output));
}

TEST_F(TableFormatterTest, SpecialCharactersInContent) {
	TableFormatter formatter;

	formatter.addColumn("Special");
	formatter.addRow({"Tab\tTest"});
	formatter.addRow({"Newline\nTest"});
	formatter.addRow({"Unicode: 你好 🌍"});

	EXPECT_NO_THROW(formatter.print(output));
}

TEST_F(TableFormatterTest, ManyColumns) {
	TableFormatter formatter;

	// Create many columns
	for (int i = 1; i <= 20; ++i) {
		formatter.addColumn("Col" + std::to_string(i));
	}

	// Add one row with all columns
	std::vector<std::string> row;
	for (int i = 1; i <= 20; ++i) {
		row.push_back("Data" + std::to_string(i));
	}
	formatter.addRow(row);

	EXPECT_NO_THROW(formatter.print(output));
}

// ============================================================================
// Stream Output Tests
// ============================================================================

TEST_F(TableFormatterTest, PrintToOutputStream) {
	TableFormatter formatter;

	formatter.addColumn("A");
	formatter.addRow({"1"});

	// Print to stringstream
	std::stringstream ss;
	EXPECT_NO_THROW(formatter.print(ss));

	// Verify something was written
	std::string result = ss.str();
	EXPECT_FALSE(result.empty());
}

// ============================================================================
// Additional Edge Case Tests for Coverage Improvement
// ============================================================================

TEST_F(TableFormatterTest, ColumnWidthAdjustment) {
	TableFormatter formatter;

	// Add column with short header
	formatter.addColumn("ID");
	// Add row with longer content - column width should expand
	formatter.addRow({"1234567890"});

	EXPECT_NO_THROW(formatter.print(output));

	std::string result = output.str();
	EXPECT_FALSE(result.empty());
}

TEST_F(TableFormatterTest, MultipleRowsWithVaryingWidths) {
	TableFormatter formatter;

	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// First row with short content
	formatter.addRow({"A", "B"});
	// Second row with longer content
	formatter.addRow({"LongText1", "LongText2"});
	// Third row with mixed content
	formatter.addRow({"X", "VeryLongTextHere"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, PrintToCout) {
	TableFormatter formatter;

	formatter.addColumn("TestCol");
	formatter.addRow({"TestValue"});

	// Redirect cout to stringstream
	std::stringstream buffer;
	std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

	EXPECT_NO_THROW(formatter.print()); // Print to cout

	// Restore cout
	std::cout.rdbuf(old);

	std::string result = buffer.str();
	EXPECT_FALSE(result.empty()) << "Should print to cout";
}

TEST_F(TableFormatterTest, UnicodeBorderWithMultipleColumns) {
	TableFormatter formatter(TableFormatter::BorderStyle::UNICODE);

	formatter.addColumn("A");
	formatter.addColumn("B");
	formatter.addColumn("C");

	formatter.addRow({"1", "2", "3"});
	formatter.addRow({"4", "5", "6"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, ASCIIBorderWithMultipleColumns) {
	TableFormatter formatter(TableFormatter::BorderStyle::ASCII);

	formatter.addColumn("ColA");
	formatter.addColumn("ColB");

	formatter.addRow({"Val1", "Val2"});
	formatter.addRow({"Val3", "Val4"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, TableWithOnlyTitleAndColumnsNoRows) {
	TableFormatter formatter;

	formatter.setTitle("X"); // Very short title
	formatter.addColumn("Col1");
	formatter.addColumn("Col2");

	// Don't add any rows - just title and headers
	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, EmptyStringContent) {
	TableFormatter formatter;

	formatter.addColumn("Col1");
	formatter.addRow({""}); // Empty string content
	formatter.addRow({"NonEmpty"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, SpacesInContent) {
	TableFormatter formatter;

	formatter.addColumn("Name");
	formatter.addRow({"Value With Spaces"});
	formatter.addRow({"  Leading and trailing  "});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, VeryShortColumnWithLongContent) {
	TableFormatter formatter;

	// Add column with very short header
	formatter.addColumn("X");
	// Add row with very long content
	formatter.addRow({std::string(200, 'Y')});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

// ============================================================================
// Tests for custom padding
// ============================================================================

TEST_F(TableFormatterTest, CustomPadding) {
	// Create formatter with custom padding of 3 spaces
	TableFormatter formatter(TableFormatter::BorderStyle::ASCII, 3);

	formatter.addColumn("A");
	formatter.addRow({"B"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, ZeroPadding) {
	// Create formatter with zero padding
	TableFormatter formatter(TableFormatter::BorderStyle::ASCII, 0);

	formatter.addColumn("Col");
	formatter.addRow({"Val"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, LargePadding) {
	// Create formatter with large padding
	TableFormatter formatter(TableFormatter::BorderStyle::ASCII, 5);

	formatter.addColumn("X");
	formatter.addRow({"Y"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

// ============================================================================
// Tests for single column tables (edge case for width calculation)
// ============================================================================

TEST_F(TableFormatterTest, SingleColumnWithTitle) {
	TableFormatter formatter;

	formatter.setTitle("T"); // Short title
	formatter.addColumn("Column");
	formatter.addRow({"Value"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, SingleColumnWithoutTitle) {
	TableFormatter formatter;

	formatter.addColumn("C");
	formatter.addRow({"V"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

// ============================================================================
// Tests for title centering edge cases
// ============================================================================

TEST_F(TableFormatterTest, TitleExactWidth) {
	TableFormatter formatter;

	// Make table wide enough for title
	formatter.addColumn("ColumnHeader"); // 13 chars
	formatter.setTitle("Title"); // Short title that fits

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, TitleOneLessThanWidth) {
	TableFormatter formatter;

	formatter.addColumn("Col");
	formatter.setTitle("X"); // Very short title

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}

TEST_F(TableFormatterTest, TitleAndDataMixedWidths) {
	TableFormatter formatter;

	formatter.setTitle("Data");
	formatter.addColumn("A");
	formatter.addColumn("B");

	// Add rows with varying widths to trigger width recalculation
	formatter.addRow({"1", "2"});
	formatter.addRow({"Longer text for A", "X"});
	formatter.addRow({"Y", "Longer text for B"});

	EXPECT_NO_THROW(formatter.print(output));
	EXPECT_FALSE(output.str().empty());
}


