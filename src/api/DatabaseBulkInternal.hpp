#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "graph/storage/data/ColumnarBulkInput.hpp"

namespace zyx {

	class Database;

	namespace detail {

		/**
		 * Internal bridge shared by the public C++ columnar API and Driver ABI.
		 *
		 * Keeping this outside the installed API surface lets the Arrow adapter pass
		 * storage-native columns without first materializing a second public Value
		 * graph. All input remains synchronously borrowed by the caller of this
		 * bridge.
		 */
		class DatabaseBulkInternal final {
		public:
			[[nodiscard]] static std::vector<int64_t>
			createEdgesColumnar(const Database &database, const std::string &edgeType,
								const std::vector<int64_t> &sourceIds, const std::vector<int64_t> &targetIds,
								const std::vector<graph::storage::BulkPropertyColumn> &columns);
		};

	} // namespace detail
} // namespace zyx
