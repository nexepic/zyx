#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace zyx {

	class Database;
	class Transaction;

	namespace detail {

		// Columnar ingest is an API-boundary representation. Keeping it here
		// prevents the logical native path from depending on a retired physical
		// storage subsystem solely for a value container.
		struct BulkPropertyColumn {
			std::string key;
			std::vector<graph::PropertyValue> values;
		};

		/**
		 * Internal bridge shared by the public C++ columnar API and Driver ABI.
		 *
		 * Keeping this outside the installed API surface lets the Arrow adapter pass
		 * decoded internal columns without materializing a second public Value graph.
		 * The logical backend converts each bounded batch into transaction-local
		 * requests before publication. All input remains synchronously borrowed by
		 * the caller of this bridge.
		 */
		class DatabaseBulkInternal final {
		public:
			// The Driver ABI owns a transaction for one ingest session. Keep its
			// columnar batches inside that transaction rather than auto-committing
			// through the database-level logical write adapter.
			[[nodiscard]] static std::vector<int64_t>
			createEdgesColumnar(const Database &database, Transaction &transaction, const std::string &edgeType,
								const std::vector<int64_t> &sourceIds, const std::vector<int64_t> &targetIds,
								const std::vector<BulkPropertyColumn> &columns);
		};

	} // namespace detail
} // namespace zyx
