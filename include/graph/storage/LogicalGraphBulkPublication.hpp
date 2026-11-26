#pragma once
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::storage {
	// Optional for backends whose ordinary commit requires a separate log.
	class LogicalGraphBulkPublication {
	public:
		virtual ~LogicalGraphBulkPublication() = default;
		virtual std::unique_ptr<LogicalGraphTransaction> beginStandaloneTransaction() = 0;
	};
}
