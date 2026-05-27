#include "graph/query/execution/NodeColumnBatchMaterializer.hpp"

#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		Node loadNodeForMaterialization(int64_t nodeId,
		                                storage::DataManager &dm,
		                                const NodeColumnBatch &batch,
		                                size_t rowIndex,
		                                const NodeScanRequirements &requirements) {
			Node node = dm.getNode(nodeId);

			if (requirements.needsFullNode()) {
				node.setProperties(dm.getNodeProperties(nodeId));
				return node;
			}

			for (const auto &[key, column] : batch.propertyColumns) {
				if (rowIndex < column.size() && column[rowIndex].has_value()) {
					node.addProperty(key, *column[rowIndex]);
				}
			}
			return node;
		}
	} // namespace

	RecordBatch materializeNodeRecords(const NodeColumnBatch &batch,
	                                   const std::string &variable,
	                                   storage::DataManager &dm,
	                                   const NodeScanRequirements &requirements) {
		const auto traceStart = Clock::now();

		RecordBatch records;
		records.reserve(batch.selectedCount());

		size_t materializedIndex = 0;
		for (size_t rowIndex = 0; rowIndex < batch.nodeIds.size(); ++rowIndex) {
			if (!batch.isSelected(rowIndex)) {
				continue;
			}

			Node node;
			if (requirements.needsFullNode() && materializedIndex < batch.materializedNodes.size()) {
				node = batch.materializedNodes[materializedIndex];
			} else {
				node = loadNodeForMaterialization(batch.nodeIds[rowIndex], dm, batch, rowIndex, requirements);
			}
			++materializedIndex;

			Record record;
			record.setNode(variable, node);
			records.push_back(std::move(record));
		}

		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration("node_scan.materialize", elapsedNs(traceStart));
		}

		return records;
	}

} // namespace graph::query::execution
