/**
 * @file AstExtractor.cpp
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

#include "../AstExtractor.hpp"

namespace graph::parser::cypher::helpers {

std::string AstExtractor::extractVariable(CypherParser::VariableContext *ctx) {
	return ctx ? ctx->getText() : "";
}

std::vector<std::string> AstExtractor::extractLabels(CypherParser::NodeLabelsContext *ctx) {
	std::vector<std::string> labels;
	if (!ctx)
		return labels;
	for (auto *nodeLabel : ctx->nodeLabel()) {
		labels.push_back(nodeLabel->labelName()->getText());
	}
	return labels;
}

std::string AstExtractor::extractLabel(CypherParser::NodeLabelsContext *ctx) {
	if (!ctx || ctx->nodeLabel().empty())
		return "";
	return ctx->nodeLabel(0)->labelName()->getText();
}

std::string AstExtractor::extractLabelFromNodeLabel(CypherParser::NodeLabelContext *ctx) {
	return ctx ? ctx->labelName()->getText() : "";
}

std::string AstExtractor::extractRelType(CypherParser::RelationshipTypesContext *ctx) {
	if (!ctx || ctx->relTypeName().empty())
		return "";
	return ctx->relTypeName(0)->getText();
}

std::string AstExtractor::extractPropertyKeyFromExpr(CypherParser::PropertyExpressionContext *ctx) {
	if (!ctx)
		return "";

	// propertyExpression -> atom (DOT propertyKeyName)*

	// Case A: n.age (with DOTs)
	if (!ctx->propertyKeyName().empty()) {
		// Return the last part (the key)
		return ctx->propertyKeyName().back()->getText();
	}

	// Case B: age (legacy or simplified syntax - no DOTs)
	// The key is inside the atom (variable)
	if (ctx->atom()) {
		return ctx->atom()->getText();
	}

	return "";
}

PropertyValue AstExtractor::parseValue(CypherParser::LiteralContext *ctx) {
	if (!ctx)
		return PropertyValue();

	// String literal
	if (ctx->StringLiteral()) {
		std::string s = ctx->StringLiteral()->getText();
		// Remove quotes and unescape
		std::string raw = s.substr(1, s.length() - 2);
		std::string unescaped;
		unescaped.reserve(raw.size());
		for (size_t i = 0; i < raw.size(); ++i) {
			if (raw[i] == '\\' && i + 1 < raw.size()) {
				char next = raw[i + 1];
				switch (next) {
					case 'b': unescaped += '\b'; ++i; break;
					case 't': unescaped += '\t'; ++i; break;
					case 'n': unescaped += '\n'; ++i; break;
					case 'f': unescaped += '\f'; ++i; break;
					case 'r': unescaped += '\r'; ++i; break;
					case '"': unescaped += '"'; ++i; break;
					case '\'': unescaped += '\''; ++i; break;
					case '\\': unescaped += '\\'; ++i; break;
					case 'u':
						if (i + 5 < raw.size()) {
							std::string hex = raw.substr(i + 2, 4);
							char32_t cp = static_cast<char32_t>(std::stoul(hex, nullptr, 16));
							if (cp <= 0x7F) {
								unescaped += static_cast<char>(cp);
							}
							i += 5;
						} else {
							unescaped += raw[i];
						}
						break;
					default: unescaped += raw[i]; break;
				}
			} else {
				unescaped += raw[i];
			}
		}
		return PropertyValue(unescaped);
	}

	// Number literal (integer or float)
	if (ctx->numberLiteral()) {
		std::string s = ctx->numberLiteral()->getText();
		// Check for decimal point or scientific notation
		if (s.find('.') != std::string::npos || s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
			// Force double constructor to avoid integer template deduction
			double dval = std::stod(s);
			return PropertyValue(dval);
		}
		// For integers without decimal point or exponent, use stoll to avoid float conversion
		return PropertyValue(std::stoll(s));
	}

	// Boolean literal
	if (ctx->booleanLiteral()) {
		return PropertyValue(ctx->booleanLiteral()->K_TRUE() != nullptr);
	}

	// Null literal
	if (ctx->K_NULL())
		return PropertyValue();

	// Unknown type - return empty
	return PropertyValue();
}

std::unordered_map<std::string, PropertyValue> AstExtractor::extractProperties(
	CypherParser::PropertiesContext *ctx,
	const std::function<PropertyValue(CypherParser::ExpressionContext *)> &evaluator) {

	std::unordered_map<std::string, PropertyValue> props;
	if (!ctx || !ctx->mapLiteral())
		return props;

	auto mapLit = ctx->mapLiteral();
	auto keys = mapLit->propertyKeyName();
	auto exprs = mapLit->expression();

	// Parse each key-value pair
	for (size_t i = 0; i < keys.size(); ++i) {
		std::string key = keys[i]->getText();
		// Use the provided evaluator to convert expression to PropertyValue
		props[key] = evaluator(exprs[i]);
	}

	return props;
}

} // namespace graph::parser::cypher::helpers
