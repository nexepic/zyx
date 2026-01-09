/**
 * @file Transaction.hpp
 * @author Nexepic
 * @date 2025/2/26
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
#include <unordered_map>
#include "Edge.hpp"
#include "Node.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph {

	class Database;

	class Transaction {
	public:
		explicit Transaction(const Database &db);

		[[nodiscard]] Node insertNode(const std::string &label) const;
		[[nodiscard]] Edge insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const;

		void commit() const;
		static void rollback();

	private:
		std::shared_ptr<storage::FileStorage> storage;
	};

} // namespace graph
