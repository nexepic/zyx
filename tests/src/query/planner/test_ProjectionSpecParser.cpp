/**
 * @file test_ProjectionSpecParser.cpp
 * @author Nexepic
 * @date 2026/7/7
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 **/

#include <gtest/gtest.h>

#include <limits>
#include <utility>

#include "graph/query/planner/ProjectionSpecParser.hpp"

namespace {

using graph::PropertyValue;
using graph::query::algorithm::ProjectionOrientation;
using graph::query::algorithm::ProjectionWeightKind;
using graph::query::planner::ProjectionSpecParser;

PropertyValue::MapType map(std::initializer_list<std::pair<const std::string, PropertyValue>> entries) {
	return PropertyValue::MapType(entries);
}

PropertyValue list(std::initializer_list<PropertyValue> values) {
	return PropertyValue(std::vector<PropertyValue>(values));
}

PropertyValue pvMap(std::initializer_list<std::pair<const std::string, PropertyValue>> entries) {
	return PropertyValue(map(entries));
}

} // namespace

TEST(ProjectionSpecParserTest, ParsesLegacySignature) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("legacy"),
		PropertyValue("Person"),
		PropertyValue("KNOWS"),
		PropertyValue("score"),
	});

	EXPECT_EQ(spec.name, "legacy");
	ASSERT_EQ(spec.nodeLabels.size(), 1u);
	EXPECT_EQ(spec.nodeLabels[0], "Person");
	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_EQ(spec.relationships[0].type, "KNOWS");
	EXPECT_EQ(spec.relationships[0].orientation, ProjectionOrientation::GPO_NATURAL);
	EXPECT_EQ(spec.relationships[0].weight.kind, ProjectionWeightKind::GPWK_PROPERTY);
	EXPECT_EQ(spec.relationships[0].weight.propertyName, "score");
}

TEST(ProjectionSpecParserTest, ProjectionSpecHelpersReflectProjectionShape) {
	graph::query::algorithm::ProjectionSpec spec;
	EXPECT_TRUE(spec.includesAllNodeLabels());
	EXPECT_FALSE(spec.usesWeights());

	spec.nodeLabels.push_back("Person");
	EXPECT_FALSE(spec.includesAllNodeLabels());

	graph::query::algorithm::RelationshipProjectionSpec relationship;
	relationship.weight.kind = ProjectionWeightKind::GPWK_CONSTANT;
	relationship.weight.constantWeight = 2.0;
	spec.relationships.push_back(std::move(relationship));
	EXPECT_TRUE(spec.usesWeights());
}

TEST(ProjectionSpecParserTest, ParsesConfigMapWithMultiLabelsAndRelationshipMap) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("code"),
		pvMap({
			{"nodeLabels", list({PropertyValue("Function"), PropertyValue("Method"), PropertyValue("Class")})},
			{"orientation", PropertyValue("UNDIRECTED")},
			{"relationships", pvMap({
				{"CALLS", pvMap({{"weight", PropertyValue(8.0)}})},
				{"IMPORTS", pvMap({{"weight", PropertyValue("score")}, {"defaultWeight", PropertyValue(2.0)}})},
				{"DECLARES", pvMap({{"orientation", PropertyValue("REVERSE")},
									 {"relationshipWeightProperty", PropertyValue("rank")},
									 {"defaultValue", PropertyValue(1)}})},
			})},
		}),
	});

	EXPECT_EQ(spec.name, "code");
	EXPECT_EQ(spec.defaultOrientation, ProjectionOrientation::GPO_UNDIRECTED);
	EXPECT_EQ(spec.nodeLabels.size(), 3u);
	ASSERT_EQ(spec.relationships.size(), 3u);
	bool sawCalls = false;
	bool sawImports = false;
	bool sawDeclares = false;
	for (const auto &relationship : spec.relationships) {
		if (relationship.type == "CALLS") {
			sawCalls = true;
			EXPECT_EQ(relationship.orientation, ProjectionOrientation::GPO_UNDIRECTED);
			EXPECT_EQ(relationship.weight.kind, ProjectionWeightKind::GPWK_CONSTANT);
			EXPECT_DOUBLE_EQ(relationship.weight.constantWeight, 8.0);
		} else if (relationship.type == "IMPORTS") {
			sawImports = true;
			EXPECT_EQ(relationship.weight.kind, ProjectionWeightKind::GPWK_PROPERTY);
			EXPECT_EQ(relationship.weight.propertyName, "score");
			EXPECT_DOUBLE_EQ(relationship.weight.defaultWeight, 2.0);
		} else if (relationship.type == "DECLARES") {
			sawDeclares = true;
			EXPECT_EQ(relationship.orientation, ProjectionOrientation::GPO_REVERSE);
			EXPECT_EQ(relationship.weight.propertyName, "rank");
			EXPECT_DOUBLE_EQ(relationship.weight.defaultWeight, 1.0);
		}
	}
	EXPECT_TRUE(sawCalls);
	EXPECT_TRUE(sawImports);
	EXPECT_TRUE(sawDeclares);
}

