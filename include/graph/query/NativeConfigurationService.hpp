/**
 * @file NativeConfigurationService.hpp
 * @brief Database-owned policy for native durable configuration and observability.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/LogicalGraphAdministration.hpp"

namespace graph::query {

	struct NativeQueryResourcePolicy {
		int64_t timeout_ms = 30'000;
		int64_t maximum_memory_mb = 0;
		int64_t maximum_var_length_depth = 50;

		constexpr bool operator==(const NativeQueryResourcePolicy &) const = default;
	};

	// Slow-query records intentionally contain only a logical-plan category. The
	// database must not retain Cypher text, literals, or bound parameter values
	// in process-local diagnostics by default.
	struct NativeSlowQueryRecord {
		uint64_t sequence = 0U;
		std::string logical_plan_type;
		uint64_t elapsed_ms = 0U;
		bool succeeded = false;

		bool operator==(const NativeSlowQueryRecord &) const = default;
	};

	struct NativeSlowQueryPolicy {
		bool enabled = false;
		int64_t threshold_ms = 1'000;

		constexpr bool operator==(const NativeSlowQueryPolicy &) const = default;
	};

	// Query-runtime metrics are deliberately instance-local. Storage statistics
	// retain their backend-defined lifetime and remain available independently.
	struct NativeQueryRuntimeStatistics {
		uint64_t completed_queries = 0U;
		uint64_t succeeded_queries = 0U;
		uint64_t failed_queries = 0U;
		uint64_t aggregate_elapsed_ms = 0U;

		constexpr bool operator==(const NativeQueryRuntimeStatistics &) const = default;
	};

	// The service is owned by one QueryEngine/Database. It is the sole native
	// policy authority: the storage boundary persists already-validated records,
	// while this layer decides which records are supported and which affect query
	// execution. It deliberately exposes no process-global configuration.
	class NativeConfigurationService final {
	public:
		explicit NativeConfigurationService(std::shared_ptr<storage::LogicalGraphAdministrationBackend> backend);

		[[nodiscard]] std::vector<storage::LogicalGraphConfiguration> list() const;
		[[nodiscard]] storage::LogicalGraphConfiguration get(std::string_view key) const;
		void set(std::string_view key, const PropertyValue &value);
		[[nodiscard]] NativeQueryResourcePolicy queryResourcePolicy() const;
		[[nodiscard]] NativeSlowQueryPolicy slowQueryPolicy() const;
		[[nodiscard]] bool queryRuntimeStatisticsEnabled() const noexcept;
		// This is intentionally noexcept and lock-free on the hot query-finalization
		// path. The process-local stats control does not affect slow-query records.
		void recordQueryExecution(uint64_t elapsedMs, bool succeeded) noexcept;
		void recordSlowQuery(std::string_view logicalPlanType, uint64_t elapsedMs, bool succeeded) noexcept;
		[[nodiscard]] std::vector<NativeSlowQueryRecord> slowQueries() const;
		void resetSlowQueries() noexcept;
		[[nodiscard]] std::vector<storage::LogicalGraphRuntimeStatistic> statistics() const;
		void resetStatistics();

	private:
		std::shared_ptr<storage::LogicalGraphAdministrationBackend> backend_;
		std::shared_ptr<storage::LogicalGraphMaintenanceController> maintenance_controller_;
		mutable std::shared_mutex policy_mutex_;
		NativeQueryResourcePolicy policy_;
		NativeSlowQueryPolicy slow_query_policy_;
		std::atomic<bool> query_runtime_statistics_enabled_{true};
		std::atomic<uint64_t> completed_queries_{0U};
		std::atomic<uint64_t> succeeded_queries_{0U};
		std::atomic<uint64_t> failed_queries_{0U};
		std::atomic<uint64_t> aggregate_elapsed_ms_{0U};
		mutable std::mutex slow_query_mutex_;
		std::deque<NativeSlowQueryRecord> slow_queries_;
		uint64_t next_slow_query_sequence_ = 1U;

		[[nodiscard]] bool supports(std::string_view key) const noexcept;
		void validate(std::string_view key, const PropertyValue &value) const;
		static PropertyValue defaultValue(std::string_view key);
		void apply(std::string_view key, const PropertyValue &value);
		void requireNoPersistedRuntimeConfiguration(std::string_view key) const;
		[[nodiscard]] NativeQueryRuntimeStatistics queryRuntimeStatistics() const noexcept;
	};

} // namespace graph::query
