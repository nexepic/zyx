/**
 * @file test_OptionalMatchOperator.cpp
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
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
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

/**
 * Test OptionalMatchOperator toString with multiple required variables
 * Covers the comma separator branch (i > 0) in toString()
 */
TEST_F(OptionalMatchOperatorTest, ToStringWithMultipleRequiredVariables) {
	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"a", "b", "c"});

	auto str = op.toString();
	EXPECT_TRUE(str.find("a") != std::string::npos);
	EXPECT_TRUE(str.find("b") != std::string::npos);
	EXPECT_TRUE(str.find("c") != std::string::npos);
	// Verify commas are present (covers i > 0 branch)
	EXPECT_TRUE(str.find(", ") != std::string::npos);
}

/**
 * Test OptionalMatchOperator with empty required variables list
 * Covers the case where requiredVariables_ is empty in toString() and createNullExtendedRecord()
 */
TEST_F(OptionalMatchOperatorTest, ToStringWithEmptyRequiredVariables) {
	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {});

	auto str = op.toString();
	EXPECT_TRUE(str.find("OptionalMatchOperator") != std::string::npos);
	EXPECT_TRUE(str.find("[]") != std::string::npos);
}

/**
 * Test createNullExtendedRecord when required variable already exists as a node
 * Covers the nodeOpt.has_value() true branch in createNullExtendedRecord()
 */
TEST_F(OptionalMatchOperatorTest, NullExtensionSkipsExistingNodeVariable) {
	Record inputRecord;
	inputRecord.setValue("input", PropertyValue(42));
	// Set a node for the required variable - it already exists
	Node node(1, 100);
	inputRecord.setNode("pattern", node);

	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{inputRecord});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();
	auto resultOpt = op.next();
	ASSERT_TRUE(resultOpt.has_value());
	EXPECT_EQ(resultOpt->size(), 1UL);

	// The node should still exist (not overwritten with NULL)
	auto nodeOpt = (*resultOpt)[0].getNode("pattern");
	EXPECT_TRUE(nodeOpt.has_value());
	EXPECT_EQ(nodeOpt->getId(), 1);

	op.close();
}

/**
 * Test createNullExtendedRecord when required variable already exists as an edge
 * Covers the edgeOpt.has_value() true branch in createNullExtendedRecord()
 */
TEST_F(OptionalMatchOperatorTest, NullExtensionSkipsExistingEdgeVariable) {
	Record inputRecord;
	inputRecord.setValue("input", PropertyValue(42));
	// Set an edge for the required variable - it already exists
	Edge edge(10, 1, 2, 200);
	inputRecord.setEdge("rel", edge);

	auto inputOp = std::make_unique<MockInputOperator>(std::vector<Record>{inputRecord});
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"rel"});

	op.open();
	auto resultOpt = op.next();
	ASSERT_TRUE(resultOpt.has_value());
	EXPECT_EQ(resultOpt->size(), 1UL);

	// The edge should still exist (not overwritten with NULL)
	auto edgeOpt = (*resultOpt)[0].getEdge("rel");
	EXPECT_TRUE(edgeOpt.has_value());
	EXPECT_EQ(edgeOpt->getId(), 10);

	op.close();
}

/**
 * Test OptionalMatchOperator with input returning empty batch
 * Covers the batchOpt->empty() true branch in next()
 */
TEST_F(OptionalMatchOperatorTest, HandlesEmptyBatchFromInput) {
	// Custom mock that returns an empty batch
	class EmptyBatchOperator : public PhysicalOperator {
	public:
		EmptyBatchOperator() : returned_(false) {}
		void open() override { returned_ = false; }
		std::optional<RecordBatch> next() override {
			if (returned_) return std::nullopt;
			returned_ = true;
			return RecordBatch{}; // empty batch
		}
		void close() override {}
		std::vector<std::string> getOutputVariables() const override { return {"input"}; }
		std::string toString() const override { return "EmptyBatchOperator"; }
	private:
		bool returned_;
	};

	auto inputOp = std::make_unique<EmptyBatchOperator>();
	auto patternOp = std::make_unique<MockPatternOperator>(std::vector<Record>{}, false);

	OptionalMatchOperator op(std::move(inputOp), std::move(patternOp), {"pattern"});

	op.open();
	auto resultOpt = op.next();
	// Empty batch should cause inputExhausted_ = true and return nullopt
	EXPECT_FALSE(resultOpt.has_value());

	op.close();
}
