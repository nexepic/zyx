/**
 * @file test_OptionalMatchOperator_Unit.cpp
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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
#include <memory>
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

/**
 * Test fixture for OptionalMatchOperator
 */
class OptionalMatchOperatorTest : public ::testing::Test {
protected:
	// Mock input operator that returns test records
	class MockInputOperator : public PhysicalOperator {
	public:
		MockInputOperator(std::vector<Record> records) : records_(std::move(records)), index_(0) {}

		void open() override { index_ = 0; }
		std::optional<RecordBatch> next() override {
			if (index_ >= records_.size()) return std::nullopt;
			RecordBatch batch;
			batch.push_back(records_[index_++]);
			return batch;
		}
		void close() override {}
		std::vector<std::string> getOutputVariables() const override { return {"input"}; }
		std::string toString() const override { return "MockInputOperator"; }

	private:
		std::vector<Record> records_;
		size_t index_;
	};

	// Mock pattern operator that returns matches (or not)
	class MockPatternOperator : public PhysicalOperator {
	public:
		MockPatternOperator(std::vector<Record> matches, bool returnMatches)
			: matches_(std::move(matches)), returnMatches_(returnMatches), index_(0) {}

		void open() override { index_ = 0; }
		std::optional<RecordBatch> next() override {
			if (!returnMatches_ || index_ >= matches_.size()) return std::nullopt;
			RecordBatch batch;
			batch.push_back(matches_[index_++]);
			return batch;
		}
		void close() override {}
		std::vector<std::string> getOutputVariables() const override { return {"pattern"}; }
		std::string toString() const override { return "MockPatternOperator"; }

	private:
		std::vector<Record> matches_;
		bool returnMatches_;
		size_t index_;
	};
};

/**
 * Test OptionalMatchOperator with matching pattern
 */
TEST_F(OptionalMatchOperatorTest, ReturnsMergedRecordWhenPatternMatches) {
	// Create input record
	Record inputRecord;
	inputRecord.setValue("input", PropertyValue(42));

	// Create matching pattern record
	Record patternRecord;
	patternRecord.setValue("pattern", PropertyValue("matched"));

	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{inputRecord});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{patternRecord}, true);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();
	auto resultOpt = op.next();
	ASSERT_TRUE(resultOpt.has_value());
	EXPECT_EQ(resultOpt->size(), 1UL);

	const Record &result = (*resultOpt)[0];
	auto inputValue = result.getValue("input");
	auto patternValue = result.getValue("pattern");
	ASSERT_TRUE(inputValue.has_value());
	ASSERT_TRUE(patternValue.has_value());
	// Use getVariant() to access the underlying std::variant
	EXPECT_EQ(std::get<int64_t>(inputValue->getVariant()), 42);
	EXPECT_EQ(std::get<std::string>(patternValue->getVariant()), "matched");

	op.close();
}

/**
 * Test OptionalMatchOperator with no matches (NULL extension)
 */
TEST_F(OptionalMatchOperatorTest, ReturnsNullExtendedRecordWhenNoMatch) {
	// Create input record
	Record inputRecord;
	inputRecord.setValue("input", PropertyValue(42));

	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{inputRecord});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();
	auto resultOpt = op.next();
	ASSERT_TRUE(resultOpt.has_value());
	EXPECT_EQ(resultOpt->size(), 1UL);

	const Record &result = (*resultOpt)[0];
	auto inputValue = result.getValue("input");
	ASSERT_TRUE(inputValue.has_value());
	EXPECT_EQ(std::get<int64_t>(inputValue->getVariant()), 42);

	// Pattern variable should have NULL value
	auto patternValue = result.getValue("pattern");
	// NULL values are stored as PropertyValue with NULL_TYPE
	EXPECT_TRUE(patternValue.has_value());
	EXPECT_EQ(patternValue->getType(), PropertyType::NULL_TYPE);

	op.close();
}

/**
 * Test OptionalMatchOperator with multiple input records
 */
TEST_F(OptionalMatchOperatorTest, HandlesMultipleInputRecords) {
	// Create multiple input records
	std::vector<Record> inputRecords;
	for (int i = 0; i < 3; ++i) {
		Record r;
		r.setValue("input", PropertyValue(i));
		inputRecords.push_back(r);
	}

	auto inputOp = std::make_unique<MockInputOperator>(inputRecords);
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();

	// Should get 3 NULL-extended records
	int totalCount = 0;
	while (true) {
		auto batchOpt = op.next();
		if (!batchOpt || batchOpt->empty()) break;
		totalCount += batchOpt->size();
	}

	EXPECT_EQ(totalCount, 3);
	op.close();
}

/**
 * Test OptionalMatchOperator getOutputVariables
 */
TEST_F(OptionalMatchOperatorTest, GetOutputVariablesCombinesSchemas) {
	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	auto schema = op.getOutputVariables();
	EXPECT_EQ(schema.size(), 2UL);
	EXPECT_EQ(schema[0], "input");
	EXPECT_EQ(schema[1], "pattern");
}

/**
 * Test OptionalMatchOperator toString
 */
TEST_F(OptionalMatchOperatorTest, ToStringReturnsDescriptiveString) {
	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	auto str = op.toString();
	EXPECT_TRUE(str.find("OptionalMatchOperator") != std::string::npos);
	EXPECT_TRUE(str.find("pattern") != std::string::npos);
}

/**
 * Test OptionalMatchOperator validates null operators
 */
TEST_F(OptionalMatchOperatorTest, ThrowsOnNullInputOperator) {
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	EXPECT_THROW(
		OptionalMatchOperator(nullptr, std::move(patternOp), {"pattern"}),
		std::invalid_argument
	);
}

/**
 * Test OptionalMatchOperator validates null pattern operator
 */
TEST_F(OptionalMatchOperatorTest, ThrowsOnNullPatternOperator) {
	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{});

	EXPECT_THROW(
		OptionalMatchOperator(std::move(inputOp), nullptr, {"pattern"}),
		std::invalid_argument
	);
}

/**
 * Test OptionalMatchOperator with pattern that has multiple matches
 */
TEST_F(OptionalMatchOperatorTest, ReturnsAllPatternMatches) {
	// Create input record
	Record inputRecord;
	inputRecord.setValue("input", PropertyValue(42));

	// Create two matching pattern records - return them in one batch
	std::vector<Record> patternRecords;
	Record r1, r2;
	r1.setValue("pattern", PropertyValue("match1"));
	r2.setValue("pattern", PropertyValue("match2"));
	patternRecords.push_back(r1);
	patternRecords.push_back(r2);

	// Custom mock that returns both records in one batch
	class MultiMatchPatternOperator : public PhysicalOperator {
	public:
		MultiMatchPatternOperator(std::vector<Record> matches)
			: matches_(std::move(matches)), returned_(false) {}

		void open() override { returned_ = false; }
		std::optional<RecordBatch> next() override {
			if (returned_) return std::nullopt;
			returned_ = true;
			return matches_;
		}
		void close() override {}
		std::vector<std::string> getOutputVariables() const override { return {"pattern"}; }
		std::string toString() const override { return "MultiMatchPatternOperator"; }

	private:
		RecordBatch matches_;
		bool returned_;
	};

	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{inputRecord});
	auto patternOp = std::make_unique<MultiMatchPatternOperator>(patternRecords);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();
	auto resultOpt = op.next();
	ASSERT_TRUE(resultOpt.has_value());
	// Should have 2 records (one for each pattern match)
	EXPECT_EQ(resultOpt->size(), 2UL);

	op.close();
}
