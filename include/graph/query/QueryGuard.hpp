/**
 * @file QueryGuard.hpp
 * @brief Cooperative query safety guard for timeout and memory limits.
 **/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#define ZYX_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define ZYX_LIKELY(x) (x)
#endif

namespace graph::query {

class QueryTimeoutException : public std::runtime_error {
public:
	explicit QueryTimeoutException(int64_t timeoutMs)
		: std::runtime_error("Query exceeded timeout of " + std::to_string(timeoutMs) + "ms") {}
};

class QueryMemoryExceededException : public std::runtime_error {
public:
	explicit QueryMemoryExceededException(int64_t limitMb, size_t baselineBytes, size_t currentBytes)
		: std::runtime_error("Query exceeded memory limit of " + std::to_string(limitMb) +
		                     "MB (query resident growth: " +
		                     std::to_string((currentBytes - baselineBytes) / (1024 * 1024)) + "MB, baseline RSS: " +
		                     std::to_string(baselineBytes / (1024 * 1024)) + "MB, current RSS: " +
		                     std::to_string(currentBytes / (1024 * 1024)) + "MB)") {}
};

class QueryGuard {
public:
	using MemoryUsageSampler = std::function<size_t()>;

	// The memory policy constrains resident-set growth that begins with this
	// query, never the process memory already required to host the database.
	// An optional sampler makes the policy independently testable and lets an
	// embedding provide an equivalent platform-specific resident metric.
	QueryGuard(int64_t timeoutMs, int64_t memoryLimitMb, MemoryUsageSampler memoryUsageSampler = {});

	inline void check() {
		if (ZYX_LIKELY((++counter_ & CHECK_MASK) != 0)) return;
		checkSlow();
	}

	// Durable publication points cannot defer a timeout check to the next
	// periodic sample. They use this immediate variant before committing.
	void checkNow();

	static size_t getCurrentRSSBytes();

private:
	void checkSlow();
	[[nodiscard]] size_t currentResidentBytes() const;

	std::chrono::steady_clock::time_point deadline_;
	int64_t timeoutMs_ = 0;
	int64_t memoryLimitMb_ = 0;
	size_t memoryLimitBytes_ = 0;
	size_t baselineResidentBytes_ = 0;
	MemoryUsageSampler memoryUsageSampler_;
	uint32_t counter_ = 0;
	static constexpr uint32_t CHECK_MASK = 1023;
	bool timeoutEnabled_ = false;
	bool memoryEnabled_ = false;
};

} // namespace graph::query
