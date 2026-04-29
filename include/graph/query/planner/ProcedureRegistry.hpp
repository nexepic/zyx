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
namespace graph::query::algorithm {
	class GraphProjectionManager;
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
		std::shared_ptr<algorithm::GraphProjectionManager> projectionManager;
		uint64_t planCacheHits = 0;
		uint64_t planCacheMisses = 0;
	};

	using ProcedureFactory = std::function<std::unique_ptr<execution::PhysicalOperator>(
			const ProcedureContext &ctx, const std::vector<PropertyValue> &args)>;

	/**
	 * @brief Describes a registered procedure's factory and mutation characteristics.
	 */
	struct ProcedureDescriptor {
		ProcedureFactory factory;
		bool mutatesData = false;
		bool mutatesSchema = false;
	};

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

		void registerProcedure(const std::string &name, const ProcedureFactory &factory,
		                       bool mutatesData = false, bool mutatesSchema = false) {
			registry_[name] = {factory, mutatesData, mutatesSchema};
		}

		ProcedureFactory get(const std::string &name) {
			if (const auto it = registry_.find(name); it != registry_.end())
				return it->second.factory;
			return nullptr;
		}

		/**
		 * @brief Returns all registered procedure names.
		 */
		std::vector<std::string> getRegisteredNames() const {
			std::vector<std::string> names;
			names.reserve(registry_.size());
			for (const auto& [name, _] : registry_) {
				names.push_back(name);
			}
			return names;
		}

		/**
		 * @brief Returns the descriptor for a procedure, or nullptr if not found.
		 */
		const ProcedureDescriptor *getDescriptor(const std::string &name) const {
			if (const auto it = registry_.find(name); it != registry_.end())
				return &it->second;
			return nullptr;
		}

	private:
		std::map<std::string, ProcedureDescriptor> registry_;
	};

} // namespace graph::query::planner