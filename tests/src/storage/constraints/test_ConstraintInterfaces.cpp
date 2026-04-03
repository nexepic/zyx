/**
 * @file test_ConstraintInterfaces.cpp
 * @brief Unit tests for constraint interfaces and TypeConstraint.
 */

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "graph/storage/constraints/IConstraint.hpp"
#include "graph/storage/constraints/IEntityValidator.hpp"
#include "graph/storage/constraints/TypeConstraint.hpp"

using namespace graph;
using namespace graph::storage::constraints;

namespace {

class DummyConstraint final : public IConstraint {
public:
	void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) override {
		lastInsertId = entityId;
		lastInsertProps = props;
	}

	void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override {
		lastUpdateId = entityId;
		lastOldProps = oldProps;
		lastNewProps = newProps;
	}

	ConstraintType getType() const override { return ConstraintType::CT_NOT_NULL; }
	std::string getName() const override { return "dummy_constraint"; }
	std::string getLabel() const override { return "Person"; }
	std::vector<std::string> getProperties() const override { return {"name"}; }

	int64_t lastInsertId = 0;
	int64_t lastUpdateId = 0;
	std::unordered_map<std::string, PropertyValue> lastInsertProps;
	std::unordered_map<std::string, PropertyValue> lastOldProps;
	std::unordered_map<std::string, PropertyValue> lastNewProps;
};

} // namespace

TEST(ConstraintInterfaceTest, DefaultGetOptionsFromBaseClass) {
	DummyConstraint impl;
	IConstraint *base = &impl;

	EXPECT_EQ(base->getOptions(), "");
}

TEST(ConstraintInterfaceTest, ValidateMethodsAndMetadata) {
	DummyConstraint c;
	std::unordered_map<std::string, PropertyValue> props{{"name", PropertyValue("Ada")}};
	std::unordered_map<std::string, PropertyValue> oldProps{{"name", PropertyValue("Ada")}};
	std::unordered_map<std::string, PropertyValue> newProps{{"name", PropertyValue("Grace")}};

	c.validateInsert(7, props);
	c.validateUpdate(7, oldProps, newProps);

	EXPECT_EQ(c.lastInsertId, 7);
	EXPECT_EQ(c.lastUpdateId, 7);
	EXPECT_EQ(c.lastInsertProps.at("name").toString(), "Ada");
	EXPECT_EQ(c.lastOldProps.at("name").toString(), "Ada");
	EXPECT_EQ(c.lastNewProps.at("name").toString(), "Grace");

	EXPECT_EQ(c.getType(), ConstraintType::CT_NOT_NULL);
	EXPECT_EQ(c.getName(), "dummy_constraint");
	EXPECT_EQ(c.getLabel(), "Person");
	EXPECT_EQ(c.getProperties(), std::vector<std::string>({"name"}));
}

TEST(EntityValidatorInterfaceTest, DefaultHooksAreNoOps) {
	IEntityValidator validator;
	Node node;
	Edge edge;
	std::unordered_map<std::string, PropertyValue> oldProps{{"k", PropertyValue(int64_t(1))}};
	std::unordered_map<std::string, PropertyValue> newProps{{"k", PropertyValue(int64_t(2))}};

	EXPECT_NO_THROW(validator.validateNodeInsert(node, newProps));
	EXPECT_NO_THROW(validator.validateNodeUpdate(node, oldProps, newProps));
	EXPECT_NO_THROW(validator.validateNodeDelete(node));

	EXPECT_NO_THROW(validator.validateEdgeInsert(edge, newProps));
	EXPECT_NO_THROW(validator.validateEdgeUpdate(edge, oldProps, newProps));
	EXPECT_NO_THROW(validator.validateEdgeDelete(edge));
}

TEST(TypeConstraintTest, MetadataAndOptions) {
	TypeConstraint c("type_age", "Person", "age", PropertyType::INTEGER);

	EXPECT_EQ(c.getType(), ConstraintType::CT_PROPERTY_TYPE);
	EXPECT_EQ(c.getName(), "type_age");
	EXPECT_EQ(c.getLabel(), "Person");
	EXPECT_EQ(c.getProperties(), std::vector<std::string>({"age"}));
	EXPECT_EQ(c.getOptions(), "INTEGER");
}

TEST(TypeConstraintTest, ParsePropertyTypeCoversAllSupportedAliases) {
	EXPECT_EQ(TypeConstraint::parsePropertyType("BOOLEAN"), PropertyType::BOOLEAN);
	EXPECT_EQ(TypeConstraint::parsePropertyType("integer"), PropertyType::INTEGER);
	EXPECT_EQ(TypeConstraint::parsePropertyType("FLOAT"), PropertyType::DOUBLE);
	EXPECT_EQ(TypeConstraint::parsePropertyType("double"), PropertyType::DOUBLE);
	EXPECT_EQ(TypeConstraint::parsePropertyType("String"), PropertyType::STRING);
	EXPECT_EQ(TypeConstraint::parsePropertyType("list"), PropertyType::LIST);
	EXPECT_EQ(TypeConstraint::parsePropertyType("MAP"), PropertyType::MAP);
	EXPECT_THROW(TypeConstraint::parsePropertyType("VECTOR"), std::runtime_error);
}

TEST(TypeConstraintTest, PropertyTypeToStringHasFallback) {
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::BOOLEAN), "BOOLEAN");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::INTEGER), "INTEGER");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::DOUBLE), "FLOAT");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::STRING), "STRING");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::LIST), "LIST");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(PropertyType::MAP), "MAP");
	EXPECT_EQ(TypeConstraint::propertyTypeToString(static_cast<PropertyType>(255)), "UNKNOWN");
}

TEST(TypeConstraintTest, ValidateInsertAndUpdateTypeChecks) {
	TypeConstraint c("type_age", "Person", "age", PropertyType::INTEGER);

	std::unordered_map<std::string, PropertyValue> missingProp;
	EXPECT_NO_THROW(c.validateInsert(1, missingProp));

	std::unordered_map<std::string, PropertyValue> nullProp{{"age", PropertyValue()}};
	EXPECT_NO_THROW(c.validateInsert(1, nullProp));

	std::unordered_map<std::string, PropertyValue> okProp{{"age", PropertyValue(int64_t(30))}};
	EXPECT_NO_THROW(c.validateInsert(1, okProp));

	std::unordered_map<std::string, PropertyValue> badProp{{"age", PropertyValue(std::string("thirty"))}};
	EXPECT_THROW(c.validateInsert(1, badProp), std::runtime_error);

	std::unordered_map<std::string, PropertyValue> oldProps{{"age", PropertyValue(int64_t(20))}};
	EXPECT_THROW(c.validateUpdate(1, oldProps, badProp), std::runtime_error);
	EXPECT_NO_THROW(c.validateUpdate(1, oldProps, okProp));
}