TEST(ProjectionSpecParserTest, ParsesRelationshipTypeAliasesAndAllTokens) {
	auto allNodes = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("all"),
		pvMap({
			{"labels", PropertyValue("*")},
			{"relationshipTypes", list({PropertyValue("*"), PropertyValue("IGNORED")})},
			{"orientation", PropertyValue("DIRECTED")},
		}),
	});
	EXPECT_TRUE(allNodes.nodeLabels.empty());
	ASSERT_EQ(allNodes.relationships.size(), 1u);
	EXPECT_TRUE(allNodes.relationships[0].type.empty());
	EXPECT_EQ(allNodes.relationships[0].orientation, ProjectionOrientation::GPO_NATURAL);

	auto listTypes = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("types"),
		pvMap({
			{"node_labels", list({PropertyValue("Person"), PropertyValue("Person")})},
			{"relationships", list({PropertyValue("KNOWS"), PropertyValue("LIKES")})},
		}),
	});
	ASSERT_EQ(listTypes.nodeLabels.size(), 1u);
	EXPECT_EQ(listTypes.relationships.size(), 2u);
}

TEST(ProjectionSpecParserTest, ParsesRelationshipShortForms) {
	auto stringRelationship = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("single"),
		pvMap({{"relationships", PropertyValue("KNOWS")},
			   {"orientation", PropertyValue("NATURAL")}}),
	});
	ASSERT_EQ(stringRelationship.relationships.size(), 1u);
	EXPECT_EQ(stringRelationship.relationships[0].type, "KNOWS");
	EXPECT_EQ(stringRelationship.relationships[0].orientation, ProjectionOrientation::GPO_NATURAL);

	auto numericWeight = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("numeric"),
		pvMap({{"relationships", pvMap({{"CALLS", PropertyValue(4)}})}}),
	});
	ASSERT_EQ(numericWeight.relationships.size(), 1u);
	EXPECT_EQ(numericWeight.relationships[0].weight.kind, ProjectionWeightKind::GPWK_CONSTANT);
	EXPECT_DOUBLE_EQ(numericWeight.relationships[0].weight.constantWeight, 4.0);

	auto doubleNumericWeight = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("numeric_double"),
		pvMap({{"relationships", pvMap({{"CALLS", PropertyValue(4.5)}})}}),
	});
	ASSERT_EQ(doubleNumericWeight.relationships.size(), 1u);
	EXPECT_EQ(doubleNumericWeight.relationships[0].weight.kind, ProjectionWeightKind::GPWK_CONSTANT);
	EXPECT_DOUBLE_EQ(doubleNumericWeight.relationships[0].weight.constantWeight, 4.5);

	auto allRelationshipString = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("all_string"),
		pvMap({{"relationships", PropertyValue("*")}}),
	});
	ASSERT_EQ(allRelationshipString.relationships.size(), 1u);
	EXPECT_TRUE(allRelationshipString.relationships[0].type.empty());

	auto allRelationshipKey = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("all_key"),
		pvMap({{"relationships", pvMap({{"", pvMap({{"weight", PropertyValue(1.0)}})}})}}),
	});
	ASSERT_EQ(allRelationshipKey.relationships.size(), 1u);
	EXPECT_TRUE(allRelationshipKey.relationships[0].type.empty());

	auto relationshipTypes = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("relationship_types"),
		pvMap({{"relationship_types", list({PropertyValue("A"), PropertyValue("B")})}}),
	});
	ASSERT_EQ(relationshipTypes.relationships.size(), 2u);
	EXPECT_EQ(relationshipTypes.relationships[0].orientation, ProjectionOrientation::GPO_NATURAL);
}

