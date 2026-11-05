/**
 * @file QueryGuard.cpp
 * @brief Implementation of cooperative query safety guard.
 **/

#include "graph/query/QueryGuard.hpp"
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
#include "graph/debug/SanitizerAllocationSnapshot.hpp"
#include <cstdio>
#include <cstdlib>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace graph::query {

QueryGuard::QueryGuard(int64_t timeoutMs, int64_t memoryLimitMb, MemoryUsageSampler memoryUsageSampler)
	: timeoutMs_(timeoutMs), memoryLimitMb_(memoryLimitMb), memoryUsageSampler_(std::move(memoryUsageSampler)) {
	if (timeoutMs > 0) {
		timeoutEnabled_ = true;
		deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	}
	if (memoryLimitMb > 0) {
		memoryEnabled_ = true;
		memoryLimitBytes_ = static_cast<size_t>(memoryLimitMb) * 1024 * 1024;
		baselineResidentBytes_ = currentResidentBytes();
	}
}

void QueryGuard::checkSlow() {
	if (timeoutEnabled_) {
		if (std::chrono::steady_clock::now() >= deadline_) {
			throw QueryTimeoutException(timeoutMs_);
		}
	}
	if (memoryEnabled_) {
		const size_t currentBytes = currentResidentBytes();
		if (currentBytes > baselineResidentBytes_ && currentBytes - baselineResidentBytes_ > memoryLimitBytes_) {
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
            if (std::getenv("ZYX_CI_RSS_TRACE")) {
                const auto allocator = debug::captureSanitizerAllocations();
                std::fprintf(stderr, "CI_RSS_REJECT limit=%zu baseline=%zu current=%zu asan_available=%d live=%zu heap=%zu\n",
                    memoryLimitBytes_, baselineResidentBytes_, currentBytes, allocator.available,
                    allocator.live_bytes, allocator.heap_bytes);
            }
#endif
			throw QueryMemoryExceededException(memoryLimitMb_, baselineResidentBytes_, currentBytes);
		}
	}
}

void QueryGuard::checkNow() { checkSlow(); }

size_t QueryGuard::currentResidentBytes() const {
	return memoryUsageSampler_ ? memoryUsageSampler_() : getCurrentRSSBytes();
}

size_t QueryGuard::getCurrentRSSBytes() {
#if defined(__APPLE__)
	mach_task_basic_info_data_t info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	                             reinterpret_cast<task_info_t>(&info), &count);
	if (kr == KERN_SUCCESS) { // ZYX_COV_EXCL_LINE: macOS kernel API failure cannot be induced portably.
		return info.resident_size;
	}
	return 0;
#elif defined(__linux__)
    // Sampling must not itself grow the heap (or ASan's quarantine).
    // /proc exposes only counters; fixed local storage is sufficient for both
    // page counts, even at their size_t maxima.
    char data[128];
    int fd;
    do { fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return 0;
    size_t length = 0U;
    while (length < sizeof(data)) {
        ssize_t readBytes;
        do { readBytes = ::read(fd, data + length, sizeof(data) - length); } while (readBytes < 0 && errno == EINTR);
        if (readBytes <= 0) break;
        const bool complete = std::memchr(data + length, '\n', static_cast<size_t>(readBytes)) != nullptr;
        length += static_cast<size_t>(readBytes);
        if (complete) break;
    }
    ::close(fd); // Linux closes the descriptor even if close reports EINTR.
    if (length == 0U) return 0;
    size_t total = 0U, resident = 0U;
    const auto first = std::from_chars(data, data + length, total);
    if (first.ec != std::errc{}) return 0;
    auto start = first.ptr;
    while (start < data + length && *start == ' ') ++start;
    const auto second = std::from_chars(start, data + length, resident);
    const long pageBytes = ::sysconf(_SC_PAGESIZE);
    if (second.ec != std::errc{} || pageBytes <= 0 ||
        resident > (std::numeric_limits<size_t>::max)() / static_cast<size_t>(pageBytes)) return 0;
    return resident * static_cast<size_t>(pageBytes);
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
