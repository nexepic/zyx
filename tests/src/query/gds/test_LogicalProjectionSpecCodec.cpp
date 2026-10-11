/**
 * @file test_LogicalProjectionSpecCodec.cpp
 * @brief Contract tests for native gds.graph.project argument decoding.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/gds/LogicalProjectionSpecCodec.hpp"

namespace graph::query::gds {

	namespace {

		[[nodiscard]] PropertyValue map(std::initializer_list<std::pair<const std::string, PropertyValue>> entries) {
			return PropertyValue(PropertyValue::MapType(entries));
		}

		[[nodiscard]] PropertyValue list(std::initializer_list<PropertyValue> entries) {
			return PropertyValue(std::vector<PropertyValue>(entries));
		}

	} // namespace

	TEST(LogicalProjectionSpecCodecTest, DecodesConfigAndNativeSignaturesWithoutStorageDependencies) {
		const LogicalGraphProjectionSpec config = LogicalProjectionSpecCodec::decodeGraphProjectArguments({
				PropertyValue("code"),
				map({
						{"nodeLabels", list({PropertyValue("Function"), PropertyValue("Method")})},
						{"orientation", PropertyValue("UNDIRECTED")},
						{"relationshipWeightProperty", PropertyValue("cost")},
						{"defaultWeight", PropertyValue(2.0)},
						{"relationships", map({
												  {"CALLS", map({{"orientation", PropertyValue("REVERSE")}})},
												  {"IMPORTS", PropertyValue(3.0)},
										  })},
				}),
		});
		EXPECT_EQ(config.name, "code");
		EXPECT_EQ(config.node_labels, (std::vector<std::string>{"Function", "Method"}));
		ASSERT_EQ(config.relationships.size(), 2U);
		const auto calls = std::ranges::find(config.relationships, "CALLS", &LogicalRelationshipProjection::type);
		ASSERT_NE(calls, config.relationships.end());
		EXPECT_EQ(calls->orientation, LogicalProjectionOrientation::LPO_REVERSE);
		EXPECT_EQ(calls->weight.kind, LogicalProjectionWeightKind::LPWK_PROPERTY);
		EXPECT_EQ(calls->weight.property_name, "cost");
		EXPECT_DOUBLE_EQ(calls->weight.default_weight, 2.0);
		const auto imports = std::ranges::find(config.relationships, "IMPORTS", &LogicalRelationshipProjection::type);
		ASSERT_NE(imports, config.relationships.end());
		EXPECT_EQ(imports->weight.kind, LogicalProjectionWeightKind::LPWK_CONSTANT);
		EXPECT_DOUBLE_EQ(imports->weight.constant_weight, 3.0);

		const LogicalGraphProjectionSpec native = LogicalProjectionSpecCodec::decodeGraphProjectArguments({
				PropertyValue("legacy"),
				PropertyValue("Person"),
				PropertyValue("KNOWS"),
				PropertyValue("score"),
		});
		ASSERT_EQ(native.node_labels, (std::vector<std::string>{"Person"}));
		ASSERT_EQ(native.relationships.size(), 1U);
		EXPECT_EQ(native.relationships.front().type, "KNOWS");
		EXPECT_EQ(native.relationships.front().weight.kind, LogicalProjectionWeightKind::LPWK_PROPERTY);
		EXPECT_EQ(native.relationships.front().weight.property_name, "score");
	}

	TEST(LogicalProjectionSpecCodecTest, RejectsAmbiguousUnknownAndUnboundedInputsBeforeProjectionBuild) {
		const auto decode = [](std::vector<PropertyValue> arguments) {
			return LogicalProjectionSpecCodec::decodeGraphProjectArguments(arguments);
		};
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"unknown", PropertyValue(true)}})})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({
																		   {"labels", PropertyValue("Person")},
																		   {"nodeLabels", PropertyValue("User")},
																   })})),
					 std::invalid_argument);
		EXPECT_THROW(
				static_cast<void>(decode({PropertyValue("x"), map({
																	  {"relationships", PropertyValue("KNOWS")},
																	  {"relationshipTypes", PropertyValue("LIKES")},
															  })})),
				std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("Person"), PropertyValue("KNOWS"),
											   PropertyValue(int64_t{1})})),
					 std::invalid_argument);

		const LogicalProjectionSpecCodecBounds tight{.maximum_arguments = 4U,
													 .maximum_map_entries = 2U,
													 .maximum_list_entries = 1U,
													 .maximum_text_bytes = 16U};
		EXPECT_THROW(
				static_cast<void>(LogicalProjectionSpecCodec::decodeGraphProjectArguments(
						{PropertyValue("x"), map({{"nodeLabels", list({PropertyValue("A"), PropertyValue("B")})}})},
						tight)),
				std::invalid_argument);
	}

	TEST(LogicalProjectionSpecCodecTest, DecodesStructuredSelectorsAndNormalizesTokenSelectors) {
		const LogicalGraphProjectionSpec structured = LogicalProjectionSpecCodec::decodeGraphProjectArguments({
				PropertyValue("structured"),
				map({{"CodeAlias", map({{"labels", list({PropertyValue("Code"), PropertyValue("Service")})}})}}),
				map({
						{"CALLS", map({{"type", PropertyValue("INVOKES")},
									   {"orientation", PropertyValue("REVERSE")},
									   {"weightProperty", PropertyValue("cost")},
									   {"defaultWeight", PropertyValue(1.5)}})},
						{"USES", PropertyValue(3.0)},
				}),
				map({{"orientation", PropertyValue("UNDIRECTED")},
					 {"relationshipWeightProperty", PropertyValue("fallback")},
					 {"defaultWeight", PropertyValue(2.0)}}),
		});
		EXPECT_EQ(structured.node_labels, (std::vector<std::string>{"Code", "Service"}));
		EXPECT_EQ(structured.default_orientation, LogicalProjectionOrientation::LPO_UNDIRECTED);
		ASSERT_EQ(structured.relationships.size(), 2U);
		const auto invokes =
				std::ranges::find(structured.relationships, "INVOKES", &LogicalRelationshipProjection::type);
		ASSERT_NE(invokes, structured.relationships.end());
		EXPECT_EQ(invokes->orientation, LogicalProjectionOrientation::LPO_REVERSE);
		EXPECT_EQ(invokes->weight.kind, LogicalProjectionWeightKind::LPWK_PROPERTY);
		EXPECT_EQ(invokes->weight.property_name, "cost");
		EXPECT_DOUBLE_EQ(invokes->weight.default_weight, 1.5);
		const auto uses = std::ranges::find(structured.relationships, "USES", &LogicalRelationshipProjection::type);
		ASSERT_NE(uses, structured.relationships.end());
		EXPECT_EQ(uses->orientation, LogicalProjectionOrientation::LPO_UNDIRECTED);
		EXPECT_EQ(uses->weight.kind, LogicalProjectionWeightKind::LPWK_CONSTANT);
		EXPECT_DOUBLE_EQ(uses->weight.constant_weight, 3.0);

		const LogicalGraphProjectionSpec normalized = LogicalProjectionSpecCodec::decodeGraphProjectArguments({
				PropertyValue("all"),
				map({{"nodeLabels", list({PropertyValue("Code"), PropertyValue("Code"), PropertyValue("*")})},
					 {"relationshipTypes", list({PropertyValue("CALLS"), PropertyValue("CALLS"), PropertyValue("")})}}),
		});
		EXPECT_TRUE(normalized.node_labels.empty());
		ASSERT_EQ(normalized.relationships.size(), 1U);
		EXPECT_TRUE(normalized.relationships.front().type.empty());
		EXPECT_EQ(normalized.relationships.front().orientation, LogicalProjectionOrientation::LPO_NATURAL);
	}

	TEST(LogicalProjectionSpecCodecTest, RejectsInvalidWeightsStructuredEntriesAndBounds) {
		const auto decode = [](std::vector<PropertyValue> arguments,
							   const LogicalProjectionSpecCodecBounds bounds = LogicalProjectionSpecCodecBounds{}) {
			return LogicalProjectionSpecCodec::decodeGraphProjectArguments(arguments, bounds);
		};
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x")})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"defaultWeight", PropertyValue(1.0)}})})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(
							 decode({PropertyValue("x"),
									 map({{"nodeLabels", map({
																 {"Alias", map({{"unknown", PropertyValue(true)}})},
														 })}})})),
					 std::invalid_argument);
		EXPECT_THROW(
				static_cast<void>(decode(
						{PropertyValue("x"),
						 map({{"relationships", map({
														{"CALLS", map({{"weight", PropertyValue(1.0)},
																	   {"weightProperty", PropertyValue("cost")}})},
												})}})})),
				std::invalid_argument);
		EXPECT_THROW(static_cast<void>(
							 decode({PropertyValue("x"),
									 map({{"relationships", map({
																	{"CALLS", map({{"weight", PropertyValue(-1.0)}})},
															})}})})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode(
							 {PropertyValue("x"),
							  map({{"relationships",
									map({
											{"CALLS",
											 map({{"weight", PropertyValue(std::numeric_limits<double>::infinity())}})},
									})}})})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("Code"), PropertyValue("CALLS"),
											   map({{"orientation", PropertyValue("SIDEWAYS")}})})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("Code"), PropertyValue("CALLS")},
											  {.maximum_arguments = 3U,
											   .maximum_map_entries = 1U,
											   .maximum_list_entries = 1U,
											   .maximum_text_bytes = 1U})),
					 std::invalid_argument);
	}

	TEST(LogicalProjectionSpecCodecTest, CoversAlternatePublicSignaturesAliasesAndInputLimits) {
		const auto decode = [](std::vector<PropertyValue> arguments,
							   const LogicalProjectionSpecCodecBounds bounds = LogicalProjectionSpecCodecBounds{}) {
			return LogicalProjectionSpecCodec::decodeGraphProjectArguments(arguments, bounds);
		};

		EXPECT_THROW(static_cast<void>(decode({PropertyValue(int64_t{1}), map({})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("xx"), map({})},
											 {.maximum_arguments = 4U,
											  .maximum_map_entries = 1U,
											  .maximum_list_entries = 1U,
											  .maximum_text_bytes = 1U})),
					 std::invalid_argument);
		for (const LogicalProjectionSpecCodecBounds invalidBounds:
				 std::array<LogicalProjectionSpecCodecBounds, 4U>{{
						 {.maximum_arguments = 3U, .maximum_map_entries = 1U, .maximum_list_entries = 1U,
						  .maximum_text_bytes = 1U},
						 {.maximum_arguments = 4U, .maximum_map_entries = 0U, .maximum_list_entries = 1U,
						  .maximum_text_bytes = 1U},
						 {.maximum_arguments = 4U, .maximum_map_entries = 1U, .maximum_list_entries = 0U,
						  .maximum_text_bytes = 1U},
						 {.maximum_arguments = 4U, .maximum_map_entries = 1U, .maximum_list_entries = 1U,
						  .maximum_text_bytes = 0U},
				 }}) {
			EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({})}, invalidBounds)), std::invalid_argument);
		}

		const LogicalGraphProjectionSpec aliases = decode({
				PropertyValue("aliases"),
				map({
						{"labels", PropertyValue("*")},
						{"orientation", PropertyValue("directed")},
						{"relationshipTypes", list({PropertyValue("CALLS"), PropertyValue("USES")})},
				}),
		});
		EXPECT_TRUE(aliases.node_labels.empty());
		ASSERT_EQ(aliases.relationships.size(), 2U);
		EXPECT_EQ(aliases.relationships[0U].orientation, LogicalProjectionOrientation::LPO_NATURAL);

		const LogicalGraphProjectionSpec numericAndStructured = decode({
				PropertyValue("structured-alternates"),
				map({{"Alias", PropertyValue("Service")}}),
				map({
						{"CALLS", PropertyValue(int64_t{2})},
						{"*", map({{"orientation", PropertyValue("UNDIRECTED")}})},
				}),
				map({{"orientation", PropertyValue("REVERSE")}}),
		});
		ASSERT_EQ(numericAndStructured.node_labels, (std::vector<std::string>{"Service"}));
		ASSERT_EQ(numericAndStructured.relationships.size(), 2U);
		const auto calls = std::ranges::find(numericAndStructured.relationships, "CALLS",
				&LogicalRelationshipProjection::type);
		ASSERT_NE(calls, numericAndStructured.relationships.end());
		EXPECT_EQ(calls->weight.kind, LogicalProjectionWeightKind::LPWK_CONSTANT);
		EXPECT_DOUBLE_EQ(calls->weight.constant_weight, 2.0);

		const LogicalGraphProjectionSpec legacyAllTokens = decode({
				PropertyValue("legacy-all"), PropertyValue(""), PropertyValue("*"), PropertyValue("")});
		EXPECT_TRUE(legacyAllTokens.node_labels.empty());
		ASSERT_EQ(legacyAllTokens.relationships.size(), 1U);
		EXPECT_TRUE(legacyAllTokens.relationships.front().type.empty());
		EXPECT_EQ(legacyAllTokens.relationships.front().weight.kind, LogicalProjectionWeightKind::LPWK_NONE);

		EXPECT_THROW(static_cast<void>(decode({
							 PropertyValue("x"),
							 map({{"relationships", map({
													 {"CALLS", map({{"weightProperty", PropertyValue("cost")},
																		 {"weight_property", PropertyValue("cost2")}})},
												 })}}),
						 })),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("Code"),
											 PropertyValue("CALLS"), list({})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"nodeLabels", PropertyValue(true)}})})),
					 std::invalid_argument);
	}

	TEST(LogicalProjectionSpecCodecTest, CoversProjectionVariantsAndResourceRejections) {
		const auto decode = [](std::vector<PropertyValue> arguments,
							   const LogicalProjectionSpecCodecBounds bounds = LogicalProjectionSpecCodecBounds{}) {
			return LogicalProjectionSpecCodec::decodeGraphProjectArguments(arguments, bounds);
		};
		const LogicalProjectionSpecCodecBounds tiny{.maximum_arguments = 4U,
				.maximum_map_entries = 1U, .maximum_list_entries = 1U, .maximum_text_bytes = 1U};

		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"a", PropertyValue(true)}, {"b", PropertyValue(true)}})}, tiny)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"xx", PropertyValue(true)}})}, tiny)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"labels", PropertyValue("xx")}})}, tiny)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue(true)})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("Code"), PropertyValue("CALLS"), PropertyValue("x"), PropertyValue("y")})), std::invalid_argument);

		const LogicalGraphProjectionSpec natural = decode({PropertyValue("natural"), map({
				{"orientation", PropertyValue("NATURAL")},
				{"relationships", list({PropertyValue("CALLS"), PropertyValue("USES")})},
		})});
		ASSERT_EQ(natural.relationships.size(), 2U);
		EXPECT_EQ(natural.relationships.front().orientation, LogicalProjectionOrientation::LPO_NATURAL);

		const LogicalGraphProjectionSpec wildcardNode = decode({PropertyValue("wildcard-node"),
				map({{"*", PropertyValue("Code")}}), PropertyValue("CALLS")});
		EXPECT_TRUE(wildcardNode.node_labels.empty());
		const LogicalGraphProjectionSpec emptyNode = decode({PropertyValue("empty-node"),
				map({{"Alias", PropertyValue("")}}), PropertyValue("CALLS")});
		EXPECT_TRUE(emptyNode.node_labels.empty());

		const LogicalGraphProjectionSpec weighted = decode({PropertyValue("weighted"), map({
				{"relationshipWeightProperty", PropertyValue("fallback")},
				{"relationships", map({
						{"CALLS", map({{"weight", PropertyValue("cost")}})},
						{"USES", map({{"type", PropertyValue("*")}})},
				})},
		})});
		ASSERT_EQ(weighted.relationships.size(), 2U);
		const auto calls = std::ranges::find(weighted.relationships, "CALLS", &LogicalRelationshipProjection::type);
		ASSERT_NE(calls, weighted.relationships.end());
		EXPECT_EQ(calls->weight.kind, LogicalProjectionWeightKind::LPWK_PROPERTY);
		EXPECT_EQ(calls->weight.property_name, "cost");
		const auto wildcard = std::ranges::find(weighted.relationships, "", &LogicalRelationshipProjection::type);
		ASSERT_NE(wildcard, weighted.relationships.end());
		EXPECT_EQ(wildcard->weight.property_name, "fallback");

		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", map({
				{"CALLS", map({{"weight", PropertyValue(true)}})},
		})}})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", map({
				{"CALLS", map({{"weight", PropertyValue(1.0)}, {"defaultWeight", PropertyValue(1.0)}})},
		})}})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", PropertyValue(true)}})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", map({{"xx", PropertyValue(1.0)}})}})}, tiny)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", PropertyValue("xx")}})}, tiny)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), map({{"relationships", map({{"CALLS", PropertyValue(1.0)}})}}), PropertyValue(true)})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("C"), map({{"CALLS", PropertyValue(true)}})})), std::invalid_argument);
		const LogicalGraphProjectionSpec wildcardRelationship = decode({PropertyValue("wildcard-relationship"), map({{"relationships", PropertyValue("*")}})});
		ASSERT_EQ(wildcardRelationship.relationships.size(), 1U);
		EXPECT_TRUE(wildcardRelationship.relationships.front().type.empty());
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("C"), map({{"xx", PropertyValue(1.0)}})},
				{.maximum_arguments = 4U, .maximum_map_entries = 2U, .maximum_list_entries = 1U, .maximum_text_bytes = 1U})), std::invalid_argument);
		const LogicalGraphProjectionSpec defaultsWithoutOrientation = decode({PropertyValue("defaults"), PropertyValue("C"),
				PropertyValue("CALLS"), map({{"relationshipWeightProperty", PropertyValue("cost")}})});
		EXPECT_EQ(defaultsWithoutOrientation.default_orientation, LogicalProjectionOrientation::LPO_NATURAL);
		EXPECT_THROW(static_cast<void>(decode({})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue(""), map({})})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("C"), PropertyValue("R"), PropertyValue("w"), PropertyValue("extra")},
				{.maximum_arguments = 5U, .maximum_map_entries = 1U, .maximum_list_entries = 1U, .maximum_text_bytes = 8U})), std::invalid_argument);
		EXPECT_TRUE(decode({PropertyValue("x"), map({{"xx", PropertyValue("C")}}), PropertyValue("R")},
				{.maximum_arguments = 4U, .maximum_map_entries = 2U, .maximum_list_entries = 1U, .maximum_text_bytes = 1U}).node_labels.empty());
		EXPECT_THROW(static_cast<void>(decode({PropertyValue("x"), PropertyValue("C"), PropertyValue(true)})), std::invalid_argument);
		const LogicalGraphProjectionSpec legacyThree = decode({PropertyValue("legacy-three"), PropertyValue("C"), PropertyValue("R")});
		EXPECT_EQ(legacyThree.relationships.front().weight.kind, LogicalProjectionWeightKind::LPWK_NONE);
		const LogicalGraphProjectionSpec emptyRelationships = decode({PropertyValue("empty-relationships"), PropertyValue("C"), list({})});
		ASSERT_EQ(emptyRelationships.relationships.size(), 1U);
		EXPECT_TRUE(emptyRelationships.relationships.front().type.empty());
	}

} // namespace graph::query::gds
