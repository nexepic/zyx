#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace graph::storage {
// A query-only internal permission. No file path, allocator or physical identity
// crosses this interface. A logical read may hold it on a writable native file;
// physically read-only files never provide this capability.
// Sessions are task-confined. Returned read bytes own their storage; callers
// must quiesce private readers before releasing handles or finishing the owner.
class LogicalQueryScratchSession {
public:
	virtual ~LogicalQueryScratchSession() = default;
	[[nodiscard]] virtual uint64_t write(std::span<const std::byte>) = 0;
	[[nodiscard]] virtual std::vector<std::byte> read(uint64_t) = 0;
	virtual void release(uint64_t) = 0;
	virtual void finish() = 0;
	[[nodiscard]] virtual size_t maximumFragmentBytes() const = 0;
};
class LogicalQueryScratchProvider {
public:
	virtual ~LogicalQueryScratchProvider() = default;
	[[nodiscard]] virtual bool queryScratchEnabled() const {return true;}
	[[nodiscard]] virtual std::unique_ptr<LogicalQueryScratchSession> openQueryScratchSession(size_t maximumDiskBytes,size_t maximumFragments) const = 0;
};
}
