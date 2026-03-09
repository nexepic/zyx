/**
 * @file PredicateFunction.cpp
 * @date 2025/3/9
 *
 * Implementation of PredicateFunction
 */

#include "graph/query/expressions/PredicateFunction.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include <stdexcept>
#include <sstream>

namespace graph::query::expressions {

PropertyValue PredicateFunction::evaluate(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) const {
	// Quantifier functions have special Cypher syntax that requires custom parsing
	// This method is called when the function is invoked through the standard
	// ScalarFunction interface, which doesn't support the special syntax.

	std::ostringstream oss;
	oss << getName() << "() function has special Cypher syntax that requires "
	    << "custom argument parsing. The syntax is:\n"
	    << "  " << getName() << "(variable IN list WHERE condition)\n"
	    << "This function cannot be called through the standard function interface. "
	    << "Please use it in a WHERE clause with the proper syntax.";

	throw std::runtime_error(oss.str());
}

} // namespace graph::query::expressions
