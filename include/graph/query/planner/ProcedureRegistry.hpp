/**
 * @file ProcedureRegistry.hpp
 * @author Nexepic
 * @date 2025/12/30
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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace graph::storage {
	class DataManager;
}
namespace graph::query::indexes {
	class IndexManager;
}
namespace graph::query::execution {
	class PhysicalOperator;
}
namespace graph {
	struct PropertyValue;
}

namespace graph::query::planner {

	struct ProcedureContext {
		std::shared_ptr<storage::DataManager> dataManager;
		std::shared_ptr<indexes::IndexManager> indexManager;
		uint64_t planCacheHits = 0;
		uint64_t planCacheMisses = 0;
	};

	using ProcedureFactory = std::function<std::unique_ptr<execution::PhysicalOperator>(
			const ProcedureContext &ctx, const std::vector<PropertyValue> &args)>;

	class ProcedureRegistry {
	public:
		// The implementation in .cpp registers all built-in procedures here.
		ProcedureRegistry();
		~ProcedureRegistry() = default;

		// Singleton access (Optional, keep if your Planner uses it)
		static ProcedureRegistry &instance() {
			static ProcedureRegistry instance;
			return instance;
		}

		void registerProcedure(const std::string &name, const ProcedureFactory &factory) {
			registry_[name] = factory;
		}

		ProcedureFactory get(const std::string &name) {
			if (const auto it = registry_.find(name); it != registry_.end())
				return it->second;
			return nullptr;
		}

	private:
		std::map<std::string, ProcedureFactory> registry_;
	};

} // namespace graph::query::planner