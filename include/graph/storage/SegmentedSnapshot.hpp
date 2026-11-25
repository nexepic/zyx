// Export into a new, private binary stream. Check close before publication.
#pragma once
#include <iosfwd>
#include "graph/storage/StorageBackend.hpp"
#include "zyx/published_snapshot.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
namespace graph::storage {
	std::array<std::byte, 32> publishTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
	std::shared_ptr<StorageBackend> openPublishedSegmentedBackend(const std::string &path, size_t bytes,
																 const zyx::PublishedSnapshot &witness);
	void exportIndexedTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
	void exportTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out, bool columns);
	void exportSegmentedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
}
