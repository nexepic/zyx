/**
 * @file OwnedExpressionMap.hpp
 * @brief Owned string-keyed logical expression metadata.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/query/expressions/Expression.hpp"
#include "graph/utils/OwnedSharedStringMap.hpp"
namespace graph::query::expressions {
using OwnedExpressionMap = utils::OwnedSharedStringMap<Expression>;
inline OwnedExpressionMap cloneExpressionMap(const std::unordered_map<std::string, std::shared_ptr<Expression>> &source) {
    return OwnedExpressionMap::transformValues(source, [](const auto &value) {
        return utils::shareOwned(value ? value->clone() : nullptr);
    });
}
} // namespace graph::query::expressions
