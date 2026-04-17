/**
 * @file QueryGuard.cpp
 * @brief Implementation of cooperative query safety guard.
 **/

#include "graph/query/QueryGuard.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace graph::query {

QueryGuard::QueryGuard(int64_t timeoutMs, int64_t memoryLimitMb)
	: timeoutMs_(timeoutMs), memoryLimitMb_(memoryLimitMb) {
	if (timeoutMs > 0) {
		timeoutEnabled_ = true;
		deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	}
	if (memoryLimitMb > 0) {
		memoryEnabled_ = true;
		memoryLimitBytes_ = static_cast<size_t>(memoryLimitMb) * 1024 * 1024;
	}
}

void QueryGuard::checkSlow() {
	if (timeoutEnabled_) {
		if (std::chrono::steady_clock::now() >= deadline_) {
			throw QueryTimeoutException(timeoutMs_);
		}
	}
	if (memoryEnabled_) {
		size_t rss = getCurrentRSSBytes();
		if (rss > memoryLimitBytes_) {
			throw QueryMemoryExceededException(memoryLimitMb_, rss);
		}
	}
}

size_t QueryGuard::getCurrentRSSBytes() {
#if defined(__APPLE__)
	mach_task_basic_info_data_t info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	                             reinterpret_cast<task_info_t>(&info), &count);
	if (kr == KERN_SUCCESS) {
		return info.resident_size;
	}
	return 0;
#elif defined(__linux__)
	std::ifstream statm("/proc/self/statm");
	if (statm.is_open()) {
		size_t size = 0, resident = 0;
		statm >> size >> resident;
		return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
	}
	return 0;
#elif defined(_WIN32)
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		return pmc.WorkingSetSize;
	}
	return 0;
#else
	return 0;
#endif
}

} // namespace graph::query
