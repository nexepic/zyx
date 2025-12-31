/**
 * @file ProcedureRegistry.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
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

	// Context object holding all dependencies a procedure might need
	struct ProcedureContext {
		std::shared_ptr<storage::DataManager> dataManager;
		std::shared_ptr<indexes::IndexManager> indexManager;
	};

	// Factory accepts Context instead of just DataManager
	using ProcedureFactory = std::function<std::unique_ptr<execution::PhysicalOperator>(
			const ProcedureContext &ctx, const std::vector<PropertyValue> &args)>;

	class ProcedureRegistry {
	public:
		static ProcedureRegistry &instance() {
			static ProcedureRegistry instance;
			return instance;
		}

		void registerProcedure(const std::string &name, const ProcedureFactory &factory) { registry_[name] = factory; }

		ProcedureFactory get(const std::string &name) {
			if (const auto it = registry_.find(name); it != registry_.end())
				return it->second;
			return nullptr;
		}

	private:
		std::map<std::string, ProcedureFactory> registry_;
		ProcedureRegistry();
	};

} // namespace graph::query::planner
