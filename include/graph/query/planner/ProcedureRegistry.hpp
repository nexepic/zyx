/**
 * @file ProcedureRegistry.hpp
 * @brief Native logical procedure metadata registry.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

namespace graph::query::planner {

	// Parsing needs mutation metadata before the logical runtime dispatches a
	// procedure. Physical-operator factories deliberately do not cross this
	// boundary: native execution owns its dispatch in LogicalPlanProcedureRuntime.
	struct ProcedureDescriptor {
		bool mutatesData = false;
		bool mutatesSchema = false;
	};

	class ProcedureRegistry {
	public:
		ProcedureRegistry();
		~ProcedureRegistry() = default;

		[[nodiscard]] static ProcedureRegistry &instance() {
			static ProcedureRegistry instance;
			return instance;
		}

		void registerProcedure(std::string name, ProcedureDescriptor descriptor);

		[[nodiscard]] const ProcedureDescriptor *getDescriptor(const std::string &name) const;
		[[nodiscard]] std::vector<std::string> getRegisteredNames() const;

	private:
		std::map<std::string, ProcedureDescriptor, std::less<>> registry_;
	};

} // namespace graph::query::planner
