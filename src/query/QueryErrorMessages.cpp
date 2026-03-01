/**
 * @file QueryErrorMessages.cpp
 * @author Metrix Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include "graph/query/QueryErrorMessages.hpp"

namespace graph::query {

std::string QueryErrorMessages::mustFollowClause(const std::string &clauseName,
                                                   const std::string &validPreceding) {
	return clauseName + " clause must follow a " + validPreceding + " clause.";
}

std::string QueryErrorMessages::requiresLiteral(const std::string &clause,
                                                 const std::string &expected) {
	return clause + " requires a literal " + expected + ".";
}

std::string QueryErrorMessages::invalidContext(const std::string &operation,
                                               const std::string &requirement) {
	return "Cannot " + operation + " in this context. " + requirement + ".";
}

std::string QueryErrorMessages::undefinedVariable(const std::string &varName) {
	return "Undefined variable: " + varName;
}

std::string QueryErrorMessages::invalidSyntax(const std::string &context,
                                               const std::string &details) {
	return "Invalid syntax in " + context + ": " + details;
}

} // namespace graph::query