TEST(ProjectionSpecParserTest, ParsesNeo4jStyleNativeSignatureWithGlobalDefaults) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("native"),
		list({PropertyValue("Function"), PropertyValue("Method")}),
		pvMap({
			{"CALLS", pvMap({{"weight", PropertyValue(8.0)}})},
			{"IMPORTS", pvMap({})},
		}),
		pvMap({
			{"orientation", PropertyValue("UNDIRECTED")},
			{"relationshipWeightProperty", PropertyValue("score")},
			{"defaultWeight", PropertyValue(2.0)},
		}),
	});

	EXPECT_EQ(spec.name, "native");
	EXPECT_EQ(spec.defaultOrientation, ProjectionOrientation::GPO_UNDIRECTED);
	ASSERT_EQ(spec.nodeLabels.size(), 2u);
	ASSERT_EQ(spec.relationships.size(), 2u);

	bool sawCalls = false;
	bool sawImports = false;
	for (const auto &relationship : spec.relationships) {
		EXPECT_EQ(relationship.orientation, ProjectionOrientation::GPO_UNDIRECTED);
		if (relationship.type == "CALLS") {
			sawCalls = true;
			EXPECT_EQ(relationship.weight.kind, ProjectionWeightKind::GPWK_CONSTANT);
			EXPECT_DOUBLE_EQ(relationship.weight.constantWeight, 8.0);
		} else if (relationship.type == "IMPORTS") {
			sawImports = true;
			EXPECT_EQ(relationship.weight.kind, ProjectionWeightKind::GPWK_PROPERTY);
			EXPECT_EQ(relationship.weight.propertyName, "score");
			EXPECT_DOUBLE_EQ(relationship.weight.defaultWeight, 2.0);
		}
	}
	EXPECT_TRUE(sawCalls);
	EXPECT_TRUE(sawImports);
}

TEST(ProjectionSpecParserTest, ParsesNativeSignatureWithEmptyRelationshipMapAsAllRelationships) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("native_all"),
		PropertyValue("Person"),
		pvMap({}),
	});

	ASSERT_EQ(spec.nodeLabels.size(), 1u);
	EXPECT_EQ(spec.nodeLabels[0], "Person");
	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_TRUE(spec.relationships[0].type.empty());
	EXPECT_EQ(spec.relationships[0].weight.kind, ProjectionWeightKind::GPWK_NONE);
}

TEST(ProjectionSpecParserTest, ParsesNativeSignatureWithRelationshipListAndWeightOnlyConfig) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("native_list"),
		PropertyValue("Person"),
		list({PropertyValue("KNOWS")}),
		pvMap({{"relationshipWeightProperty", PropertyValue("score")}}),
	});

	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_EQ(spec.relationships[0].type, "KNOWS");
	EXPECT_EQ(spec.relationships[0].weight.kind, ProjectionWeightKind::GPWK_PROPERTY);
	EXPECT_EQ(spec.relationships[0].weight.propertyName, "score");
	EXPECT_DOUBLE_EQ(spec.relationships[0].weight.defaultWeight, 1.0);
}

TEST(ProjectionSpecParserTest, ParsesStringStringNativeSignatureWhenFourthArgIsConfigMap) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("native_strings"),
		PropertyValue("Person"),
		PropertyValue("KNOWS"),
		pvMap({{"orientation", PropertyValue("REVERSE")}}),
	});

	ASSERT_EQ(spec.nodeLabels.size(), 1u);
	EXPECT_EQ(spec.nodeLabels[0], "Person");
	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_EQ(spec.relationships[0].type, "KNOWS");
	EXPECT_EQ(spec.relationships[0].orientation, ProjectionOrientation::GPO_REVERSE);
}

