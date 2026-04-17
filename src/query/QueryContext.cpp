/**
 * @file QueryContext.cpp
 * @brief Implementation of QueryContext methods.
 **/

#include "graph/query/QueryContext.hpp"
#include "graph/query/QueryGuard.hpp"

namespace graph::query {

void QueryContext::checkGuard() const {
	if (guard) guard->check();
}

} // namespace graph::query
