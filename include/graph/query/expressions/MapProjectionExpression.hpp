/**
 * @file MapProjectionExpression.hpp
 * @date 2025
 *
 * Expression for map projections: n {.name, .age, key: expr, .*}
 */

#pragma once

#include "graph/query/expressions/Expression.hpp"
#include <memory>
#include <string>
#include <vector>

namespace graph::query::expressions {

/**
 * @enum MapProjectionItemType
 * @brief Types of map projection items
 */
enum class MapProjectionItemType {
	MPROP_PROPERTY,     // .name — select a property
	MPROP_LITERAL,      // key: expr — computed value
	MPROP_ALL_PROPERTIES // .* — all properties
};

/**
 * @struct MapProjectionItem
 * @brief A single item in a map projection
 */
struct MapProjectionItem {
	MapProjectionItemType type;
	std::string key;
	std::unique_ptr<Expression> expression; // only for MPROP_LITERAL

	MapProjectionItem(MapProjectionItemType t, std::string k, std::unique_ptr<Expression> expr = nullptr)
		: type(t), key(std::move(k)), expression(std::move(expr)) {}

	MapProjectionItem clone() const {
		return MapProjectionItem(type, key, expression ? expression->clone() : nullptr);
	}
};

/**
 * @class MapProjectionExpression
 * @brief Expression for map projections: n {.name, .age, key: expr, .*}
 */
class MapProjectionExpression : public Expression {
public:
	MapProjectionExpression(std::string variable, std::vector<MapProjectionItem> items)
		: variable_(std::move(variable)), items_(std::move(items)) {}

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::MAP_PROJECTION;
	}

	void accept(ExpressionVisitor &visitor) override { visitor.visit(this); }
	void accept(ConstExpressionVisitor &visitor) const override { visitor.visit(this); }

	[[nodiscard]] std::string toString() const override {
		std::string result = variable_ + " {";
		for (size_t i = 0; i < items_.size(); ++i) {
			if (i > 0) result += ", ";
			switch (items_[i].type) {
				case MapProjectionItemType::MPROP_PROPERTY:
					result += "." + items_[i].key;
					break;
				case MapProjectionItemType::MPROP_LITERAL:
					result += items_[i].key + ": " + (items_[i].expression ? items_[i].expression->toString() : "null");
					break;
				case MapProjectionItemType::MPROP_ALL_PROPERTIES:
					result += ".*";
					break;
			}
		}
		result += "}";
		return result;
	}

	[[nodiscard]] std::unique_ptr<Expression> clone() const override {
		std::vector<MapProjectionItem> clonedItems;
		clonedItems.reserve(items_.size());
		for (const auto& item : items_) {
			clonedItems.push_back(item.clone());
		}
		return std::make_unique<MapProjectionExpression>(variable_, std::move(clonedItems));
	}

	[[nodiscard]] const std::string& getVariable() const { return variable_; }
	[[nodiscard]] const std::vector<MapProjectionItem>& getItems() const { return items_; }

private:
	std::string variable_;
	std::vector<MapProjectionItem> items_;
};

} // namespace graph::query::expressions
