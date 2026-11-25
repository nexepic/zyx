// Export into a new, private binary stream. Check close before publication.
#pragma once
#include <iosfwd>
#include "graph/storage/LogicalGraphRead.hpp"
namespace graph::storage {
	void exportIndexedTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
	void exportTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out, bool columns);
	void exportSegmentedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
}
