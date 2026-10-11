/**
 * @file test_NativeConfigurationService.cpp
 * @brief Unit tests for Database-owned native configuration policy.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <map>
#include <mutex>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "graph/query/NativeConfigurationService.hpp"

namespace {

	using graph::PropertyValue;
	using graph::query::NativeConfigurationService;
	using graph::storage::LogicalGraphAdministrationBackend;
	using graph::storage::LogicalGraphConfiguration;
	using graph::storage::LogicalGraphRuntimeStatistic;

	class InMemoryAdministrationBackend final : public LogicalGraphAdministrationBackend,
										 public graph::storage::LogicalGraphMaintenanceController {
	public:
		[[nodiscard]] std::optional<PropertyValue> findConfiguration(const std::string_view key) const override {
			std::lock_guard lock(mutex_);
			const auto found = configurations_.find(std::string(key));
			if (found == configurations_.end()) {
				return std::nullopt;
			}
			return found->second;
		}

		[[nodiscard]] std::vector<LogicalGraphConfiguration> readConfigurations(const size_t maximumRecords) const override {
			std::lock_guard lock(mutex_);
			std::vector<LogicalGraphConfiguration> result;
			result.reserve(std::min(maximumRecords, configurations_.size()));
			for (const auto &[key, value]: configurations_) {
				if (result.size() == maximumRecords) {
					break;
				}
				result.push_back({key, value});
			}
			return result;
		}

		void setConfiguration(const std::string_view key, const PropertyValue &value) override {
			std::function<void(std::string_view, const PropertyValue &)> afterWrite;
			{
				std::lock_guard lock(mutex_);
				++write_attempts_;
				if (reject_writes_) {
					throw std::runtime_error("in-memory administration backend rejected write");
				}
				configurations_.insert_or_assign(std::string(key), value);
				afterWrite = after_write_;
			}
			if (afterWrite) {
				afterWrite(key, value);
			}
		}

		[[nodiscard]] std::vector<LogicalGraphRuntimeStatistic> runtimeStatistics() const override {
			std::lock_guard lock(mutex_);
			return statistics_;
		}

		void resetRuntimeStatistics() override {
			std::lock_guard lock(mutex_);
			statistics_.clear();
			++reset_calls_;
		}

		[[nodiscard]] bool nativeMaintenanceAvailable() const noexcept override {
			return maintenance_available_.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool nativeMaintenanceEnabled() const override {
			return maintenance_enabled_.load(std::memory_order_acquire);
		}

		void setNativeMaintenanceEnabled(const bool enabled) override {
			if (!nativeMaintenanceAvailable()) {
				throw std::logic_error("in-memory maintenance control is unavailable");
			}
			if (enabled && reject_maintenance_enable_.load(std::memory_order_acquire)) {
				throw std::runtime_error("in-memory maintenance control rejected enablement");
			}
			maintenance_enabled_.store(enabled, std::memory_order_release);
		}

		void setPersisted(std::string key, PropertyValue value) {
			std::lock_guard lock(mutex_);
			configurations_.insert_or_assign(std::move(key), std::move(value));
		}

		void setRejectWrites(const bool reject) {
			std::lock_guard lock(mutex_);
			reject_writes_ = reject;
		}

		void setAfterWrite(std::function<void(std::string_view, const PropertyValue &)> callback) {
			std::lock_guard lock(mutex_);
			after_write_ = std::move(callback);
		}

		void setStatistics(std::vector<LogicalGraphRuntimeStatistic> statistics) {
			std::lock_guard lock(mutex_);
			statistics_ = std::move(statistics);
		}

		void setMaintenanceAvailable(const bool available) {
			maintenance_available_.store(available, std::memory_order_release);
			maintenance_enabled_.store(available, std::memory_order_release);
		}

		void setRejectMaintenanceEnable(const bool reject) {
			reject_maintenance_enable_.store(reject, std::memory_order_release);
		}

		[[nodiscard]] size_t writeAttempts() const {
			std::lock_guard lock(mutex_);
			return write_attempts_;
		}

		[[nodiscard]] size_t resetCalls() const {
			std::lock_guard lock(mutex_);
			return reset_calls_;
		}

	private:
		mutable std::mutex mutex_;
		std::map<std::string, PropertyValue, std::less<>> configurations_;
		std::vector<LogicalGraphRuntimeStatistic> statistics_;
		size_t write_attempts_ = 0U;
		size_t reset_calls_ = 0U;
		bool reject_writes_ = false;
		std::function<void(std::string_view, const PropertyValue &)> after_write_;
		std::atomic<bool> maintenance_available_{false};
		std::atomic<bool> maintenance_enabled_{false};
		std::atomic<bool> reject_maintenance_enable_{false};
	};

} // namespace

TEST(NativeConfigurationServiceTest, LoadsDefaultsPersistsValidatedPolicyAndDelegatesStatistics) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setPersisted("query.timeout_ms", PropertyValue(int64_t{17}));
	backend->setStatistics({{"io", "read_calls", PropertyValue(int64_t{3})}});
	NativeConfigurationService service(backend);

	EXPECT_EQ(service.queryResourcePolicy(), (graph::query::NativeQueryResourcePolicy{17, 0, 50}));
	EXPECT_EQ(service.get("query.max_memory_mb"),
			  (LogicalGraphConfiguration{"query.max_memory_mb", PropertyValue(int64_t{0})}));
	EXPECT_EQ(service.list(),
			  (std::vector<LogicalGraphConfiguration>{{"query.max_memory_mb", PropertyValue(int64_t{0})},
														 {"query.max_var_length_depth", PropertyValue(int64_t{50})},
														 {"query.slow_log.enabled", PropertyValue(false)},
														 {"query.slow_log.threshold_ms", PropertyValue(int64_t{1'000})},
														 {"query.timeout_ms", PropertyValue(int64_t{17})},
														 {"stats.enabled", PropertyValue(true)}}));

	service.set("query.max_memory_mb", PropertyValue(int64_t{512}));
	service.set("query.max_var_length_depth", PropertyValue(int64_t{3}));
	EXPECT_EQ(service.queryResourcePolicy(), (graph::query::NativeQueryResourcePolicy{17, 512, 3}));
	EXPECT_EQ(backend->findConfiguration("query.max_memory_mb"), PropertyValue(int64_t{512}));
	EXPECT_EQ(backend->findConfiguration("query.max_var_length_depth"), PropertyValue(int64_t{3}));
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"io", "read_calls", PropertyValue(int64_t{3})},
														 {"query", "completed", PropertyValue(int64_t{0})},
														 {"query", "succeeded", PropertyValue(int64_t{0})},
														 {"query", "failed", PropertyValue(int64_t{0})},
														 {"query", "aggregate_elapsed_ms", PropertyValue(int64_t{0})}}));
	service.resetStatistics();
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"query", "completed", PropertyValue(int64_t{0})},
														 {"query", "succeeded", PropertyValue(int64_t{0})},
														 {"query", "failed", PropertyValue(int64_t{0})},
														 {"query", "aggregate_elapsed_ms", PropertyValue(int64_t{0})}}));
	EXPECT_EQ(backend->resetCalls(), 1U);
}

TEST(NativeConfigurationServiceTest, KeepsQueryRuntimeStatisticsEphemeralAndHidesThemWithoutDiscardingCounters) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setStatistics({{"io", "read_calls", PropertyValue(int64_t{3})}});
	NativeConfigurationService service(backend);

	service.recordQueryExecution(7U, true);
	service.recordQueryExecution(11U, false);
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"io", "read_calls", PropertyValue(int64_t{3})},
														 {"query", "completed", PropertyValue(int64_t{2})},
														 {"query", "succeeded", PropertyValue(int64_t{1})},
														 {"query", "failed", PropertyValue(int64_t{1})},
														 {"query", "aggregate_elapsed_ms", PropertyValue(int64_t{18})}}));

	const size_t writesBeforeRuntimeChange = backend->writeAttempts();
	service.set("stats.enabled", PropertyValue(false));
	EXPECT_FALSE(service.queryRuntimeStatisticsEnabled());
	EXPECT_EQ(service.get("stats.enabled"), (LogicalGraphConfiguration{"stats.enabled", PropertyValue(false)}));
	EXPECT_EQ(backend->writeAttempts(), writesBeforeRuntimeChange);
	EXPECT_EQ(backend->findConfiguration("stats.enabled"), std::nullopt);
	service.recordQueryExecution(100U, true);
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"io", "read_calls", PropertyValue(int64_t{3})}}));

	service.set("stats.enabled", PropertyValue(true));
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"io", "read_calls", PropertyValue(int64_t{3})},
														 {"query", "completed", PropertyValue(int64_t{2})},
														 {"query", "succeeded", PropertyValue(int64_t{1})},
														 {"query", "failed", PropertyValue(int64_t{1})},
														 {"query", "aggregate_elapsed_ms", PropertyValue(int64_t{18})}}));

	service.resetStatistics();
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"query", "completed", PropertyValue(int64_t{0})},
														 {"query", "succeeded", PropertyValue(int64_t{0})},
														 {"query", "failed", PropertyValue(int64_t{0})},
														 {"query", "aggregate_elapsed_ms", PropertyValue(int64_t{0})}}));
	EXPECT_EQ(backend->resetCalls(), 1U);
}

TEST(NativeConfigurationServiceTest, RejectsPersistedProcessLocalStatisticsControl) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setPersisted("stats.enabled", PropertyValue(false));
	EXPECT_THROW(static_cast<void>(NativeConfigurationService(backend)), std::invalid_argument);
}

TEST(NativeConfigurationServiceTest, RejectsInvalidValuesBeforePersistingAndPreservesPolicyOnWriteFailure) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService service(backend);
	const graph::query::NativeQueryResourcePolicy defaults = service.queryResourcePolicy();

	EXPECT_THROW(static_cast<void>(service.get("unknown.key")), std::invalid_argument);
	EXPECT_THROW(service.set("unknown.key", PropertyValue(int64_t{1})), std::invalid_argument);
	EXPECT_THROW(service.set("query.timeout_ms", PropertyValue(true)), std::invalid_argument);
	EXPECT_THROW(service.set("query.timeout_ms", PropertyValue(int64_t{-1})), std::invalid_argument);
	EXPECT_THROW(service.set("query.timeout_ms", PropertyValue(int64_t{86'400'001})), std::invalid_argument);
	EXPECT_THROW(service.set("query.max_memory_mb", PropertyValue(int64_t{1'048'577})), std::invalid_argument);
	EXPECT_THROW(service.set("query.max_var_length_depth", PropertyValue(int64_t{-1})), std::invalid_argument);
	EXPECT_THROW(service.set("query.max_var_length_depth", PropertyValue(int64_t{1'000'001})), std::invalid_argument);
	EXPECT_EQ(backend->writeAttempts(), 0U);
	EXPECT_EQ(service.queryResourcePolicy(), defaults);

	backend->setRejectWrites(true);
	EXPECT_THROW(service.set("query.timeout_ms", PropertyValue(int64_t{12})), std::runtime_error);
	EXPECT_EQ(backend->writeAttempts(), 1U);
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), std::nullopt);
	EXPECT_EQ(service.queryResourcePolicy(), defaults);
}

TEST(NativeConfigurationServiceTest, FailsClosedWhenConstructionCannotEstablishAValidPolicy) {
	EXPECT_THROW(static_cast<void>(NativeConfigurationService(nullptr)), std::invalid_argument);

	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setPersisted("query.timeout_ms", PropertyValue(true));
	EXPECT_THROW(static_cast<void>(NativeConfigurationService(backend)), std::invalid_argument);
}

TEST(NativeConfigurationServiceTest, ReloadsPersistedPolicyAndSerializesConcurrentReadersWithUpdates) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setPersisted("query.timeout_ms", PropertyValue(int64_t{11}));
	backend->setPersisted("query.max_memory_mb", PropertyValue(int64_t{22}));
	NativeConfigurationService service(backend);
	EXPECT_EQ(service.queryResourcePolicy(), (graph::query::NativeQueryResourcePolicy{11, 22, 50}));

	std::atomic<bool> stopReaders = false;
	std::atomic<bool> observedInvalidPolicy = false;
	std::vector<std::thread> readers;
	for (size_t reader = 0U; reader < 4U; ++reader) {
		readers.emplace_back([&] {
			while (!stopReaders.load(std::memory_order_acquire)) {
				const graph::query::NativeQueryResourcePolicy policy = service.queryResourcePolicy();
				if (policy.timeout_ms < 0 || policy.timeout_ms > 86'400'000 || policy.maximum_memory_mb < 0 ||
					policy.maximum_memory_mb > 1'048'576) {
					observedInvalidPolicy.store(true, std::memory_order_release);
				}
			}
		});
	}
	for (int64_t value = 0; value < 256; ++value) {
		service.set("query.timeout_ms", PropertyValue(value));
		service.set("query.max_memory_mb", PropertyValue(value));
	}
	stopReaders.store(true, std::memory_order_release);
	for (std::thread &reader: readers) {
		reader.join();
	}

	EXPECT_FALSE(observedInvalidPolicy.load(std::memory_order_acquire));
	EXPECT_EQ(service.queryResourcePolicy(), (graph::query::NativeQueryResourcePolicy{255, 255, 50}));
	NativeConfigurationService reopened(backend);
	EXPECT_EQ(reopened.queryResourcePolicy(), (graph::query::NativeQueryResourcePolicy{255, 255, 50}));
}

TEST(NativeConfigurationServiceTest, SerializesDurableConfigurationWritersWithTheirPolicyApplication) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService service(backend);
	std::mutex gateMutex;
	std::condition_variable firstWriteEntered;
	bool firstWriterIsBetweenDurableAndPolicySteps = false;
	backend->setAfterWrite([&](const std::string_view key, const PropertyValue &value) {
		if (key != "query.timeout_ms" || value != PropertyValue(int64_t{1})) {
			return;
		}
		{
			std::lock_guard lock(gateMutex);
			firstWriterIsBetweenDurableAndPolicySteps = true;
		}
		firstWriteEntered.notify_one();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	});

	std::thread first([&] { service.set("query.timeout_ms", PropertyValue(int64_t{1})); });
	{
		std::unique_lock lock(gateMutex);
		firstWriteEntered.wait(lock, [&] { return firstWriterIsBetweenDurableAndPolicySteps; });
	}
	std::thread second([&] { service.set("query.timeout_ms", PropertyValue(int64_t{2})); });
	second.join();
	first.join();

	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), PropertyValue(int64_t{2}));
	EXPECT_EQ(service.queryResourcePolicy().timeout_ms, 2);
}

TEST(NativeConfigurationServiceTest, RetainsOnlyBoundedRedactedSlowQueryRecords) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService service(backend);

	service.recordSlowQuery("Project", 5U, true);
	EXPECT_TRUE(service.slowQueries().empty());
	service.set("query.slow_log.enabled", PropertyValue(true));
	service.set("query.slow_log.threshold_ms", PropertyValue(int64_t{10}));
	EXPECT_EQ(service.slowQueryPolicy(), (graph::query::NativeSlowQueryPolicy{true, 10}));
	service.recordSlowQuery("Project", 9U, true);
	service.recordSlowQuery("Traversal", 10U, false);
	ASSERT_EQ(service.slowQueries().size(), 1U);
	EXPECT_EQ(service.slowQueries().front(),
			  (graph::query::NativeSlowQueryRecord{1U, "Traversal", 10U, false}));

	service.set("query.slow_log.threshold_ms", PropertyValue(int64_t{0}));
	for (uint64_t index = 0U; index < 300U; ++index) {
		service.recordSlowQuery("CallProcedure", index, true);
	}
	const std::vector<graph::query::NativeSlowQueryRecord> records = service.slowQueries();
	ASSERT_EQ(records.size(), 256U);
	EXPECT_EQ(records.front().sequence, 46U);
	EXPECT_EQ(records.back().sequence, 301U);
	EXPECT_EQ(records.front().logical_plan_type, "CallProcedure");
	EXPECT_TRUE(records.back().succeeded);
	service.resetSlowQueries();
	EXPECT_TRUE(service.slowQueries().empty());
}

TEST(NativeConfigurationServiceTest, RejectsInvalidSlowQueryConfigurationBeforePersisting) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService service(backend);

	EXPECT_THROW(service.set("query.slow_log.enabled", PropertyValue(int64_t{1})), std::invalid_argument);
	EXPECT_THROW(service.set("query.slow_log.threshold_ms", PropertyValue(int64_t{-1})), std::invalid_argument);
	EXPECT_THROW(service.set("query.slow_log.threshold_ms", PropertyValue(int64_t{86'400'001})),
			 std::invalid_argument);
	EXPECT_THROW(service.set("stats.enabled", PropertyValue(int64_t{1})), std::invalid_argument);
	EXPECT_EQ(backend->writeAttempts(), 0U);
}

TEST(NativeConfigurationServiceTest, SaturatesRuntimeStatisticsAndRejectsUnrepresentableSlowQueryDiagnostics) {
	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService service(backend);
	const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

	service.recordQueryExecution(maximum, true);
	service.recordQueryExecution(1U, false);
	EXPECT_EQ(service.statistics(),
			  (std::vector<LogicalGraphRuntimeStatistic>{{"query", "completed", PropertyValue(int64_t{2})},
											 {"query", "succeeded", PropertyValue(int64_t{1})},
											 {"query", "failed", PropertyValue(int64_t{1})},
											 {"query", "aggregate_elapsed_ms", PropertyValue(std::numeric_limits<int64_t>::max())}}));

	service.set("query.slow_log.enabled", PropertyValue(true));
	service.set("query.slow_log.threshold_ms", PropertyValue(int64_t{0}));
	service.recordSlowQuery("", 0U, true);
	service.recordSlowQuery("Project", maximum + 1U, true);
	EXPECT_TRUE(service.slowQueries().empty());
}

TEST(NativeConfigurationServiceTest, AppliesDurableCompactionPolicyOnlyToAnAvailableMaintenanceOwner) {
	auto unavailable = std::make_shared<InMemoryAdministrationBackend>();
	NativeConfigurationService unavailableService(unavailable);
	EXPECT_THROW(unavailableService.set("storage.compaction.enabled", PropertyValue(false)), std::invalid_argument);
	EXPECT_EQ(unavailable->writeAttempts(), 0U);

	auto backend = std::make_shared<InMemoryAdministrationBackend>();
	backend->setMaintenanceAvailable(true);
	backend->setPersisted("storage.compaction.enabled", PropertyValue(false));
	NativeConfigurationService service(backend);
	EXPECT_EQ(service.get("storage.compaction.enabled"),
			  (LogicalGraphConfiguration{"storage.compaction.enabled", PropertyValue(false)}));
	EXPECT_FALSE(backend->nativeMaintenanceEnabled());
	EXPECT_EQ(backend->findConfiguration("storage.compaction.enabled"), PropertyValue(false));
	service.set("storage.compaction.enabled", PropertyValue(true));
	EXPECT_TRUE(backend->nativeMaintenanceEnabled());
	EXPECT_THROW(service.set("storage.compaction.enabled", PropertyValue(int64_t{1})), std::invalid_argument);

	backend->setRejectWrites(true);
	EXPECT_THROW(service.set("storage.compaction.enabled", PropertyValue(false)), std::runtime_error);
	EXPECT_TRUE(backend->nativeMaintenanceEnabled());
	EXPECT_EQ(backend->findConfiguration("storage.compaction.enabled"), PropertyValue(true));
}

TEST(NativeConfigurationServiceTest, ReportsMaintenanceLifecycleFailuresWithoutPublishingAnUnsafeDisable) {
	{
		auto backend = std::make_shared<InMemoryAdministrationBackend>();
		backend->setMaintenanceAvailable(true);
		backend->setPersisted("storage.compaction.enabled", PropertyValue(false));
		NativeConfigurationService service(backend);
		backend->setRejectMaintenanceEnable(true);

		EXPECT_THROW(service.set("storage.compaction.enabled", PropertyValue(true)), std::runtime_error);
		EXPECT_EQ(backend->findConfiguration("storage.compaction.enabled"), PropertyValue(true));
		EXPECT_FALSE(backend->nativeMaintenanceEnabled());
	}

	{
		auto backend = std::make_shared<InMemoryAdministrationBackend>();
		backend->setMaintenanceAvailable(true);
		backend->setPersisted("storage.compaction.enabled", PropertyValue(true));
		NativeConfigurationService service(backend);
		backend->setRejectWrites(true);
		backend->setRejectMaintenanceEnable(true);

		EXPECT_THROW(service.set("storage.compaction.enabled", PropertyValue(false)), std::runtime_error);
		EXPECT_EQ(backend->findConfiguration("storage.compaction.enabled"), PropertyValue(true));
		EXPECT_FALSE(backend->nativeMaintenanceEnabled());
	}
}
