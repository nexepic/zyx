/**
 * @file MapProjectionExpression.hpp
 * @date 2025
 *
 * Expression for map projections: n {.name, .age, key: expr, .*}
 */

#pragma once
#include "graph/utils/OwnedStdString.hpp"

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
    using OwnedHeapElement = void;
	MapProjectionItemType type;
	utils::OwnedStdString key;
	std::unique_ptr<Expression> expression; // only for MPROP_LITERAL

	MapProjectionItem(MapProjectionItemType t, utils::OwnedStdString k, std::unique_ptr<Expression> expr = nullptr)
		: type(t), key(std::move(k)), expression(std::move(expr)) {}

    MapProjectionItem(const MapProjectionItem &source)
        : type(source.type), key(source.key), expression(source.expression ? source.expression->clone() : nullptr) {}
    MapProjectionItem(MapProjectionItem &&) = default;
    MapProjectionItem &operator=(const MapProjectionItem &source) {
        if (this != &source) {
            MapProjectionItem copy(source);
            std::swap(type, copy.type); key.swap(copy.key); expression.swap(copy.expression);
        }
        return *this;
    }
    MapProjectionItem &operator=(MapProjectionItem &&source) {
        if (this != &source) {
            MapProjectionItem moved(std::move(source));
            std::swap(type, moved.type); key.swap(moved.key); expression.swap(moved.expression);
        }
        return *this;
    }

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
	MapProjectionExpression(utils::OwnedStdString variable, utils::OwnedStdVector<MapProjectionItem> items)
		: variable_(std::move(variable)), items_(std::move(items)) {}

	// A map literal shares the expression-owned item representation used by a
	// projection, but deliberately has no source binding to dereference.
	explicit MapProjectionExpression(utils::OwnedStdVector<MapProjectionItem> items)
		: items_(std::move(items)), is_map_literal_(true) {}

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::MAP_PROJECTION;
	}

	void accept(ExpressionVisitor &visitor) override { visitor.visit(this); }
	void accept(ConstExpressionVisitor &visitor) const override { visitor.visit(this); }

	[[nodiscard]] std::string toString() const override {
		std::string result = is_map_literal_ ? "{" : variable_ + " {";
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
		utils::OwnedStdVector<MapProjectionItem> clonedItems;
		clonedItems.reserve(items_.size());
		for (const auto& item : items_) {
			clonedItems.push_back(item.clone());
		}
		if (is_map_literal_) {
			return std::make_unique<MapProjectionExpression>(std::move(clonedItems));
		}
		return std::make_unique<MapProjectionExpression>(variable_, std::move(clonedItems));
	}

	[[nodiscard]] const std::string& getVariable() const { return variable_; }
	[[nodiscard]] const std::vector<MapProjectionItem>& getItems() const { return items_; }
	[[nodiscard]] bool isMapLiteral() const { return is_map_literal_; }

private:
	utils::OwnedStdString variable_;
	utils::OwnedStdVector<MapProjectionItem> items_;
	bool is_map_literal_ = false;
};

} // namespace graph::query::expressions
