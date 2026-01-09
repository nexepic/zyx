/**
 * @file IQueryParser.hpp
 * @author Nexepic
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#pragma once

#include <memory>
#include <string>
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::parser {

	/**
	 * @interface IQueryParser
	 * @brief Interface that all DSL parsers (Cypher, etc.) must implement.
	 */
	class IQueryParser {
	public:
		virtual ~IQueryParser() = default;

		/**
		 * @brief Parses a raw query string into an executable Operator Tree.
		 *
		 * @param query The source query string.
		 * @return std::unique_ptr<graph::query::execution::PhysicalOperator> The root of the execution tree.
		 */
		[[nodiscard]] virtual std::unique_ptr<query::execution::PhysicalOperator>
		parse(const std::string &query) const = 0;
	};

} // namespace graph::parser