TEST(ProjectionSpecParserTest, ParsesNodeProjectionMapAndRelationshipTypeOverride) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("mapped"),
		pvMap({
			{"fn", pvMap({{"label", PropertyValue("Function")}})},
			{"callable", pvMap({{"labels", list({PropertyValue("Method"), PropertyValue("Constructor")})}})},
		}),
		pvMap({
			{"callProjection", pvMap({{"type", PropertyValue("CALLS")},
									  {"orientation", PropertyValue("REVERSE")}})},
		}),
	});

	EXPECT_EQ(spec.nodeLabels.size(), 3u);
	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_EQ(spec.relationships[0].type, "CALLS");
	EXPECT_EQ(spec.relationships[0].orientation, ProjectionOrientation::GPO_REVERSE);

	auto allTypeOverride = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("mapped_all"),
		PropertyValue("Person"),
		pvMap({{"any", pvMap({{"type", PropertyValue("*")}})}}),
	});
	ASSERT_EQ(allTypeOverride.relationships.size(), 1u);
	EXPECT_TRUE(allTypeOverride.relationships[0].type.empty());

	auto stringAndListLabels = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("mapped_labels"),
		pvMap({
			{"functionAlias", PropertyValue("Function")},
			{"callableAlias", list({PropertyValue("Method"), PropertyValue("Constructor")})},
		}),
		PropertyValue("CALLS"),
	});
	EXPECT_EQ(stringAndListLabels.nodeLabels.size(), 3u);

	auto allLabelsFromKey = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("mapped_key_all"),
		pvMap({{"*", pvMap({})}}),
		PropertyValue("CALLS"),
	});
	EXPECT_TRUE(allLabelsFromKey.nodeLabels.empty());

	auto allLabelsFromConfig = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("mapped_config_all"),
		pvMap({{"everything", pvMap({{"labels", PropertyValue("*")}})}}),
		PropertyValue("CALLS"),
	});
	EXPECT_TRUE(allLabelsFromConfig.nodeLabels.empty());
}

TEST(ProjectionSpecParserTest, DefaultsToAllRelationshipsWhenConfigOmitsRelationships) {
	auto spec = ProjectionSpecParser::parseGraphProjectArgs({
		PropertyValue("default_all"),
		pvMap({{"nodeLabels", PropertyValue("Person")}}),
	});

	ASSERT_EQ(spec.relationships.size(), 1u);
	EXPECT_TRUE(spec.relationships[0].type.empty());
	EXPECT_EQ(spec.relationships[0].orientation, ProjectionOrientation::GPO_NATURAL);
}

TEST(ProjectionSpecParserTest, RejectsInvalidLegacyArguments) {
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({PropertyValue("only"), PropertyValue("Person")}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("x"), PropertyValue("A"), PropertyValue("R"), PropertyValue("w"), PropertyValue("extra")}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue(), PropertyValue("A"), PropertyValue("R")}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue(""), PropertyValue("A"), PropertyValue("R")}),
				 std::runtime_error);
}

TEST(ProjectionSpecParserTest, RejectsInvalidTopLevelConfig) {
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue(""), pvMap({{"nodeLabels", PropertyValue("Person")}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"unknown", PropertyValue(1)}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", PropertyValue("R")},
												  {"relationshipTypes", PropertyValue("R")}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"nodeLabels", PropertyValue(1)}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"orientation", PropertyValue("SIDEWAYS")}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 list({PropertyValue("Person")}),
					 PropertyValue("KNOWS"),
					 PropertyValue(1)}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"Person", pvMap({{"properties", PropertyValue("name")}})}}),
					 PropertyValue("KNOWS")}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"Person", PropertyValue(1)}}),
					 PropertyValue("KNOWS")}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 PropertyValue("Person"),
					 PropertyValue("KNOWS"),
					 pvMap({{"unsupported", PropertyValue(1)}})}),
				 std::runtime_error);
}

TEST(ProjectionSpecParserTest, RejectsInvalidRelationshipConfig) {
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", pvMap({{"R", pvMap({{"bad", PropertyValue(1)}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", pvMap({{"R", PropertyValue("score")}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", PropertyValue(1)}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", list({PropertyValue(1)})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"), pvMap({{"relationships", pvMap({{"R", pvMap({{"orientation", PropertyValue(1)}})}})}})}),
				 std::runtime_error);
}

TEST(ProjectionSpecParserTest, RejectsInvalidWeightConfig) {
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weight", PropertyValue(
																	 std::numeric_limits<double>::quiet_NaN())}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weight", PropertyValue(-1)}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weight", PropertyValue(true)}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weight", PropertyValue(1)},
																	{"weightProperty", PropertyValue("score")}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weightProperty", PropertyValue("")}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weight", PropertyValue("")}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"defaultWeight", PropertyValue(1)}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"relationships", pvMap({{"R", pvMap({{"weightProperty", PropertyValue("score")},
																	{"defaultWeight", PropertyValue(-1)}})}})}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 pvMap({{"nodeLabels", PropertyValue("Person")},
							{"relationshipWeightProperty", PropertyValue("")}})}),
				 std::runtime_error);
	EXPECT_THROW(ProjectionSpecParser::parseGraphProjectArgs({
					 PropertyValue("bad"),
					 PropertyValue("Person"),
					 pvMap({}),
					 pvMap({{"defaultWeight", PropertyValue(3.0)}})}),
				 std::runtime_error);
}
