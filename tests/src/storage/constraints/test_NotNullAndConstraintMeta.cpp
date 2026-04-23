/**
 * @file test_NotNullAndConstraintMeta.cpp
 * @brief Focused tests for NotNullConstraint and ConstraintMetadata parsing.
 */

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "graph/storage/constraints/ConstraintMeta.hpp"
#include "graph/storage/constraints/NotNullConstraint.hpp"

using namespace graph;
using namespace graph::storage::constraints;

TEST(NotNullConstraintTest, MetadataAndHappyPathValidation) {
	NotNullConstraint c("nn_person_name", "Person", "name");

	EXPECT_EQ(c.getType(), ConstraintType::CT_NOT_NULL);
	EXPECT_EQ(c.getName(), "nn_person_name");
	EXPECT_EQ(c.getLabel(), "Person");
	EXPECT_EQ(c.getProperties(), std::vector<std::string>({"name"}));

	std::unordered_map<std::string, PropertyValue> oldProps{{"name", PropertyValue("Ada")}};
	std::unordered_map<std::string, PropertyValue> newProps{{"name", PropertyValue("Grace")}};

	EXPECT_NO_THROW(c.validateInsert(1, newProps));
	EXPECT_NO_THROW(c.validateUpdate(1, oldProps, newProps));
}

TEST(NotNullConstraintTest, InsertThrowsForMissingOrNullProperty) {
	NotNullConstraint c("nn_person_name", "Person", "name");

	std::unordered_map<std::string, PropertyValue> missing;
	EXPECT_THROW(c.validateInsert(1, missing), std::runtime_error);

	std::unordered_map<std::string, PropertyValue> nullProps{{"name", PropertyValue()}};
	EXPECT_THROW(c.validateInsert(1, nullProps), std::runtime_error);
}

TEST(NotNullConstraintTest, UpdateThrowsForMissingOrNullProperty) {
	NotNullConstraint c("nn_person_name", "Person", "name");

	std::unordered_map<std::string, PropertyValue> oldProps{{"name", PropertyValue("Ada")}};
	std::unordered_map<std::string, PropertyValue> missingNew;
	EXPECT_THROW(c.validateUpdate(1, oldProps, missingNew), std::runtime_error);

	std::unordered_map<std::string, PropertyValue> nullNew{{"name", PropertyValue()}};
	EXPECT_THROW(c.validateUpdate(1, oldProps, nullNew), std::runtime_error);
}

TEST(ConstraintMetadataTest, FromStringHandlesTrailingSeparator) {
	ConstraintMetadata meta{"c1", "node", "not_null", "Person", "name", ""};
	const auto encoded = meta.toString();

	auto parsed = ConstraintMetadata::fromString("c1", encoded);
	EXPECT_EQ(parsed.name, "c1");
	EXPECT_EQ(parsed.entityType, "node");
	EXPECT_EQ(parsed.constraintType, "not_null");
	EXPECT_EQ(parsed.label, "Person");
	EXPECT_EQ(parsed.properties, "name");
	EXPECT_EQ(parsed.options, "");
}

TEST(ConstraintMetadataTest, FromStringHandlesNonTrailingAndCorruptInput) {
	auto parsed = ConstraintMetadata::fromString("c2", "node|unique|Person|email|case_sensitive");
	EXPECT_EQ(parsed.name, "c2");
	EXPECT_EQ(parsed.entityType, "node");
	EXPECT_EQ(parsed.constraintType, "unique");
	EXPECT_EQ(parsed.label, "Person");
	EXPECT_EQ(parsed.properties, "email");
	EXPECT_EQ(parsed.options, "case_sensitive");

	auto corrupt = ConstraintMetadata::fromString("broken", "node|unique|Person");
	EXPECT_EQ(corrupt.name, "broken");
	EXPECT_EQ(corrupt.entityType, "unknown");
	EXPECT_EQ(corrupt.constraintType, "unknown");
	EXPECT_EQ(corrupt.label, "");
	EXPECT_EQ(corrupt.properties, "");
	EXPECT_EQ(corrupt.options, "");
}

// Covers the str.empty() branch in fromString: when the input string is empty
// the trailing-'|' guard takes the false branch (str is empty), parts is also
// empty (<5), so the corrupt fallback path is taken.
TEST(ConstraintMetadataTest, FromStringEmptyInputReturnsUnknown) {
	auto result = ConstraintMetadata::fromString("empty_name", "");
	EXPECT_EQ(result.name, "empty_name");
	EXPECT_EQ(result.entityType, "unknown");
	EXPECT_EQ(result.constraintType, "unknown");
}

// Covers toString() round-trip with non-empty options field (no trailing '|' appended).
TEST(ConstraintMetadataTest, ToStringIncludesOptions) {
	ConstraintMetadata meta{"c3", "node", "property_type", "Person", "age", "INTEGER"};
	auto s = meta.toString();
	EXPECT_EQ(s, "node|property_type|Person|age|INTEGER");
}

