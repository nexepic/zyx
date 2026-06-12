#pragma once

#include <cstddef>
#include <cstdint>

namespace graph::storage {
	namespace {
		struct PropertyEntityRowRef {
			int64_t id = 0;
			size_t row = 0;
		};

		struct PropertyEntitySegmentWork {
			size_t segmentIndex = 0;
			size_t idBegin = 0;
			size_t idEnd = 0;
		};

	} // namespace
} // namespace graph::storage
