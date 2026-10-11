#pragma once
#include "graph/core/GuardedPropertyCopy.hpp"

namespace graph::query::execution::detail {
using graph::property_copy::copyPropertyStringGuarded;
using graph::property_copy::copyPropertyStringsGuarded;
using graph::property_copy::copyPropertyValueImpl;
using graph::property_copy::copyPropertyValueGuarded;
using graph::property_copy::copyPropertyMapGuarded;
}
