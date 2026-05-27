#pragma once

#include <string>
#include <vector>

namespace graph::query::execution {

	enum class NodeMaterializationMode {
		NSM_ID_ONLY,
		NSM_SELECTED_PROPERTIES,
		NSM_FULL_NODE
	};

	struct NodeScanRequirements {
		NodeMaterializationMode materialization = NodeMaterializationMode::NSM_FULL_NODE;
		std::vector<std::string> requiredProperties;
		bool needsLabels = true;
		bool needsActiveCheck = true;
		bool countOnly = false;

		[[nodiscard]] bool needsFullNode() const {
			return materialization == NodeMaterializationMode::NSM_FULL_NODE;
		}

		[[nodiscard]] bool needsProperties() const {
			return !requiredProperties.empty() || needsFullNode();
		}
	};

} // namespace graph::query::execution
