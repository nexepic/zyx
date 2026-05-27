#pragma once

#include <string>

#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	RecordBatch materializeNodeRecords(const NodeColumnBatch &batch,
	                                   const std::string &variable,
	                                   storage::DataManager &dm,
	                                   const NodeScanRequirements &requirements);

} // namespace graph::query::execution
