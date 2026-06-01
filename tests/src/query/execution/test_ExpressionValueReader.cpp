#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/TemporalTypes.hpp"
#include "graph/query/execution/ExpressionValueReader.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"

using graph::Node;
using graph::Edge;
using graph::PropertyType;
using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::query::execution::ExpressionValueReader;
using graph::query::execution::ExpressionValueReaderKind;
using graph::query::execution::Record;
using graph::query::expressions::BinaryOpExpression;
using graph::query::expressions::BinaryOperatorType;
using graph::query::expressions::ExpressionEvaluationException;
using graph::query::expressions::LiteralExpression;
using graph::query::expressions::ParameterExpression;
using graph::query::expressions::UndefinedVariableException;
using graph::query::expressions::VariableReferenceExpression;

TEST(ExpressionValueReaderTest, CompilesLiteralAndParameterReaders) {
	auto literal = ExpressionValueReader::compile(std::make_shared<LiteralExpression>(int64_t{42}));
	EXPECT_EQ(literal.kind(), ExpressionValueReaderKind::EVR_LITERAL);
	EXPECT_EQ(std::get<int64_t>(literal.evaluate(Record{}).getVariant()), 42);

	auto parameter = ExpressionValueReader::compile(std::make_shared<ParameterExpression>("limit"));
	EXPECT_EQ(parameter.kind(), ExpressionValueReaderKind::EVR_PARAMETER);
	std::unordered_map<std::string, PropertyValue> params{{"limit", PropertyValue(int64_t{7})}};
	EXPECT_EQ(std::get<int64_t>(parameter.evaluate(Record{}, nullptr, &params).getVariant()), 7);
	EXPECT_THROW((void) parameter.evaluate(Record{}), ExpressionEvaluationException);
}

TEST(ExpressionValueReaderTest, ReadsAllLiteralKindsAndNullExpression) {
	auto nullExpression = ExpressionValueReader::compile(nullptr);
	EXPECT_EQ(nullExpression.kind(), ExpressionValueReaderKind::EVR_NULL);
	EXPECT_EQ(nullExpression.evaluate(Record{}).getType(), PropertyType::NULL_TYPE);

	EXPECT_EQ(ExpressionValueReader::compile(std::make_shared<LiteralExpression>()).evaluate(Record{}).getType(),
	          PropertyType::NULL_TYPE);
	EXPECT_EQ(std::get<bool>(
	              ExpressionValueReader::compile(std::make_shared<LiteralExpression>(true)).evaluate(Record{}).getVariant()),
	          true);
	EXPECT_DOUBLE_EQ(
			std::get<double>(
					ExpressionValueReader::compile(std::make_shared<LiteralExpression>(2.5)).evaluate(Record{}).getVariant()),
			2.5);
	EXPECT_EQ(
			std::get<std::string>(
					ExpressionValueReader::compile(std::make_shared<LiteralExpression>(std::string("neo"))).evaluate(Record{}).getVariant()),
			"neo");
}

TEST(ExpressionValueReaderTest, ThrowsForMissingNamedParameter) {
	auto parameter = ExpressionValueReader::compile(std::make_shared<ParameterExpression>("missing"));
	std::unordered_map<std::string, PropertyValue> params{{"other", PropertyValue(int64_t{1})}};
	EXPECT_THROW((void) parameter.evaluate(Record{}, nullptr, &params), ExpressionEvaluationException);
}

TEST(ExpressionValueReaderTest, ReadsVariablesAndPropertiesWithoutGenericEvaluator) {
	Record record;
	record.setValue("country", PropertyValue("US"));
	Node user(11, 1);
	user.addProperty("age", PropertyValue(int64_t{30}));
	record.setNode("u", user);

	auto variable = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("country"));
	EXPECT_EQ(variable.kind(), ExpressionValueReaderKind::EVR_VARIABLE);
	EXPECT_EQ(std::get<std::string>(variable.evaluate(record).getVariant()), "US");

	auto nodeId = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("u"));
	EXPECT_EQ(std::get<int64_t>(nodeId.evaluate(record).getVariant()), 11);

	auto property = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("u", "age"));
	EXPECT_EQ(property.kind(), ExpressionValueReaderKind::EVR_PROPERTY);
	EXPECT_EQ(std::get<int64_t>(property.evaluate(record).getVariant()), 30);
}

