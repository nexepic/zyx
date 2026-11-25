#pragma once
#include <mutex>
#include "State.hpp"
namespace graph::storage::nativewal {
	// An exclusively opened directory is the writer identity. Only immutable,
	// validated snapshots escape the gate. Old versions live as long as their readers.
	class Journal {
		struct Files;
		std::unique_ptr<Files> files;
		Digest baseDigest{};
		size_t walLength = 0;
		utils::Sha256 walHash;
		uint64_t baseSequence = 0;
		bool readOnly, poisoned = false, needsStabilization = true;
		std::mutex gate;
		std::shared_ptr<const State> current;
		void checkpointLocked();

	public:
		Journal(const std::string &, bool, size_t memory);
		~Journal();
		static std::string create(const std::string &);
		std::shared_ptr<const State> snapshot();
		void commit(const std::shared_ptr<const State> &expected, std::shared_ptr<State> next, const Changes &);
		void checkpoint();
	};
} // namespace graph::storage::nativewal
