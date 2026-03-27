/**
 * @file AstExtractor.hpp
 * @author Nexepic
 * @date 2025/12/9
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

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "generated/CypherParser.h"
#include "graph/core/PropertyTypes.hpp"

namespace graph::parser::cypher::helpers {

/**
 * @class AstExtractor
 * @brief Static utility class for extracting information from ANTLR4 AST contexts.
 *
 * This class provides methods to extract common elements from Cypher parse tree
 * contexts such as variables, labels, relationship types, and literal values.
 * All methods are static as this is a stateless utility class.
 */
class AstExtractor {
public:
	// --- Variable Extraction ---
	/**
	 * @brief Extract variable name from a VariableContext.
	 * @param ctx The variable context (may be null)
	 * @return The variable name, or empty string if ctx is null
	 */
	static std::string extractVariable(CypherParser::VariableContext *ctx);

	// --- Label Extraction ---
	/**
	 * @brief Extract all label names from NodeLabelsContext.
	 * @param ctx The node labels context (may be null)
	 * @return Vector of label names, empty if ctx is null or has no labels
	 */
	static std::vector<std::string> extractLabels(CypherParser::NodeLabelsContext *ctx);

	/**
	 * @brief Extract first label name from NodeLabelsContext (backward compat).
	 * @param ctx The node labels context (may be null)
	 * @return The first label name, or empty string if ctx is null or has no labels
	 */
	static std::string extractLabel(CypherParser::NodeLabelsContext *ctx);

	/**
	 * @brief Extract label name from a single NodeLabelContext.
	 * @param ctx The node label context (may be null)
	 * @return The label name, or empty string if ctx is null
	 */
	static std::string extractLabelFromNodeLabel(CypherParser::NodeLabelContext *ctx);

	// --- Relationship Type Extraction ---
	/**
	 * @brief Extract relationship type from RelationshipTypesContext.
	 * @param ctx The relationship types context (may be null)
	 * @return The first relationship type, or empty string if ctx is null or has no types
	 */
	static std::string extractRelType(CypherParser::RelationshipTypesContext *ctx);

	// --- Property Extraction ---
	/**
	 * @brief Extract property key from a PropertyExpressionContext.
	 * Handles both n.prop and prop syntaxes.
	 * @param ctx The property expression context (may be null)
	 * @return The property key name, or empty string if ctx is null
	 */
	static std::string extractPropertyKeyFromExpr(CypherParser::PropertyExpressionContext *ctx);

	// --- Literal Value Parsing ---
	/**
	 * @brief Parse a LiteralContext into a PropertyValue.
	 * Handles strings, numbers, booleans, and null.
	 * @param ctx The literal context (may be null)
	 * @return The parsed PropertyValue, or empty PropertyValue if ctx is null
	 */
	static PropertyValue parseValue(CypherParser::LiteralContext *ctx);

	// --- Property Map Extraction ---
	/**
	 * @brief Extract property map from a PropertiesContext.
	 * Evaluates each expression in the map literal.
	 * @param ctx The properties context (may be null)
	 * @param evaluator Function to evaluate expressions into PropertyValue
	 * @return Map of property names to values, empty if ctx is null
	 */
	static std::unordered_map<std::string, PropertyValue> extractProperties(
		CypherParser::PropertiesContext *ctx,
		const std::function<PropertyValue(CypherParser::ExpressionContext *)> &evaluator);
};

} // namespace graph::parser::cypher::helpers