TEST(ExpressionValueReaderTest, PreservesMissingVariableAndMapPropertySemantics) {
	Record record;
	PropertyValue::MapType map{{"name", PropertyValue("Ada")}};
	record.setValue("row", PropertyValue(map));

	auto mapProperty = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("row", "name"));
	EXPECT_EQ(std::get<std::string>(mapProperty.evaluate(record).getVariant()), "Ada");

	auto missingProperty = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("row", "missing"));
	EXPECT_EQ(missingProperty.evaluate(record).getType(), PropertyType::NULL_TYPE);

	auto missingVariable = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("missing"));
	EXPECT_THROW((void) missingVariable.evaluate(record), UndefinedVariableException);
}

TEST(ExpressionValueReaderTest, ReadsEdgePropertiesAndMissingPropertyVariableAsNull) {
	Record record;
	Edge relationship(9, 1, 2, 3);
	relationship.addProperty("weight", PropertyValue(1.5));
	record.setEdge("r", relationship);

	auto edgeProperty = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("r", "weight"));
	EXPECT_DOUBLE_EQ(std::get<double>(edgeProperty.evaluate(record).getVariant()), 1.5);

	auto missingEdgeProperty = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("r", "missing"));
	EXPECT_EQ(missingEdgeProperty.evaluate(record).getType(), PropertyType::NULL_TYPE);

	auto missingVariableProperty =
			ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("missing", "property"));
	EXPECT_EQ(missingVariableProperty.evaluate(record).getType(), PropertyType::NULL_TYPE);
}

TEST(ExpressionValueReaderTest, ReadsTemporalComponents) {
	Record record;
	record.setValue("date", PropertyValue(TemporalDate::fromYMD(2024, 5, 6)));
	record.setValue("datetime", PropertyValue(TemporalDateTime::fromComponents(2024, 5, 6, 7, 8, 9, 10)));
	record.setValue("duration", PropertyValue(TemporalDuration{2, 3, 4'000'000'000LL}));

	auto dateYear = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("date", "year"));
	auto dateMonth = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("date", "month"));
	auto dateDay = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("date", "day"));
	auto dateEpoch = ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("date", "epochDays"));
	EXPECT_EQ(std::get<int64_t>(dateYear.evaluate(record).getVariant()), 2024);
	EXPECT_EQ(std::get<int64_t>(dateMonth.evaluate(record).getVariant()), 5);
	EXPECT_EQ(std::get<int64_t>(dateDay.evaluate(record).getVariant()), 6);
	EXPECT_EQ(dateEpoch.evaluate(record).getType(), PropertyType::INTEGER);

	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("datetime", "hour"))
	                          .evaluate(record)
	                          .getVariant()),
	          7);
	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("datetime", "minute"))
	                          .evaluate(record)
	                          .getVariant()),
	          8);
	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("datetime", "second"))
	                          .evaluate(record)
	                          .getVariant()),
	          9);
	EXPECT_EQ(ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("datetime", "epochMillis"))
	                  .evaluate(record)
	                  .getType(),
	          PropertyType::INTEGER);

	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("duration", "months"))
	                          .evaluate(record)
	                          .getVariant()),
	          2);
	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("duration", "days"))
	                          .evaluate(record)
	                          .getVariant()),
	          3);
	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(std::make_shared<VariableReferenceExpression>("duration", "seconds"))
	                          .evaluate(record)
	                          .getVariant()),
	          4);
	EXPECT_EQ(std::get<int64_t>(
	                  ExpressionValueReader::compile(
	                          std::make_shared<VariableReferenceExpression>("duration", "nanoseconds"))
	                          .evaluate(record)
	                          .getVariant()),
	          4'000'000'000LL);
}

TEST(ExpressionValueReaderTest, FallsBackForComplexExpressions) {
	auto left = std::make_unique<VariableReferenceExpression>("x");
	auto right = std::make_unique<LiteralExpression>(int64_t{2});
	auto expression = std::shared_ptr<graph::query::expressions::Expression>(
			new BinaryOpExpression(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right)));
	auto reader = ExpressionValueReader::compile(std::move(expression));
	EXPECT_EQ(reader.kind(), ExpressionValueReaderKind::EVR_GENERIC);

	Record record;
	record.setValue("x", PropertyValue(int64_t{5}));
	EXPECT_EQ(std::get<int64_t>(reader.evaluate(record).getVariant()), 7);
}
