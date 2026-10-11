/**
 * @file NativeConfigurationService.cpp
 * @brief Native Database-owned configuration policy implementation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/NativeConfigurationService.hpp"

#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace graph::query {

	namespace {

		constexpr std::string_view CONFIG_QUERY_TIMEOUT_MS = "query.timeout_ms";
		constexpr std::string_view CONFIG_QUERY_MAX_MEMORY_MB = "query.max_memory_mb";
		constexpr std::string_view CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH = "query.max_var_length_depth";
		constexpr std::string_view CONFIG_QUERY_SLOW_LOG_ENABLED = "query.slow_log.enabled";
		constexpr std::string_view CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS = "query.slow_log.threshold_ms";
		constexpr std::string_view CONFIG_STATS_ENABLED = "stats.enabled";
		constexpr std::string_view CONFIG_STORAGE_COMPACTION_ENABLED = "storage.compaction.enabled";
		constexpr std::array CONFIGURATION_KEYS = {
				CONFIG_QUERY_MAX_MEMORY_MB,
				CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH,
				CONFIG_QUERY_SLOW_LOG_ENABLED,
				CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS,
				CONFIG_QUERY_TIMEOUT_MS,
				CONFIG_STATS_ENABLED,
				CONFIG_STORAGE_COMPACTION_ENABLED,
		};
		constexpr int64_t MAXIMUM_QUERY_TIMEOUT_MS = 86'400'000;
		constexpr int64_t MAXIMUM_QUERY_MEMORY_MB = 1'048'576;
		constexpr int64_t MAXIMUM_QUERY_VAR_LENGTH_DEPTH = 1'000'000;
		constexpr int64_t MAXIMUM_SLOW_LOG_THRESHOLD_MS = 86'400'000;
		constexpr size_t MAXIMUM_SLOW_QUERY_RECORDS = 256U;
		constexpr uint64_t MAXIMUM_REPRESENTABLE_STATISTIC =
				static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

		[[nodiscard]] int64_t requireBoundedInteger(const PropertyValue &value, const std::string_view key,
				int64_t maximum) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
			}
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			if (parsed < 0 || parsed > maximum) {
				throw std::invalid_argument(std::string(key) + " is outside the supported configuration range");
			}
			return parsed;
		}

		void saturatingAdd(std::atomic<uint64_t> &target, const uint64_t amount) noexcept {
			uint64_t observed = target.load(std::memory_order_relaxed);
			while (observed < MAXIMUM_REPRESENTABLE_STATISTIC) {
				const uint64_t remaining = MAXIMUM_REPRESENTABLE_STATISTIC - observed;
				const uint64_t desired = amount >= remaining ? MAXIMUM_REPRESENTABLE_STATISTIC : observed + amount;
				if (target.compare_exchange_weak(observed, desired, std::memory_order_relaxed,
						std::memory_order_relaxed)) {
					return;
				}
			}
		}

	} // namespace

	NativeConfigurationService::NativeConfigurationService(
			std::shared_ptr<storage::LogicalGraphAdministrationBackend> backend) : backend_(std::move(backend)) {
		if (!backend_) {
			throw std::invalid_argument("native configuration service requires an administration backend");
		}
		maintenance_controller_ = std::dynamic_pointer_cast<storage::LogicalGraphMaintenanceController>(backend_);
		for (const std::string_view key: CONFIGURATION_KEYS) {
			if (!supports(key)) {
				continue;
			}
			if (key == CONFIG_STATS_ENABLED) {
				requireNoPersistedRuntimeConfiguration(key);
				continue;
			}
			if (const std::optional<PropertyValue> value = backend_->findConfiguration(key); value.has_value()) {
				validate(key, *value);
				if (key == CONFIG_STORAGE_COMPACTION_ENABLED) {
					maintenance_controller_->setNativeMaintenanceEnabled(std::get<bool>(value->getVariant()));
				} else {
					apply(key, *value);
				}
			}
		}
	}

	std::vector<storage::LogicalGraphConfiguration> NativeConfigurationService::list() const {
		std::vector<storage::LogicalGraphConfiguration> result;
		result.reserve(CONFIGURATION_KEYS.size());
		for (const std::string_view key: CONFIGURATION_KEYS) {
			if (!supports(key)) {
				continue;
			}
			result.push_back({std::string(key), get(key).value});
		}
		return result;
	}

	storage::LogicalGraphConfiguration NativeConfigurationService::get(const std::string_view key) const {
		if (!supports(key)) {
			throw std::invalid_argument("native configuration service does not support this configuration key");
		}
		const PropertyValue fallback = defaultValue(key);
		if (key == CONFIG_STATS_ENABLED) {
			requireNoPersistedRuntimeConfiguration(key);
			return {std::string(key), PropertyValue(queryRuntimeStatisticsEnabled())};
		}
		const std::optional<PropertyValue> persisted = backend_->findConfiguration(key);
		if (!persisted.has_value()) {
			return {std::string(key), fallback};
		}
		validate(key, *persisted);
		return {std::string(key), *persisted};
	}

	void NativeConfigurationService::set(const std::string_view key, const PropertyValue &value) {
		validate(key, value);
		if (key == CONFIG_STATS_ENABLED) {
			requireNoPersistedRuntimeConfiguration(key);
			query_runtime_statistics_enabled_.store(std::get<bool>(value.getVariant()), std::memory_order_release);
			return;
		}
		if (key == CONFIG_STORAGE_COMPACTION_ENABLED) {
			const bool requestedEnabled = std::get<bool>(value.getVariant());
			const bool previousEnabled = std::get<bool>(get(key).value.getVariant());
			if (requestedEnabled) {
				// Never run maintenance work that the durable state has not first
				// authorized. A resume failure leaves a safe paused service behind;
				// reopen retries from the published configuration.
				backend_->setConfiguration(key, value);
				try {
					maintenance_controller_->setNativeMaintenanceEnabled(true);
				} catch (...) {
					throw std::runtime_error(
							"native maintenance enablement was durably published but could not be started; "
							"close and reopen the database");
				}
				return;
			}
			// Disabling pauses before publication so no new maintenance mutation can
			// race after the caller receives a successful durable disable result.
			maintenance_controller_->setNativeMaintenanceEnabled(false);
			try {
				backend_->setConfiguration(key, value);
			} catch (...) {
				try {
					maintenance_controller_->setNativeMaintenanceEnabled(previousEnabled);
				} catch (...) {
					throw std::runtime_error(
							"native maintenance lifecycle could not be restored after configuration publication failed; "
							"close and reopen the database");
				}
				throw;
			}
			return;
		}
		std::unique_lock lock(policy_mutex_);
		// Keep durable publication and its in-memory owner in one writer order.
		// Otherwise two concurrent updates can persist the newer value while the
		// earlier writer applies its stale policy last.
		backend_->setConfiguration(key, value);
		apply(key, value);
	}

	NativeQueryResourcePolicy NativeConfigurationService::queryResourcePolicy() const {
		std::shared_lock lock(policy_mutex_);
		return policy_;
	}

	NativeSlowQueryPolicy NativeConfigurationService::slowQueryPolicy() const {
		std::shared_lock lock(policy_mutex_);
		return slow_query_policy_;
	}

	bool NativeConfigurationService::queryRuntimeStatisticsEnabled() const noexcept {
		return query_runtime_statistics_enabled_.load(std::memory_order_acquire);
	}

	void NativeConfigurationService::recordQueryExecution(const uint64_t elapsedMs, const bool succeeded) noexcept {
		if (queryRuntimeStatisticsEnabled()) {
			saturatingAdd(completed_queries_, 1U);
			saturatingAdd(succeeded ? succeeded_queries_ : failed_queries_, 1U);
			saturatingAdd(aggregate_elapsed_ms_, elapsedMs);
		}
	}

	void NativeConfigurationService::recordSlowQuery(const std::string_view logicalPlanType, const uint64_t elapsedMs,
			const bool succeeded) noexcept {
		try {
			const NativeSlowQueryPolicy policy = slowQueryPolicy();
			if (!policy.enabled || elapsedMs < static_cast<uint64_t>(policy.threshold_ms) || logicalPlanType.empty()) {
				return;
			}
			std::lock_guard lock(slow_query_mutex_);
			if (next_slow_query_sequence_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
				elapsedMs > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				return;
			}
			slow_queries_.push_back({next_slow_query_sequence_++, std::string(logicalPlanType), elapsedMs, succeeded});
			if (slow_queries_.size() > MAXIMUM_SLOW_QUERY_RECORDS) {
				slow_queries_.pop_front();
			}
		} catch (...) {
			// Observability is strictly non-intrusive: allocation pressure or an
			// unexpected diagnostic failure must not change query correctness.
		}
	}

	std::vector<NativeSlowQueryRecord> NativeConfigurationService::slowQueries() const {
		std::lock_guard lock(slow_query_mutex_);
		return {slow_queries_.begin(), slow_queries_.end()};
	}

	void NativeConfigurationService::resetSlowQueries() noexcept {
		try {
			std::lock_guard lock(slow_query_mutex_);
			slow_queries_.clear();
		} catch (...) {
			// std::mutex locking is not expected to throw, but reset remains a
			// best-effort observability operation if a platform implementation does.
		}
	}

	std::vector<storage::LogicalGraphRuntimeStatistic> NativeConfigurationService::statistics() const {
		std::vector<storage::LogicalGraphRuntimeStatistic> result = backend_->runtimeStatistics();
		if (!queryRuntimeStatisticsEnabled()) {
			return result;
		}
		const NativeQueryRuntimeStatistics queryStatistics = queryRuntimeStatistics();
		result.push_back({"query", "completed", PropertyValue(static_cast<int64_t>(queryStatistics.completed_queries))});
		result.push_back({"query", "succeeded", PropertyValue(static_cast<int64_t>(queryStatistics.succeeded_queries))});
		result.push_back({"query", "failed", PropertyValue(static_cast<int64_t>(queryStatistics.failed_queries))});
		result.push_back(
				{"query", "aggregate_elapsed_ms", PropertyValue(static_cast<int64_t>(queryStatistics.aggregate_elapsed_ms))});
		return result;
	}

	void NativeConfigurationService::resetStatistics() {
		backend_->resetRuntimeStatistics();
		completed_queries_.store(0U, std::memory_order_release);
		succeeded_queries_.store(0U, std::memory_order_release);
		failed_queries_.store(0U, std::memory_order_release);
		aggregate_elapsed_ms_.store(0U, std::memory_order_release);
	}

	bool NativeConfigurationService::supports(const std::string_view key) const noexcept {
		if (key == CONFIG_STORAGE_COMPACTION_ENABLED) {
			return maintenance_controller_ != nullptr && maintenance_controller_->nativeMaintenanceAvailable();
		}
		return key == CONFIG_QUERY_TIMEOUT_MS || key == CONFIG_QUERY_MAX_MEMORY_MB ||
			   key == CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH || key == CONFIG_QUERY_SLOW_LOG_ENABLED ||
			   key == CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS || key == CONFIG_STATS_ENABLED;
	}

	void NativeConfigurationService::validate(const std::string_view key, const PropertyValue &value) const {
		if (!supports(key)) {
			throw std::invalid_argument("native configuration service does not support this configuration key");
		}
		if (key == CONFIG_QUERY_TIMEOUT_MS) {
			static_cast<void>(requireBoundedInteger(value, key, MAXIMUM_QUERY_TIMEOUT_MS));
			return;
		}
		if (key == CONFIG_QUERY_MAX_MEMORY_MB) {
			static_cast<void>(requireBoundedInteger(value, key, MAXIMUM_QUERY_MEMORY_MB));
			return;
		}
		if (key == CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH) {
			static_cast<void>(requireBoundedInteger(value, key, MAXIMUM_QUERY_VAR_LENGTH_DEPTH));
			return;
		}
		if (key == CONFIG_QUERY_SLOW_LOG_ENABLED) {
			if (value.getType() != PropertyType::BOOLEAN) {
				throw std::invalid_argument(std::string(key) + " must be a Boolean");
			}
			return;
		}
		if (key == CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS) {
			static_cast<void>(requireBoundedInteger(value, key, MAXIMUM_SLOW_LOG_THRESHOLD_MS));
			return;
		}
		if (key == CONFIG_STATS_ENABLED) {
			if (value.getType() != PropertyType::BOOLEAN) {
				throw std::invalid_argument(std::string(key) + " must be a Boolean");
			}
			return;
		}
		if (key == CONFIG_STORAGE_COMPACTION_ENABLED) {
			if (value.getType() != PropertyType::BOOLEAN) {
				throw std::invalid_argument(std::string(key) + " must be a Boolean");
			}
			return;
		}
		throw std::logic_error("native configuration service support and validation disagreed");
	}

	PropertyValue NativeConfigurationService::defaultValue(const std::string_view key) {
		if (key == CONFIG_QUERY_TIMEOUT_MS) {
			return PropertyValue(int64_t{30'000});
		}
		if (key == CONFIG_QUERY_MAX_MEMORY_MB) {
			return PropertyValue(int64_t{0});
		}
		if (key == CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH) {
			return PropertyValue(int64_t{50});
		}
		if (key == CONFIG_QUERY_SLOW_LOG_ENABLED) {
			return PropertyValue(false);
		}
		if (key == CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS) {
			return PropertyValue(int64_t{1'000});
		}
		if (key == CONFIG_STATS_ENABLED) {
			return PropertyValue(true);
		}
		if (key == CONFIG_STORAGE_COMPACTION_ENABLED) {
			return PropertyValue(true);
		}
		throw std::invalid_argument("native configuration service does not support this configuration key");
	}

	void NativeConfigurationService::apply(const std::string_view key, const PropertyValue &value) {
		if (key == CONFIG_QUERY_TIMEOUT_MS) {
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			policy_.timeout_ms = parsed;
			return;
		}
		if (key == CONFIG_QUERY_MAX_MEMORY_MB) {
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			policy_.maximum_memory_mb = parsed;
			return;
		}
		if (key == CONFIG_QUERY_MAX_VAR_LENGTH_DEPTH) {
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			policy_.maximum_var_length_depth = parsed;
			return;
		}
		if (key == CONFIG_QUERY_SLOW_LOG_ENABLED) {
			slow_query_policy_.enabled = std::get<bool>(value.getVariant());
			return;
		}
		if (key == CONFIG_QUERY_SLOW_LOG_THRESHOLD_MS) {
			slow_query_policy_.threshold_ms = std::get<int64_t>(value.getVariant());
			return;
		}
		throw std::logic_error("validated native configuration key has no runtime policy owner");
	}

	void NativeConfigurationService::requireNoPersistedRuntimeConfiguration(const std::string_view key) const {
		if (backend_->findConfiguration(key).has_value()) {
			throw std::invalid_argument(std::string(key) +
					" is process-local runtime configuration and must not be persisted in the database");
		}
	}

	NativeQueryRuntimeStatistics NativeConfigurationService::queryRuntimeStatistics() const noexcept {
		return {
				completed_queries_.load(std::memory_order_relaxed),
				succeeded_queries_.load(std::memory_order_relaxed),
				failed_queries_.load(std::memory_order_relaxed),
				aggregate_elapsed_ms_.load(std::memory_order_relaxed),
		};
	}

} // namespace graph::query
