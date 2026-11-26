#pragma once
#include <string>
#include <cstdint>
namespace graph::storage {
	// Optional backend capability obtained from StorageBackendFactory. Checks the
	// latest confirmed generation on this open handle, including its reachable
	// dependencies. Unconfirmed tails, retired roots and private writes are
	// outside that generation. No files are repaired or checkpointed.
	struct NativeWalIntegrityReport {
		uint64_t generation = 0, base_generation = 0;
		uint64_t embedded_transactions = 0, wal_transactions = 0, confirmed_wal_bytes = 0;
		uint64_t nodes = 0, edges = 0, unique_constraints = 0;
		bool has_declared_column = false, base_column_complete = false;
	};
	class NativeWalIntegrityBackend {
	public:
		virtual ~NativeWalIntegrityBackend() = default;
		[[nodiscard]] virtual NativeWalIntegrityReport verifyIntegrity() const = 0;
	};

	// Creates a new private directory with a native experimental main file.
	// Open the returned main path through the ordinary Database interface.
	std::string createNativeWalDatabase(const std::string &newDirectory);
	// Declares one non-unique integer acceleration path for matching nodes.
	std::string createIndexedNativeWalDatabase(const std::string &newDirectory, const std::string &label,
											   const std::string &property);
} // namespace graph::storage
