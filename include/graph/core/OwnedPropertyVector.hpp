/**
 * @file OwnedPropertyVector.hpp
 * @brief Pre-admitted immutable property-value vectors.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/utils/OwnedCopyVector.hpp"
namespace graph {
struct PropertyVectorCopyPolicy {
    static size_t copyBytes(const PropertyValue &value) { return property_heap::copyRequestUpper(value); }
};
using OwnedPropertyVector = utils::OwnedCopyVector<PropertyValue, PropertyVectorCopyPolicy>;
} // namespace graph
