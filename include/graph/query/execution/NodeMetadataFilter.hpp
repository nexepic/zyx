#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class NodeMetadataRowFilter {
	public:
		NodeMetadataRowFilter(const std::shared_ptr<storage::DataManager> &dm,
							  const NodeScanConfig &config,
							  NodeScanRequirements requirements) :
			requirements_(requirements) {
			if (!dm || !requirements_.needsLabels) {
				return;
			}
			labelIds_.reserve(config.labels.size());
			for (const auto &label : config.labels) {
				const int64_t labelId = dm->resolveTokenId(label);
				labelIds_.push_back(labelId == 0 ? -1 : labelId);
			}
		}

		[[nodiscard]] bool accepts(const NodeMetadataBatch &batch, size_t row) const {
			if (!batch.isValid(row)) {
				return false;
			}
			if (requirements_.needsActiveCheck && batch.active[row] == 0) {
				return false;
			}
			for (const int64_t labelId : labelIds_) {
				if (!batch.hasLabelId(row, labelId)) {
					return false;
				}
			}
			return true;
		}

	private:
		NodeScanRequirements requirements_;
		std::vector<int64_t> labelIds_;
	};

} // namespace graph::query::execution
