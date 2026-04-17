/**
 * @file QueryGuard.hpp
 * @brief Cooperative query safety guard for timeout and memory limits.
 **/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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
	explicit QueryMemoryExceededException(int64_t limitMb, size_t actualBytes)
		: std::runtime_error("Query exceeded memory limit of " + std::to_string(limitMb) +
		                     "MB (current RSS: " + std::to_string(actualBytes / (1024 * 1024)) + "MB)") {}
};

class QueryGuard {
public:
	QueryGuard(int64_t timeoutMs, int64_t memoryLimitMb);

	inline void check() {
		if (ZYX_LIKELY((++counter_ & CHECK_MASK) != 0)) return;
		checkSlow();
	}

	static size_t getCurrentRSSBytes();

private:
	void checkSlow();

	std::chrono::steady_clock::time_point deadline_;
	int64_t timeoutMs_ = 0;
	int64_t memoryLimitMb_ = 0;
	size_t memoryLimitBytes_ = 0;
	uint32_t counter_ = 0;
	static constexpr uint32_t CHECK_MASK = 1023;
	bool timeoutEnabled_ = false;
	bool memoryEnabled_ = false;
};

} // namespace graph::query
