#pragma once
#include "graph/core/PropertyTypes.hpp"

namespace graph::query::execution::detail {

// ORDER BY has a different NULL policy from the generic PropertyValue order:
// NULL is last ascending and first descending. Equal keys remain stable at
// the caller's row/sequence comparison boundary.
[[nodiscard]] inline bool orderByPrecedes(const PropertyValue &left, const PropertyValue &right,
                                         const bool ascending) {
 const bool leftNull=left.getType()==PropertyType::NULL_TYPE;
 const bool rightNull=right.getType()==PropertyType::NULL_TYPE;
 if(leftNull || rightNull) {
  if(leftNull==rightNull)return false;
  return ascending ? !leftNull : leftNull;
 }
 return ascending ? left<right : left>right;
}

} // namespace graph::query::execution::detail
