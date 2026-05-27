#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"

namespace graph::query::execution {

	struct NodeColumnBatch {
		std::vector<int64_t> nodeIds;
		std::vector<uint8_t> selected;
		std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> propertyColumns;
		std::vector<Node> materializedNodes;

		[[nodiscard]] size_t size() const {
			return nodeIds.size();
		}

		[[nodiscard]] bool isSelected(size_t index) const {
			return selected.empty() || selected[index] != 0;
		}

		void ensureSelectionVector() {
			if (selected.empty()) {
				selected.assign(nodeIds.size(), 1);
			}
		}

		[[nodiscard]] size_t selectedCount() const {
			if (selected.empty()) {
				return nodeIds.size();
			}
			return static_cast<size_t>(std::count_if(selected.begin(), selected.end(), [](uint8_t value) {
				return value != 0;
			}));
		}

		[[nodiscard]] bool hasPropertyColumn(const std::string &key) const {
			return propertyColumns.find(key) != propertyColumns.end();
		}
	};

} // namespace graph::query::execution
