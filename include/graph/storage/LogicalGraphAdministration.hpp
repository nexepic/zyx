/**
 * @file LogicalGraphAdministration.hpp
 * @brief Storage-neutral durable configuration and runtime observability boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace graph::storage {

	struct LogicalGraphConfiguration {
		std::string key;
		PropertyValue value;

		bool operator==(const LogicalGraphConfiguration &) const = default;
	};

	struct LogicalGraphRuntimeStatistic {
		std::string category;
		std::string metric;
		PropertyValue value;

		bool operator==(const LogicalGraphRuntimeStatistic &) const = default;
	};

	// Durable configuration belongs to the database file. Runtime statistics are
	// intentionally process-local, even when both are exposed through DBMS
	// procedures. Keeping them together at this logical boundary avoids a native
	// query runtime dependency on a particular physical storage implementation.
	class LogicalGraphAdministrationBackend {
	public:
		virtual ~LogicalGraphAdministrationBackend() = default;

		[[nodiscard]] virtual std::optional<PropertyValue> findConfiguration(std::string_view key) const = 0;
		[[nodiscard]] virtual std::vector<LogicalGraphConfiguration>
		readConfigurations(size_t maximumRecords) const = 0;
		virtual void setConfiguration(std::string_view key, const PropertyValue &value) = 0;

		[[nodiscard]] virtual std::vector<LogicalGraphRuntimeStatistic> runtimeStatistics() const = 0;
		virtual void resetRuntimeStatistics() = 0;
	};

	// Maintenance work remains an opt-in embedding capability, but when a native
	// backend provides it, this boundary gives Database-owned policy code one
	// storage-neutral lifecycle owner. It deliberately does not expose physical
	// task queues or page identities to the query layer.
	class LogicalGraphMaintenanceController {
	public:
		virtual ~LogicalGraphMaintenanceController() = default;

		[[nodiscard]] virtual bool nativeMaintenanceAvailable() const noexcept = 0;
		[[nodiscard]] virtual bool nativeMaintenanceEnabled() const = 0;
		virtual void setNativeMaintenanceEnabled(bool enabled) = 0;
	};

} // namespace graph::storage
