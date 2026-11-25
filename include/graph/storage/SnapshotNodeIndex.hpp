#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace graph::storage {
	// Only a declaration resolved in the same pinned data snapshot is exportable.
	struct SnapshotNodeIndex {
		uint64_t id, generation;
		std::string label, property;
	};
	class SnapshotNodeIndexSource {
	public:
		virtual ~SnapshotNodeIndexSource() = default;
		virtual std::optional<SnapshotNodeIndex> singleNodePropertyIndex() const = 0;
	};
} // namespace graph::storage
