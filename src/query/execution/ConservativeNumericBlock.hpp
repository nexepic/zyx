#pragma once

#include <bit>
#include <cmath>
#include <span>
#include "query/execution/PrimitiveColumnFilter.hpp"

namespace graph::query::execution::detail {
// A synchronous, block-local proof. It has no persistent identity and must not
// be retained beyond the authenticated source callback. Unknown types, mixed
// constants and NaNs decline. Bounds cover the entire delivered block.
struct ConservativeNumericBlock {
    PropertyType type = PropertyType::NULL_TYPE;
    PropertyValue minimum, maximum;
    bool valid = false;

    template<class Guard>
    static ConservativeNumericBlock inspect(PropertyType type, std::span<const uint64_t> words, Guard guard) {
        ConservativeNumericBlock result;
        result.type = type;
        if (words.empty() || (type != PropertyType::INTEGER && type != PropertyType::DOUBLE)) return result;
        if (type == PropertyType::INTEGER) {
            auto lo = std::bit_cast<int64_t>(words[0]), hi = lo;
            for (size_t i = 0; i < words.size(); ++i) {
                if ((i & 15U) == 0U) guard();
                auto value = std::bit_cast<int64_t>(words[i]); lo = (std::min)(lo, value); hi = (std::max)(hi, value);
            }
            result.minimum = PropertyValue(lo); result.maximum = PropertyValue(hi);
        } else {
            auto lo = std::bit_cast<double>(words[0]), hi = lo;
            for (size_t i = 0; i < words.size(); ++i) {
                if ((i & 15U) == 0U) guard();
                auto value = std::bit_cast<double>(words[i]);
                if (std::isnan(value)) return result;
                lo = (std::min)(lo, value); hi = (std::max)(hi, value);
            }
            result.minimum = PropertyValue(lo); result.maximum = PropertyValue(hi);
        }
        result.valid = true; return result;
    }
    [[nodiscard]] bool cannotMatch(const ColumnPredicate &predicate) const {
        if (!valid || !predicate.expected || predicate.expected->getType() != type) return false;
        if (type == PropertyType::DOUBLE && std::isnan(std::get<double>(predicate.expected->getVariant()))) return false;
        const auto &v = *predicate.expected;
        switch (predicate.operation) {
            case ColumnPredicateOp::CPO_EQUAL: return v < minimum || maximum < v;
            case ColumnPredicateOp::CPO_NOT_EQUAL: return minimum == maximum && minimum == v;
            case ColumnPredicateOp::CPO_LESS: return !(minimum < v);
            case ColumnPredicateOp::CPO_GREATER: return !(v < maximum);
            case ColumnPredicateOp::CPO_LESS_EQUAL: return v < minimum;
            case ColumnPredicateOp::CPO_GREATER_EQUAL: return maximum < v;
            case ColumnPredicateOp::CPO_NON_NULL: return false;
        }
        return false;
    }
};
}
