/**
 * @file ProcedureRegistry.cpp
 * @brief Native logical procedure metadata registry.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/planner/ProcedureRegistry.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "graph/query/NativeProcedureManifest.hpp"

namespace graph::query::planner {

	ProcedureRegistry::ProcedureRegistry() {
		for (const NativeProcedureManifestEntry &entry: nativeProcedureManifest()) {
			registerProcedure(std::string(entry.name), {.mutatesData = entry.mutates_data,
															 .mutatesSchema = entry.mutates_schema});
		}
	}

	void ProcedureRegistry::registerProcedure(std::string name, ProcedureDescriptor descriptor) {
		if (name.empty()) {
			throw std::invalid_argument("logical procedure name cannot be empty");
		}
		const auto [_, inserted] = registry_.emplace(std::move(name), descriptor);
		if (!inserted) {
			throw std::logic_error("logical procedure metadata was registered more than once");
		}
	}

	const ProcedureDescriptor *ProcedureRegistry::getDescriptor(const std::string &name) const {
		const auto found = registry_.find(name);
		return found == registry_.end() ? nullptr : &found->second;
	}

	std::vector<std::string> ProcedureRegistry::getRegisteredNames() const {
		std::vector<std::string> names;
		names.reserve(registry_.size());
		for (const auto &[name, _]: registry_) {
			names.push_back(name);
		}
		return names;
	}

} // namespace graph::query::planner
